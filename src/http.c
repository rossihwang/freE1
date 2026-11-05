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

static void list_html(int fd, const char *root){
    char head[512], body[8192]; size_t off=0;
    off += snprintf(body+off,sizeof body-off,
        "<html><head><meta charset=utf-8><title>freE1</title>"
        "<style>body{background:#0b0d12;color:#e8e8e9;font:14px monospace}a{color:#3cff7a}</style></head><body>"
        "<h3>freE1 recordings</h3><ul>");
    DIR *d=opendir(root);
    if(d){
        struct dirent *e;
        while((e=readdir(d)) && off<sizeof body-256){
            if(strstr(e->d_name,".wav")==NULL) continue;
            off += snprintf(body+off,sizeof body-off,"<li><a href=\"/%s\">%s</a></li>", e->d_name, e->d_name);
        }
        closedir(d);
    }
    off += snprintf(body+off,sizeof body-off,"</ul><p>HTTP export is read-only.</p></body></html>");
    int n=snprintf(head,sizeof head,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", off);
    send_all(fd,head,n); send_all(fd,body,off);
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
    char req[2048]; ssize_t n=recv(fd,req,sizeof req-1,0);
    if(n<=0){ close(fd); return; }
    req[n]=0;
    char method[8], path[512];
    if(sscanf(req,"%7s %511s",method,path)!=2){ close(fd); return; }
    char *range=NULL;
    { char *r=strstr(req,"Range:"); if(r){ range=r; char *e=strstr(r,"\r\n"); if(e) *e=0; } }
    if(!strcmp(path,"/")) list_html(fd,g_root);
    else {
        const char *name=path+1;
        if(!strncmp(name,"files/",6)) name+=6;
        serve_file(fd,g_root,name,range);
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
