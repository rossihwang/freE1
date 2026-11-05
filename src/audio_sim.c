#include "audio.h"
#include "common.h"
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct { int nch, rate; double phase; uint32_t seed; double env; double env_t; int64_t t0; uint64_t out; } sim_t;

static int sim_read(void *v, int32_t *buf, int frames){
    sim_t *s=v;
    if(!s->t0) s->t0=mono_ms();
    for(int i=0;i<frames;i++){
        /* 缓慢起伏的“说话”包络，让电平表看起来有生命 */
        s->env_t += 1.0/s->rate;
        double e = 0.12 + 0.10*(1.0+sin(2*M_PI*0.8*s->env_t)) + 0.06*(1.0+sin(2*M_PI*2.3*s->env_t+1.1));
        s->phase += 2*M_PI*1000.0/s->rate;
        if(s->phase>2*M_PI) s->phase-=2*M_PI;
        double tone = sin(s->phase)*e;
        for(int c=0;c<s->nch;c++){
            double v;
            s->seed = s->seed*1103515245u + 12345u;
            double noise = ((int32_t)(s->seed>>8) / 8388608.0) * 0.002;
            if(c==6 || c==7) v = tone*0.6 + noise;
            else             v = tone*0.12*(1.0+0.03*c) + noise;
            buf[(size_t)i*s->nch+c] = (int32_t)(v * 2147483647.0);
        }
    }
    s->out += frames;
    { int64_t due = s->t0 + (int64_t)(s->out*1000/s->rate); int64_t now=mono_ms(); if(due>now) usleep((useconds_t)((due-now)*1000)); }
    return frames;
}
static void sim_close(void *v){ free(v); }

audio_src *audio_open_sim(int nch, int rate){
    sim_t *s=calloc(1,sizeof *s); s->nch=nch; s->rate=rate; s->seed=0x12345678;
    audio_src *a=calloc(1,sizeof *a); a->ctx=s; a->read=sim_read; a->close=sim_close;
    return a;
}
