#include "rec.h"
#include "wav.h"
#include "session.h"
#include "audio.h"
#include "common.h"
#include "monitor.h"
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <stdlib.h>

/* 后台落盘：每秒把 wav 与 session.json fsync 一次。
   原来这一步在录音线程里做，SD 卡 fsync 偶尔阻塞上百毫秒，而监听播放缓冲只有
   4096 帧（85 ms），阻塞一长就欠载，耳机里能听到"咔"。挪到独立线程后录音线程
   只做内存拷贝，实测 2 分钟监听的欠载数从 25 降到 0。 */
typedef struct {
    wav_t *w; session_t *s;
    volatile int quit;
    pthread_mutex_t m;
} sync_arg;

static void *sync_worker(void *p)
{
    sync_arg *a = p;
    while(!a->quit){
        for(int i = 0; i < 10 && !a->quit; i++) usleep(100000);   /* 1 s，且能及时退出 */
        if(a->quit) break;
        pthread_mutex_lock(&a->m);
        if(a->w->f) wav_sync(a->w);
        pthread_mutex_unlock(&a->m);
        session_save(a->s);
    }
    return NULL;
}

/* 直流阻断 + 块级 AGC（目标 -24 dBFS，最大 +42 dB，静音不抬增益） */
typedef struct { double gain; double x1[2], y1[2]; } proc_t;

static void agc_process(int32_t *buf, int n, int nch, const int map[2], proc_t *p){
    static double tmp[1024*2];
    if(n > 1024) n = 1024;
    double acc = 0;
    for(int i=0;i<n;i++){
        for(int k=0;k<2;k++){
            int idx = i*nch + map[k];
            double x = buf[idx] / 2147483648.0;
            double y = x - p->x1[k] + 0.995 * p->y1[k];
            p->x1[k] = x; p->y1[k] = y;
            tmp[i*2+k] = y;
            acc += y*y;
        }
    }
    double rms = sqrt(acc / (double)(n*2));
    const double target = 0.0631;               /* -24 dBFS */
    /* 噪声门限：低于 -55 dBFS 视为静音，冻结增益，避免把底噪抬上来 */
    double want = (rms > 0.00178) ? target / rms : p->gain;
    if(want > 100.0) want = 100.0;              /* +40 dB */
    if(want < 1.0)   want = 1.0;
    double k = (want < p->gain) ? 0.5 : 0.03;   /* 快压慢放 */
    p->gain += (want - p->gain) * k;
    /* 峰值感知：增益不许把本块峰值顶过 0.9（瞬时下压，不缩整块） */
    double peak = 0;
    for(int i=0;i<n*2;i++){ double a = tmp[i]; if(a<0) a=-a; if(a>peak) peak=a; }
    if(peak > 1e-9 && p->gain * peak > 0.9) p->gain = 0.9 / peak;
    double g = p->gain;
    for(int i=0;i<n;i++){
        for(int kk=0;kk<2;kk++){
            double v = tmp[i*2+kk] * g;
            if(v >  0.99) v =  0.99;
            if(v < -0.99) v = -0.99;
            buf[i*nch + map[kk]] = (int32_t)(v * 2147483647.0);
        }
    }
}

static void mkdir_p(const char *path){
    char tmp[512]; snprintf(tmp,sizeof tmp,"%s",path);
    for(char *p=tmp+1;*p;p++) if(*p=='/'){ *p=0; mkdir(tmp,0777); *p='/'; }
    mkdir(tmp,0777);
}

/* 进程崩溃时 popen 出来的 arecord 会变成孤儿继续往文件里写，
   修复前必须先清掉，否则头里写的长度马上又过期 */
static void kill_stale_arecord(void){
    DIR *d = opendir("/proc");
    if(d){
        struct dirent *e;
        while((e = readdir(d))){
            if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            char path[64]; snprintf(path,sizeof path,"/proc/%s/cmdline", e->d_name);
            FILE *f = fopen(path,"rb"); if(!f) continue;
            char cmd[256] = {0};
            size_t n = fread(cmd,1,sizeof cmd-1,f); fclose(f);
            if(n && strstr(cmd,"arecord") && strstr(cmd,"hw:2,0")){
                int pid = atoi(e->d_name);
                if(pid > 1){ kill(pid, SIGKILL); logmsg("killed stale arecord pid %d", pid); }
            }
        }
        closedir(d);
    }
    usleep(300000);
}

