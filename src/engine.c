#include "engine.h"
#include "common.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static pthread_t g_th;
static volatile int g_stop;
static rec_status g_st;
static int g_threaded;

static void *engine_thread(void *arg){
    rec_cfg *c = arg;
    rec_run(c);
    g_threaded = 0;
    return NULL;
}

int engine_start(const char *dir, int use_device, int omni, int seg_sec, int nch, int rate, int bits){
    if(g_threaded) return -1;
    memset(&g_st,0,sizeof g_st);
    rec_cfg *c = malloc(sizeof *c);
    memset(c,0,sizeof *c);
    c->dir=dir; c->segment_sec=seg_sec; c->use_device=use_device; c->dev="hw:2,0";
    c->nch_in=nch; c->rate=rate; c->bits=bits; c->omni=omni; c->stop=&g_stop; c->status=&g_st;
    g_stop=0;
    if(pthread_create(&g_th,NULL,engine_thread,c)!=0){ free(c); return -1; }
    g_threaded=1;
    /* 等采集真正起来（或明确失败），最多 1.5 s */
    for(int i=0;i<30;i++){
        if(g_st.running) return 0;
        if(g_st.failed){ pthread_join(g_th,NULL); g_threaded=0; free(c); return -2; }
        usleep(50000);
    }
    return 0;
}
void engine_stop(void){
    g_stop=1;
    if(g_threaded){ pthread_join(g_th,NULL); g_threaded=0; }
}
int engine_running(void){ return g_st.running; }
void engine_pause(int on){ if(g_st.running) g_st.paused = on ? 1 : 0; }
rec_status *engine_status(void){ return &g_st; }
