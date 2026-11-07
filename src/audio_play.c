#include "audio_play.h"
#include "common.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK_FRAMES 1024

typedef struct {
    pthread_mutex_t lock;
    pthread_t th;
    int  thread_alive;
    int  stop_req;
    int  paused;

    FILE *f;
    char file[192];
    uint32_t rate;
    uint16_t ch;
    uint16_t bits;
    int64_t  data_off;      /* 数据起点 */
    int64_t  data_bytes;
    int64_t  pos_bytes;     /* 已送出的字节数 */

    int64_t  seek_req_ms;   /* <0 表示无请求 */
    int      scan_chunks;   /* >0 = 每读一块多跳过这么多块（快进） */
} player_t;

static player_t g_p;

static int64_t ms_from_bytes(const player_t *p, int64_t bytes)
{
    int frame = p->ch * (p->bits / 8);
    if(frame <= 0 || p->rate == 0) return 0;
    return bytes / frame * 1000 / p->rate;
}

static int64_t bytes_from_ms(const player_t *p, int64_t ms)
{
    int frame = p->ch * (p->bits / 8);
    int64_t frames = ms * (int64_t)p->rate / 1000;
    int64_t b = frames * frame;
    if(b > p->data_bytes) b = p->data_bytes;
    if(b < 0) b = 0;
    return b;
}

