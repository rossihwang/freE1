#include "http.h"
#include "common.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/time.h>
#include <poll.h>

static void handle_conn(int fd);
static void *conn_thread(void *arg);
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>

static pthread_t g_th;
static volatile int g_run = 0;
static int g_port = 0;
static char g_root[400];
static int g_lfd = -1;
static volatile int g_del_seq = 0;   /* 每次 HTTP 删除 +1，UI 据此刷新文件列表 */
static void add_deleted(const char *name){ (void)name; g_del_seq++; }
int http_delete_seq(void){ return g_del_seq; }

/* ---------- 删除令牌 ----------
   删除只接受 POST，并且必须带页面上渲染出来的一次性令牌：GET 会被浏览器预取、
   刷新重放、爬虫扫到都可能触发删除；没有令牌的话局域网上任何页面都能悄悄提交。 */
#define TOKENS 8
static char g_tok[TOKENS][17];
static int  g_tok_next = 0;
static int  g_tok_seeded = 0;

static void token_seed(void){
    if(g_tok_seeded) return;
    g_tok_seeded = 1;
    srand((unsigned)(time(NULL) ^ (getpid() << 16)));
}

static const char *token_new(void){
    token_seed();
    char *t = g_tok[g_tok_next];
    g_tok_next = (g_tok_next + 1) % TOKENS;
    for(int i = 0; i < 16; i++) t[i] = "0123456789abcdef"[rand() & 15];
    t[16] = 0;
    return t;
}

static int token_ok(const char *t){
    if(!t || strlen(t) != 16) return 0;
    for(int i = 0; i < TOKENS; i++) if(!strcmp(g_tok[i], t)) return 1;
    return 0;
}

static int send_all(int fd, const char *buf, size_t n){
    size_t off=0;
    while(off<n){ ssize_t w=send(fd,buf+off,n-off,0); if(w<=0) return -1; off+=w; }
    return 0;
}

