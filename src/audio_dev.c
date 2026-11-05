#include "audio.h"
#include "common.h"

typedef struct { FILE *p; int nch; } adev_t;

static int dev_read(void *v, int32_t *buf, int frames){
    adev_t *d=v;
    size_t need=(size_t)frames*d->nch*4;
    size_t got=fread(buf,1,need,d->p);
    if(got==0) return -1;
    return (int)(got/((size_t)d->nch*4));
}
static void dev_close(void *v){
    adev_t *d=v; if(d->p) pclose(d->p); free(d);
}

audio_src *audio_open_arecord(int nch, int rate, const char *dev){
    char cmd[512];
    snprintf(cmd,sizeof cmd,
      "arecord -q -D %s -c %d -r %d -f S32_LE -t raw --period-size=1024 --buffer-size=8192 2>/dev/null",
      dev?dev:"hw:2,0", nch, rate);
    FILE *p=popen(cmd,"r"); if(!p){ logmsg("arecord popen failed"); return NULL; }
    adev_t *d=calloc(1,sizeof *d); d->p=p; d->nch=nch;
    audio_src *a=calloc(1,sizeof *a); a->ctx=d; a->read=dev_read; a->close=dev_close;
    logmsg("audio: %s", cmd);
    return a;
}