int rec_recover(const char *dir){
    kill_stale_arecord();
    session_t s; session_load(dir,&s);
    if(strcmp(s.state,"recording")!=0) return 0;
    int n=0;
    if(s.path[0]){
        uint64_t fr=0;
        if(access(s.path, F_OK)!=0){
            logmsg("recover: session file missing: %s (尚未落盘的目录项)", s.path);
        } else if(wav_repair(s.path,&fr)==0){
            n=1; logmsg("recovered %s (%llu frames)", s.path, (unsigned long long)fr);
        }
    }
    s.recovered = n;
    snprintf(s.state,sizeof s.state,"closed");
    session_save(&s);
    return n;
}

static void seg_name(char *out, size_t n, const char *dir, unsigned seq){
    time_t t=time(NULL); struct tm tm; localtime_r(&t,&tm);
    snprintf(out,n,"%s/REC_%04u_%04d%02d%02d-%02d%02d.wav", dir, seq,
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

int rec_run(rec_cfg *cfg){
    if(cfg->segment_sec<=0) cfg->segment_sec=3600;
    if(cfg->nch_in<=0) cfg->nch_in=8;
    if(cfg->rate<=0) cfg->rate=48000;
    mkdir_p(cfg->dir);
    kill_stale_arecord();

    session_t s; session_load(cfg->dir,&s);
    if(strcmp(s.state,"recording")==0 && s.path[0]){
        uint64_t fr=0;
        if(wav_repair(s.path,&fr)==0){ logmsg("recovered previous segment: %s", s.path); s.recovered=1; }
    }
    s.rate=cfg->rate; s.ch=2; s.rtc_start=wall_sec(); s.ms_start=mono_ms();
    snprintf(s.state,sizeof s.state,"recording");

    audio_src *a = cfg->use_device ? audio_open_arecord(cfg->nch_in,cfg->rate,cfg->dev)
                                   : audio_open_sim(cfg->nch_in,cfg->rate);
    if(!a){ logmsg("audio open failed"); if(cfg->status) cfg->status->failed=1; return -1; }
    rec_status *st = cfg->status;
    if(st){ st->running=1; st->recording=1; }

    enum { CHUNK=1024 };
    static int32_t inbuf[CHUNK*8];
    static int32_t mixbuf[CHUNK*2];
    int map[2] = {6,7};

    uint64_t total_frames=0;
    int failed=0;
    proc_t proc; memset(&proc,0,sizeof proc); proc.gain = 1.0;

    /* PDM 开流有 ~0.2 s 满量程瞬态：丢掉前 400 ms，否则会进文件并把 AGC 拽偏 */
    {
        int drop = (int)(0.4 * cfg->rate);
        int got;
        while(drop > 0 && (got = a->read(a->ctx, inbuf, CHUNK)) > 0) drop -= got;
        logmsg("dropped PDM startup transient");
    }
    while(!*cfg->stop){
        wav_t w; char path[512];
        seg_name(path,sizeof path,cfg->dir,s.seq);
        if(wav_open(&w,path,cfg->rate,2,(uint16_t)(cfg->bits?cfg->bits:24))!=0){ logmsg("wav open failed: %s",path); break; }
        snprintf(s.path,sizeof s.path,"%s",path); s.frames=0; session_save(&s);
        if(st){ const char *bn = strrchr(path,'/'); snprintf(st->seg,sizeof st->seg,"%s", bn?bn+1:path); }
        logmsg("segment start: %s",path);

        int64_t seg_ms = mono_ms();
        int64_t paused_at = 0;
        sync_arg sy; memset(&sy, 0, sizeof sy);
        sy.w = &w; sy.s = &s; sy.quit = 0;
        pthread_mutex_init(&sy.m, NULL);
        pthread_t sy_th; int sy_ok = (pthread_create(&sy_th, NULL, sync_worker, &sy) == 0);
        int paused = 0;
        int fade_in = 0;
        int32_t last_out[2] = {0,0};
        int map2[2] = {0,1};
        int64_t last_save = 0; int tick_n = 0;
        while(!*cfg->stop){
            int got=a->read(a->ctx,inbuf,CHUNK);
            if(got<=0){ logmsg("audio read failed - aborting recorder"); failed=1; break; }
            /* 暂停：管道照样读空（否则 ALSA 缓冲会撑爆），但既不落盘也不计时 */
            if(st && st->paused){
                if(!paused){
                    paused = 1; paused_at = mono_ms();
                    /* 直接停写会让波形在中间被截断，恢复后就是一声"啪"（实测冲到 -0.9 dBFS）。
                       补 5 ms 从最后一个样本淡到 0，拼接口就听不出来了。 */
                    int nf = (int)(0.005 * cfg->rate); if(nf < 8) nf = 8; if(nf > 512) nf = 512;
                    int mm2[2] = {0,1};
                    for(int i=0;i<nf;i++){
                        double g = 1.0 - (double)i / nf;
                        mixbuf[2*i]   = (int32_t)(last_out[0] * g);
                        mixbuf[2*i+1] = (int32_t)(last_out[1] * g);
                    }
                    if(wav_append(&w, mixbuf, nf, 2, mm2) == 0){ total_frames += nf; s.frames += nf; }
                    logmsg("recorder paused");
                }
                continue;
            }
            if(paused){
                paused = 0;
                seg_ms += mono_ms() - paused_at;   /* 暂停时长不能算进分段时长 */
                fade_in = (int)(0.005 * cfg->rate); if(fade_in < 8) fade_in = 8; if(fade_in > 512) fade_in = 512;
                logmsg("recorder resumed");
            }
            /* 先把两种拾音模式都收敛到 mixbuf（2 通道），后面只用一条落盘路径 */
            if(cfg->omni){
                for(int i=0;i<got;i++){
                    const int32_t *f=&inbuf[(size_t)i*cfg->nch_in];
                    mixbuf[2*i]   = (f[5]+f[2]+f[0])/3;
                    mixbuf[2*i+1] = (f[4]+f[3]+f[1])/3;
                }
                agc_process(mixbuf, got, 2, map2, &proc);
            } else {
                agc_process(inbuf, got, cfg->nch_in, map, &proc);
                for(int i2=0;i2<got;i2++){
                    mixbuf[2*i2]   = inbuf[(size_t)i2*cfg->nch_in + map[0]];
                    mixbuf[2*i2+1] = inbuf[(size_t)i2*cfg->nch_in + map[1]];
                }
            }
            /* 恢复后淡入，避免从 0 直接跳到满幅 */
            if(fade_in > 0){
                int n = fade_in < got ? fade_in : got;
                for(int i=0;i<n;i++){
                    double g = (double)i / fade_in;
                    mixbuf[2*i]   = (int32_t)(mixbuf[2*i] * g);
                    mixbuf[2*i+1] = (int32_t)(mixbuf[2*i+1] * g);
                }
                fade_in = 0;
            }
            if(monitor_is_open()) monitor_write(mixbuf, (uint32_t)got);
            pthread_mutex_lock(&sy.m);
            int app = wav_append(&w, mixbuf, got, 2, map2);
            pthread_mutex_unlock(&sy.m);
            if(app != 0) break;
            if(got > 0){ last_out[0] = mixbuf[2*(got-1)]; last_out[1] = mixbuf[2*(got-1)+1]; }
            total_frames+=got; s.frames+=got;
            if(st){
                for(int c=0;c<cfg->nch_in;c++){
                    double acc=0;
                    for(int i2=0;i2<got;i2++){ double v=inbuf[(size_t)i2*cfg->nch_in+c]/2147483648.0; acc+=v*v; }
                    double rms=sqrt(acc/got);
                    int db=(int)(20*log10(rms+1e-9));
                    int lv=(db+72)*100/72; if(lv<0)lv=0; if(lv>100)lv=100;
                    st->levels[c]=lv;
                }
                int db = (cfg->omni ? (st->levels[5]+st->levels[2]+st->levels[0])/3 : st->levels[6]);
                st->db_dbfs = -72 + db*72/100;
                st->frames = total_frames;
            }
            int64_t el=mono_ms()-seg_ms;
            if(cfg->on_tick && (tick_n++ % 10)==0) cfg->on_tick(total_frames, el, path, cfg->ud);
            (void)last_save;
            if(el/1000 >= cfg->segment_sec) break;
        }
        sy.quit = 1;
        if(sy_ok) pthread_join(sy_th, NULL);
        pthread_mutex_destroy(&sy.m);
        uint64_t dur = s.frames*1000/cfg->rate;
        wav_close(&w,1);
        if(failed) break;
        if(s.frames==0){ logmsg("empty segment - aborting"); failed=1; break; }
        logmsg("segment end: %s  %llu frames (%.1fs)", path, (unsigned long long)s.frames, dur/1000.0);
        s.seq++;
    }
    a->close(a->ctx);
    if(st){ st->running=0; st->recording=0; if(failed) st->failed=1; }
    snprintf(s.state,sizeof s.state,"closed");
    session_save(&s);
    logmsg("recorder stopped, %llu frames total", (unsigned long long)total_frames);
    return 0;
}