/* 从 WAV 头解析；只支持 PCM */
static int read_wav_header(FILE *f, uint32_t *rate, uint16_t *ch, uint16_t *bits, int64_t *data_off, int64_t *data_bytes)
{
    unsigned char h[44];
    if(fseek(f, 0, SEEK_END) != 0) return -1;
    long sz = ftell(f);
    if(sz < 44) return -1;
    if(fseek(f, 0, SEEK_SET) != 0) return -1;
    if(fread(h, 1, 44, f) != 44) return -1;
    if(memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    *ch   = (uint16_t)(h[22] | (h[23] << 8));
    *rate = (uint32_t)(h[24] | (h[25] << 8) | (h[26] << 16) | ((uint32_t)h[27] << 24));
    *bits = (uint16_t)(h[34] | (h[35] << 8));
    uint32_t data = (uint32_t)(h[40] | (h[41] << 8) | (h[42] << 16) | ((uint32_t)h[43] << 24));
    *data_off = 44;
    int64_t avail = (int64_t)sz - 44;
    if(data == 0 || data > avail) data = (uint32_t)avail;   /* 未闭合/写坏时按实际大小 */
    *data_bytes = data;
    if(*rate == 0 || *ch == 0 || (*bits != 16 && *bits != 24)) return -1;
    return 0;
}

static void *player_thread(void *arg)
{
    player_t *p = (player_t *)arg;
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw = NULL;
    int32_t *out = NULL;
    int rc;

    if(snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0){
        logmsg("play: snd_pcm_open failed");
        goto done;
    }
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
    unsigned rate = p->rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
    snd_pcm_hw_params_set_channels(pcm, hw, 2);
    snd_pcm_hw_params_set_period_size_near(pcm, hw, (snd_pcm_uframes_t[]){CHUNK_FRAMES}, NULL);
    if(snd_pcm_hw_params(pcm, hw) < 0){
        logmsg("play: hw_params failed");
        goto done;
    }
    out = malloc(sizeof(int32_t) * CHUNK_FRAMES * 2);
    if(!out) goto done;
    logmsg("play: '%s' %u Hz %u ch %u bit", p->file, p->rate, p->ch, p->bits);

    unsigned char *in = malloc((size_t)CHUNK_FRAMES * 2 * 4);
    if(!in) goto done;

    while(1){
        pthread_mutex_lock(&p->lock);
        if(p->stop_req){ pthread_mutex_unlock(&p->lock); break; }
        int paused = p->paused;
        int scan = p->scan_chunks;
        int64_t seek = p->seek_req_ms;
        if(seek >= 0){
            p->seek_req_ms = -1;
            int64_t b = bytes_from_ms(p, seek);
            p->pos_bytes = b;
            fseek(p->f, (long)(p->data_off + b), SEEK_SET);
        }
        pthread_mutex_unlock(&p->lock);

        if(seek >= 0){
            snd_pcm_drop(pcm);
            snd_pcm_prepare(pcm);
            logmsg("play: seek to %lld ms", (long long)seek);
        }

        if(paused){ usleep(30000); continue; }

        /* 读一块并转 S32_LE */
        int frame_bytes = p->ch * (p->bits / 8);
        size_t want = (size_t)CHUNK_FRAMES * frame_bytes;
        pthread_mutex_lock(&p->lock);
        size_t got = fread(in, 1, want, p->f);
        if(got > 0) p->pos_bytes += (int64_t)got;
        /* 快进：整块照常送出去（不动 PCM，所以没有每次 seek 的咔哒声），
           但下一块直接从更后面读，听觉上就是快速掠过内容 */
        if(got > 0 && scan > 0){
            int64_t skip = (int64_t)scan * (int64_t)want;
            if(p->pos_bytes + skip > p->data_bytes) skip = p->data_bytes - p->pos_bytes;
            if(skip > 0){
                fseek(p->f, (long)skip, SEEK_CUR);
                p->pos_bytes += skip;
            }
        }
        pthread_mutex_unlock(&p->lock);
        if(got == 0) break;   /* 播完 */

        size_t frames = got / frame_bytes;
        if(frames == 0) break;
        for(size_t i = 0; i < frames; i++){
            for(int c = 0; c < 2; c++){
                int32_t v = 0;
                const unsigned char *s = in + i * frame_bytes + c * (p->bits / 8);
                if(p->bits == 24){
                    int32_t t = s[0] | (s[1] << 8) | (s[2] << 16);
                    if(t & 0x800000) t |= (int32_t)0xFF000000;
                    v = t << 8;
                } else {
                    int16_t t = (int16_t)(s[0] | (s[1] << 8));
                    v = (int32_t)t << 16;
                }
                out[i * 2 + c] = v;
            }
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, out, frames);
        if(w < 0){
            if(snd_pcm_recover(pcm, (int)w, 1) < 0){ logmsg("play: write failed"); break; }
            continue;
        }
    }
    free(in);
done:
    if(out) free(out);
    if(pcm){ snd_pcm_drain(pcm); snd_pcm_close(pcm); }
    pthread_mutex_lock(&p->lock);
    p->thread_alive = 0;
    p->stop_req = 0;
    pthread_mutex_unlock(&p->lock);
    logmsg("play: thread exit (%s)", p->file);
    return NULL;
}

int play_open(const char *dir, const char *name)
{
    static int inited = 0;
    if(!inited){ pthread_mutex_init(&g_p.lock, NULL); inited = 1; }
    play_stop();

    pthread_mutex_lock(&g_p.lock);
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if(!f){ pthread_mutex_unlock(&g_p.lock); logmsg("play: open %s failed", path); return -1; }
    uint32_t rate; uint16_t ch, bits; int64_t off, bytes;
    if(read_wav_header(f, &rate, &ch, &bits, &off, &bytes) != 0){
        fclose(f); pthread_mutex_unlock(&g_p.lock);
        logmsg("play: bad wav %s", path);
        return -2;
    }
    fseek(f, (long)off, SEEK_SET);
    if(g_p.f) { fclose(g_p.f); g_p.f = NULL; }
    g_p.f = f;
    snprintf(g_p.file, sizeof g_p.file, "%s", name);
    g_p.rate = rate; g_p.ch = ch; g_p.bits = bits;
    g_p.data_off = off; g_p.data_bytes = bytes;
    g_p.pos_bytes = 0;
    g_p.paused = 0;
    g_p.scan_chunks = 0;
    g_p.stop_req = 0;
    g_p.seek_req_ms = -1;
    pthread_mutex_unlock(&g_p.lock);

    if(pthread_create(&g_p.th, NULL, player_thread, &g_p) != 0){
        logmsg("play: pthread_create failed");
        return -3;
    }
    g_p.thread_alive = 1;
    pthread_detach(g_p.th);
    return 0;
}

void play_stop(void)
{
    if(g_p.thread_alive){
        pthread_mutex_lock(&g_p.lock);
        g_p.stop_req = 1;
        pthread_mutex_unlock(&g_p.lock);
        /* 等线程结束（最多 500 ms） */
        for(int i = 0; i < 50 && g_p.thread_alive; i++) usleep(10000);
    }
    pthread_mutex_lock(&g_p.lock);
    if(g_p.f){ fclose(g_p.f); g_p.f = NULL; }
    g_p.paused = 0;
    g_p.pos_bytes = 0;
    g_p.seek_req_ms = -1;
    g_p.file[0] = 0;
    pthread_mutex_unlock(&g_p.lock);
}

void play_toggle_pause(void)
{
    pthread_mutex_lock(&g_p.lock);
    if(g_p.thread_alive) g_p.paused = !g_p.paused;
    pthread_mutex_unlock(&g_p.lock);
}

void play_scan(int skip_chunks)
{
    if(skip_chunks < 0) skip_chunks = 0;
    if(skip_chunks > 16) skip_chunks = 16;
    pthread_mutex_lock(&g_p.lock);
    g_p.scan_chunks = skip_chunks;
    pthread_mutex_unlock(&g_p.lock);
}

void play_seek_ms(int64_t ms)
{
    pthread_mutex_lock(&g_p.lock);
    if(g_p.thread_alive) g_p.seek_req_ms = ms;
    pthread_mutex_unlock(&g_p.lock);
}

void play_get_state(play_state_t *st)
{
    memset(st, 0, sizeof *st);
    pthread_mutex_lock(&g_p.lock);
    st->active = g_p.thread_alive;
    st->paused = g_p.paused;
    st->pos_ms = ms_from_bytes(&g_p, g_p.pos_bytes);
    st->total_ms = ms_from_bytes(&g_p, g_p.data_bytes);
    snprintf(st->file, sizeof st->file, "%s", g_p.file);
    pthread_mutex_unlock(&g_p.lock);
}

int play_is_active(void) { return g_p.thread_alive; }
