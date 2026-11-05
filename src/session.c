#include "session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *js(const char *buf, const char *key, char *out, int n){
    char pat[64]; snprintf(pat,sizeof pat,"\"%s\":",key);
    const char *p=strstr(buf,pat); if(!p) return NULL;
    p+=strlen(pat); while(*p==' ') p++;
    if(*p=='"'){ p++; const char *e=strchr(p,'"'); if(!e) return NULL; int l=e-p; if(l>=n)l=n-1; memcpy(out,p,l); out[l]=0; }
    else { const char *e=p; while(*e && *e!=',' && *e!='}') e++; int l=e-p; if(l>=n)l=n-1; memcpy(out,p,l); out[l]=0; }
    return out;
}

int session_load(const char *dir, session_t *s){
    memset(s,0,sizeof *s);
    snprintf(s->dir,sizeof s->dir,"%s",dir);
    snprintf(s->state,sizeof s->state,"idle");
    s->rate=48000; s->ch=2;
    char path[512]; snprintf(path,sizeof path,"%s/session.json",dir);
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    char buf[2048]; size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
    char tmp[512];
    if(js(buf,"state",tmp,sizeof tmp)) snprintf(s->state,sizeof s->state,"%s",tmp);
    if(js(buf,"path",tmp,sizeof tmp)) snprintf(s->path,sizeof s->path,"%s",tmp);
    if(js(buf,"seq",tmp,sizeof tmp)) s->seq=(unsigned)strtoul(tmp,NULL,10);
    if(js(buf,"frames",tmp,sizeof tmp)) s->frames=strtoull(tmp,NULL,10);
    if(js(buf,"rate",tmp,sizeof tmp)) s->rate=(uint32_t)strtoul(tmp,NULL,10);
    if(js(buf,"ch",tmp,sizeof tmp)) s->ch=atoi(tmp);
    if(js(buf,"rtc_start",tmp,sizeof tmp)) s->rtc_start=(uint32_t)strtoul(tmp,NULL,10);
    if(js(buf,"ms_start",tmp,sizeof tmp)) s->ms_start=strtoll(tmp,NULL,10);
    return 0;
}

int session_save(const session_t *s){
    char path[512], tmp[520];
    snprintf(path,sizeof path,"%s/session.json",s->dir);
    snprintf(tmp,sizeof tmp,"%s.tmp",path);
    FILE *f=fopen(tmp,"wb"); if(!f) return -1;
    fprintf(f,"{\"state\":\"%s\",\"seq\":%u,\"path\":\"%s\",\"frames\":%llu,\"rate\":%u,\"ch\":%d,"
              "\"rtc_start\":%u,\"ms_start\":%lld,\"recovered\":%d}\n",
            s->state,s->seq,s->path,(unsigned long long)s->frames,s->rate,s->ch,
            s->rtc_start,(long long)s->ms_start,s->recovered);
    fflush(f); fsync(fileno(f)); fclose(f);
    return rename(tmp,path);
}