static void send_err(int fd, int code, const char *msg){
    char b[256];
    int n=snprintf(b,sizeof b,"HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                   code, code==404?"Not Found":"Error", strlen(msg), msg);
    send_all(fd,b,n);
}


static void redirect(int fd, const char *loc){
    char b[256];
    int n=snprintf(b,sizeof b,"HTTP/1.1 303 See Other\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", loc);
    send_all(fd,b,n);
}

/* HTTP 服务没有认证，能连上就能删文件；所以只允许删"当前目录里真实存在的 .wav"：
   不接受路径分隔符、不接受 ..、必须以 .wav 结尾。 */
static int safe_wav_name(const char *n){
    if(!n || !n[0] || strlen(n) > 200) return 0;
    if(strchr(n,'/') || strstr(n,"..")) return 0;
    size_t l = strlen(n);
    if(l < 5 || strcmp(n + l - 4, ".wav") != 0) return 0;
    return 1;
}

static void url_decode(char *s){
    char *o = s;
    for(char *p = s; *p; p++){
        if(*p == '+'){ *o++ = ' '; }
        else if(*p == '%' && p[1] && p[2]){
            char h[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else *o++ = *p;
    }
    *o = 0;
}

static int del_one(const char *name){
    if(!safe_wav_name(name)) return 0;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", g_root, name);
    struct stat st;
    if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if(unlink(path) != 0) return 0;
    char mk[710];
    snprintf(mk, sizeof mk, "%s.marks", path);
    unlink(mk);                     /* 标记 sidecar 一起删 */
    add_deleted(name);
    logmsg("http: deleted %s", name);
    return 1;
}

/* POST /delete   body: csrf=<token>&f=a.wav&f=b.wav（也支持 f=a,b） */
static void do_delete(int fd, const char *query, const char *body){
    int done = 0, fail = 0;
    const char *src = body ? body : query;
    if(body){
        /* 令牌校验：从 body 里取 csrf= */
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s", body);
        char *cs = strstr(tmp, "csrf=");
        char tok[64] = {0};
        if(cs){
            cs += 5;
            int k = 0;
            while(cs[k] && cs[k] != '&' && k < (int)sizeof tok - 1){ tok[k] = cs[k]; k++; }
            tok[k] = 0;
        }
        if(!token_ok(tok)){
            logmsg("http: delete rejected (bad token)");
            send_err(fd, 403, "bad token");
            return;
        }
    }
    if(src){
        char q[4096];
        snprintf(q, sizeof q, "%s", src);
        char *save = NULL;
        for(char *kv = strtok_r(q, "&", &save); kv; kv = strtok_r(NULL, "&", &save)){
            if(strncmp(kv, "f=", 2)) continue;
            char *v = kv + 2;
            url_decode(v);
            char *sv = NULL;
            for(char *one = strtok_r(v, ",", &sv); one; one = strtok_r(NULL, ",", &sv)){
                if(del_one(one)) done++; else fail++;
            }
        }
    }
    sync();                         /* 别让删除只停在页缓存里 */
    char loc[128];
    snprintf(loc, sizeof loc, "/?del=%d&fail=%d", done, fail);
    redirect(fd, loc);
}

static void list_html(int fd, const char *root, const char *query){
    int del = 0, fail = 0;
    if(query){
        const char *p = strstr(query, "del=");  if(p) del = atoi(p + 4);
        p = strstr(query, "fail=");             if(p) fail = atoi(p + 4);
    }
    char head[512]; char *body = malloc(65536); size_t off = 0;
    if(!body){ send_err(fd,500,"oom"); return; }
    off += snprintf(body+off,65536-off,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>freE1</title><style>"
        "body{background:#0b0d12;color:#e8e8e9;font:14px/1.5 monospace;margin:16px}"
        "a{color:#3cff7a;text-decoration:none}"
        "table{border-collapse:collapse;width:100%%}"
        "td,th{padding:8px 4px;border-bottom:1px solid #222}"
        "th{color:#8a8f98;font-weight:normal;text-align:left}"
        ".sz{color:#8a8f98;white-space:nowrap}"
        "button{background:#2e7d50;color:#08180f;border:0;border-radius:8px;padding:12px 18px;font:15px monospace}"
        "button.del{background:#c0392b;color:#fff}"
        ".msg{padding:8px 12px;border-radius:8px;margin-bottom:12px;background:#16241c;color:#3cff7a}"
        "input[type=checkbox]{width:20px;height:20px}"
        "</style></head><body><h3>freE1 recordings</h3>");

    if(del || fail){
        off += snprintf(body+off,65536-off,"<div class=msg>已删除 %d 个文件%s</div>",
                        del, fail ? "，部分被跳过（名字非法或已不存在）" : "");
    }

    off += snprintf(body+off,65536-off,
        "<form method=POST action=/delete onsubmit=\"return confirm('删除选中的文件？不可恢复')\">"
        "<table><tr>"
        "<th><input type=checkbox id=all onclick=\"for(const c of document.querySelectorAll('input.cb'))c.checked=this.checked\"></th>"
        "<th>文件</th><th class=sz>大小</th><th class=sz>标记</th></tr>"
        "<input type=hidden name=csrf value=\"%s\">", token_new());

    DIR *d = opendir(root);
    if(d){
        static char names[160][200];
        int nn = 0;
        struct dirent *e;
        while((e=readdir(d)) && nn < 160){
            if(!safe_wav_name(e->d_name)) continue;
            snprintf(names[nn++], 200, "%s", e->d_name);
        }
        closedir(d);
        for(int a = 0; a < nn; a++)          /* 新的放前面 */
            for(int b = a + 1; b < nn; b++)
                if(strcmp(names[a], names[b]) < 0){
                    char t[200]; snprintf(t,sizeof t,"%s",names[a]);
                    snprintf(names[a],200,"%s",names[b]); snprintf(names[b],200,"%s",t);
                }
        for(int i = 0; i < nn && off < 65536 - 600; i++){
            char path[700]; struct stat st;
            snprintf(path, sizeof path, "%s/%s", root, names[i]);
            double mb = (stat(path, &st) == 0) ? st.st_size / 1048576.0 : 0.0;
            char mk[710]; struct stat mst;
            snprintf(mk, sizeof mk, "%s.marks", path);
            int has = (stat(mk, &mst) == 0 && mst.st_size > 0);
            off += snprintf(body+off,65536-off,
                "<tr><td><input class=cb type=checkbox name=f value=\"%s\"></td>"
                "<td><a href=\"/%s\">%s</a></td><td class=sz>%.1f MB</td><td class=sz>%s</td></tr>",
                names[i], names[i], names[i], mb, has ? "MARK" : "");
        }
    }
    off += snprintf(body+off,65536-off,
        "</table><p><button class=del type=submit>删除选中</button></p></form>"
        "<p class=sz>只列出并可删除本目录下的 .wav；删除会连同 .marks 一起删。"
        "HTTP 服务没有认证，请在可信网络内使用。</p></body></html>");
    int n=snprintf(head,sizeof head,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", off);
    send_all(fd,head,n); send_all(fd,body,off);
    free(body);
}

static void serve_file(int fd, const char *root, const char *name, const char *range){
    if(strstr(name,"..") || name[0]=='/'){ send_err(fd,404,"bad path"); return; }
    char path[600]; snprintf(path,sizeof path,"%s/%s",root,name);
    FILE *f=fopen(path,"rb");
    if(!f){ send_err(fd,404,"not found"); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    long start=0, end=sz-1; int partial=0;
    if(range){
        const char *p=strstr(range,"bytes=");
        if(p){ p+=6; start=strtol(p,(char**)&p,10); if(*p=='-'){ end=strtol(p+1,NULL,10); if(end<=0||end>=sz) end=sz-1; } partial=1; }
    }
    long len=end-start+1; if(len<0) len=0;
    char hdr[512];
    int n = partial
      ? snprintf(hdr,sizeof hdr,"HTTP/1.1 206 Partial Content\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\n"
                  "Content-Range: bytes %ld-%ld/%ld\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                  start,end,sz,len)
      : snprintf(hdr,sizeof hdr,"HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\n"
                  "Content-Length: %ld\r\nConnection: close\r\n\r\n", sz);
    send_all(fd,hdr,n);
    fseek(f,start,SEEK_SET);
    char buf[65536]; long left=len;
    while(left>0){
        size_t want = left<(long)sizeof buf ? (size_t)left : sizeof buf;
        size_t got=fread(buf,1,want,f); if(!got) break;
        if(send_all(fd,buf,got)!=0) break;
        left-=got;
    }
    fclose(f);
}

static void *http_thread(void *arg){
    (void)arg;
    int lfd=socket(AF_INET,SOCK_STREAM,0);
    if(lfd<0){ logmsg("http: socket failed"); g_run=0; return NULL; }
    int one=1; setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(g_port);
    if(bind(lfd,(struct sockaddr*)&a,sizeof a)!=0){ logmsg("http: bind %d failed",g_port); close(lfd); g_run=0; return NULL; }
    listen(lfd,16); g_lfd=lfd;
    signal(SIGPIPE, SIG_IGN);   /* 客户端断开不能把进程打死 */
    logmsg("http: serving %s on :%d", g_root, g_port);
    while(g_run){
        struct pollfd pf = { lfd, POLLIN, 0 };
        int pr = poll(&pf, 1, 200);          /* 200ms 超时，g_run=0 时能及时退出 */
        if(!g_run) break;
        if(pr <= 0) continue;
        struct sockaddr_in ca; socklen_t cl=sizeof ca;
        int fd=accept(lfd,(struct sockaddr*)&ca,&cl);
        if(fd<0) continue;
        struct timeval tv = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        pthread_t th;
        if(pthread_create(&th,NULL,conn_thread,(void*)(intptr_t)fd)==0) pthread_detach(th);
        else { handle_conn(fd); }
    }
    close(lfd); g_lfd=-1;
    return NULL;
}

static void handle_conn(int fd){
    /* 请求行 + 头 + 可选的 POST body；最多 32 KB（一次删几十个文件够用） */
    static char req[32768];
    size_t got = 0, hdr_end = 0;
    while(got < sizeof req - 1){
        ssize_t n = recv(fd, req + got, sizeof req - 1 - got, 0);
        if(n <= 0) break;
        got += (size_t)n;
        req[got] = 0;
        char *he = strstr(req, "\r\n\r\n");
        if(!he) he = strstr(req, "\n\n");
        if(he){
            hdr_end = (size_t)(he - req) + (he[0]=='\r' ? 4 : 2);
            /* 头里有 Content-Length 就要把 body 读满 */
            int clen = 0;
            char *cl = strstr(req, "Content-Length:");
            if(cl) clen = atoi(cl + 15);
            if(clen <= 0) break;
            if(hdr_end + (size_t)clen <= got) break;
            if(hdr_end + (size_t)clen > sizeof req - 1) break;   /* 太长：按已读到的处理 */
        }
    }
    if(got == 0){ close(fd); return; }
    req[got] = 0;

    char method[8], rawpath[2048];
    if(sscanf(req, "%7s %2047s", method, rawpath) != 2){ close(fd); return; }

    char *range=NULL;
    { char *r=strstr(req,"Range:"); if(r){ range=r; char *e=strstr(r,"\r\n"); if(e) *e=0; } }

    const char *body = hdr_end && hdr_end < got ? req + hdr_end : NULL;

    char *query = NULL;
    { char *q = strchr(rawpath, '?'); if(q){ *q = 0; query = q + 1; } }

    if(!strcmp(rawpath,"/")) list_html(fd, g_root, query);
    else if(!strcmp(rawpath,"/delete")){
        if(strcmp(method, "POST") != 0){      /* 只接受 POST：GET 一律拒绝 */
            send_err(fd, 405, "use POST");
        } else {
            do_delete(fd, query, body);
        }
    }
    else {
        const char *name=rawpath+1;
        if(!strncmp(name,"files/",6)) name+=6;
        serve_file(fd, g_root, name, range);
    }
    close(fd);
}

static void *conn_thread(void *arg){
    handle_conn((int)(intptr_t)arg);
    return NULL;
}

int http_start(const char *root, int port){
    if(g_run) return 0;
    snprintf(g_root,sizeof g_root,"%s",root);
    g_port=port; g_run=1;
    if(pthread_create(&g_th,NULL,http_thread,NULL)!=0){ g_run=0; return -1; }
    return 0;
}
void http_stop(void){
    if(!g_run) return;
    g_run = 0;
    if(g_lfd >= 0){ shutdown(g_lfd, SHUT_RDWR); close(g_lfd); g_lfd = -1; }
    pthread_join(g_th, NULL);        /* poll 200ms 超时，join 能正常返回 */
}
int http_running(void){ return g_run; }
int http_port(void){ return g_port; }
