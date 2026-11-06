#include "monitor.h"
#include "common.h"

#include <alsa/asoundlib.h>
#include <string.h>

static snd_pcm_t *g_pcm;
static int g_underruns;
static int g_drops;
static int g_open;

int monitor_open(uint32_t rate)
{
    monitor_close();
    /* 优先直开 hw:0,0：default 是 plug+softvol 链，额外一层缓冲且只肯给 4096 帧，
       直开硬件缓冲更大，欠载更少；失败再退回 default。 */
    snd_pcm_t *pcm = NULL;
    const char *dev = "hw:0,0";
    int rc = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if(rc < 0){
        dev = "default";
        rc = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    }
    if(rc < 0){
        logmsg("monitor: open failed (%s)", snd_strerror(rc));
        return -1;
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
    unsigned r = rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0);
    snd_pcm_hw_params_set_channels(pcm, hw, 2);
    /* 小周期低延迟：监听要求 < 30 ms */
    snd_pcm_uframes_t period = 1024, buffer = 16384;  /* 要大缓冲抗抖动；硬件实际只给 4096 帧 */
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
    if(snd_pcm_hw_params(pcm, hw) < 0){
        logmsg("monitor: hw_params failed");
        snd_pcm_close(pcm);
        return -1;
    }
    snd_pcm_prepare(pcm);
    g_pcm = pcm;
    g_open = 1;
    g_underruns = 0;
    g_drops = 0;
    snd_pcm_uframes_t gp = 0, gb = 0;
    snd_pcm_hw_params_get_period_size(hw, &gp, 0);
    snd_pcm_hw_params_get_buffer_size(hw, &gb);
    logmsg("monitor: open %s %u Hz, period %lu, buffer %lu (got %lu/%lu)",
           dev, r, (unsigned long)period, (unsigned long)buffer,
           (unsigned long)gp, (unsigned long)gb);
    return 0;
}

void monitor_close(void)
{
    if(g_pcm){
        snd_pcm_drop(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        logmsg("monitor: closed (%d underruns, %d drops)", g_underruns, g_drops);
    }
    g_open = 0;
}

int monitor_is_open(void) { return g_open; }
int monitor_underruns(void) { return g_underruns; }
int monitor_drops(void) { return g_drops; }

int monitor_write(const int32_t *frames, uint32_t n)
{
    if(!g_pcm || !n) return 0;
    uint32_t done = 0;
    while(done < n){
        snd_pcm_sframes_t w = snd_pcm_writei(g_pcm, frames + (size_t)done * 2, n - done);
        if(w == -EAGAIN){
            /* 缓冲满：本块剩余部分直接丢弃，绝不阻塞录音线程 */
            g_drops++;
            return 0;
        }
        if(w < 0){
            if(snd_pcm_recover(g_pcm, (int)w, 1) < 0){
                g_underruns++;
                return -1;
            }
            g_underruns++;
            continue;
        }
        done += (uint32_t)w;
    }
    return 0;
}