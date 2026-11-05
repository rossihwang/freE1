#include "wav.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>

static void put32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }
static uint32_t get32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }

static void write_header(FILE *f, uint32_t rate, uint16_t ch, uint32_t data_bytes, uint16_t bits){
    uint8_t h[44]; memset(h,0,sizeof h);
    memcpy(h,"RIFF",4);
    put32(h+4, 36u + data_bytes);
    memcpy(h+8,"WAVEfmt ",8);
    put32(h+16, 16);           /* fmt chunk size */
    put16(h+20, 1);            /* PCM */
    put16(h+22, ch);
    put32(h+24, rate);
    put32(h+28, rate * ch * (bits/8));   /* byte rate */
    put16(h+32, ch * (bits/8));          /* block align */
    put16(h+34, bits);                   /* bits */
    memcpy(h+36,"data",4);
    put32(h+40, data_bytes);
    fseek(f,0,SEEK_SET);
    fwrite(h,1,sizeof h,f);
    fseek(f,0,SEEK_END);
}

int wav_open(wav_t *w, const char *path, uint32_t rate, uint16_t ch, uint16_t bits){
    memset(w,0,sizeof *w);
    snprintf(w->path,sizeof w->path,"%s",path);
    w->f = fopen(path,"wb+");
    /* 新文件的目录项必须落盘，否则硬断电后文件会"消失"（ext2 无日志）。
       目录 fsync 在部分 SD 卡上不生效，这里再做一次全局 sync。 */
    if(w->f){
        fflush(w->f);
        fsync(fileno(w->f));
        char tmp[512]; snprintf(tmp,sizeof tmp,"%s", path);
        char *d = dirname(tmp);
        int dfd = open(d, O_RDONLY|O_DIRECTORY);
        int drc = -1;
        if(dfd >= 0){ drc = fsync(dfd); close(dfd); }
        logmsg("wav: created %s (dir fsync=%d)", path, drc);
        sync();
    }
    if(!w->f) return -1;
    w->rate=rate; w->ch=ch; w->bits=bits;
    write_header(w->f, rate, ch, 0, bits);
    fflush(w->f);
    return 0;
}

int wav_append(wav_t *w, const int32_t *src, uint32_t frames, int nch_src, const int map[2]){
    if(!w->f) return -1;
    uint8_t buf[3*2*1024];
    int bps = (w->bits == 16) ? 2 : 3;
    uint32_t done=0;
    while(done<frames){
        uint32_t n = frames-done; if(n>1024) n=1024;
        uint8_t *p=buf;
        for(uint32_t i=0;i<n;i++){
            for(int c=0;c<2;c++){
                int32_t s = src[(size_t)(done+i)*nch_src + map[c]];
                if(bps == 2){
                    int32_t v16 = s >> 16;                    /* 32 -> 16 bit */
                    *p++ = (uint8_t)(v16 & 0xff);
                    *p++ = (uint8_t)((v16>>8) & 0xff);
                } else {
                    int32_t v = s >> 8;                       /* 24-bit 左对齐在 32-bit 高 24 位 */
                    *p++ = (uint8_t)(v & 0xff);
                    *p++ = (uint8_t)((v>>8) & 0xff);
                    *p++ = (uint8_t)((v>>16) & 0xff);
                }
            }
        }
        size_t nb = (size_t)n * 2 * bps;
        if(fwrite(buf,1,nb,w->f) != nb) return -1;
        done += n;
    }
    w->data_bytes += (uint64_t)frames*2*bps;
    return 0;
}

int wav_sync(wav_t *w){
    if(!w || !w->f) return -1;
    fflush(w->f);
    return fsync(fileno(w->f));
}

int wav_close(wav_t *w, int do_sync){
    if(!w->f) return -1;
    uint32_t sz = (uint32_t)(w->data_bytes > 0xFFFFFFFFull ? 0xFFFFFFFFull : w->data_bytes);
    write_header(w->f, w->rate, w->ch, sz, w->bits);
    fflush(w->f);
    if(do_sync) fsync(fileno(w->f));
    fclose(w->f); w->f=NULL;
    return 0;
}

int wav_repair(const char *path, uint64_t *frames_out){
    FILE *f=fopen(path,"rb+");
    if(!f) return -1;
    fseek(f,0,SEEK_END); long sz=ftell(f);
    if(sz < 44){ fclose(f); return -1; }
    uint8_t h[44];
    fseek(f,0,SEEK_SET);
    if(fread(h,1,44,f)!=44){ fclose(f); return -1; }
    if(memcmp(h,"RIFF",4) || memcmp(h+8,"WAVE",4)){ fclose(f); return -1; }
    uint16_t ch_h = (uint16_t)(h[22]|(h[23]<<8));
    uint16_t bits_h = (uint16_t)(h[34]|(h[35]<<8)); if(bits_h==0) bits_h=24;
    uint32_t ba = (uint32_t)ch_h * (bits_h/8);
    uint32_t data = (uint32_t)((sz-44) / ba * ba);
    write_header(f, get32(h+24), ch_h, data, bits_h);
    fflush(f); fsync(fileno(f)); fclose(f);
    if(frames_out) *frames_out = data/ba;
    return 0;
}
