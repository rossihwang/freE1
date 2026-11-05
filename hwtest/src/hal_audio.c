#include "hal_audio.h"
#include "hal_keys.h"      /* e1_now_ms */
#include "app.h"           /* nothing, but keeps app state close */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define RATE            32000
#define NCH             8
#define FRAME_BYTES     (NCH * 4)
#define CHUNK_FRAMES    800          /* 25 ms */
#define REC_SECONDS     5
#define REC_FRAMES      (RATE * REC_SECONDS)
#define WARMUP_FRAMES   (RATE * 2 / 5)   /* 400 ms PDM power-up transient */
#define ENV_BUCKETS     64

typedef enum { CAP_NONE = 0, CAP_METER, CAP_RECORD } cap_mode_t;

static pid_t g_cap_pid = -1;
static int   g_cap_fd  = -1;
static cap_mode_t g_cap_mode = CAP_NONE;

static uint8_t g_rd[65536];
static int g_rd_len;

static float g_lvl[NCH];
static float g_peak[NCH];
static uint32_t g_peak_ms[NCH];
static bool g_clip[NCH];
static uint32_t g_warm_seen;
static uint32_t g_rec_skip;

static int32_t *g_rec;
static uint32_t g_rec_frames;
static bool g_rec_done;
static float g_rec_peak_db, g_rec_gain_db;

static pid_t g_play_pid = -1;
static int   g_play_fd  = -1;
static uint32_t g_play_sent;
static uint32_t g_play_last_ms;
static bool g_playing;

static float g_env[ENV_BUCKETS];

static pid_t spawn_capture(int *fd_out)
{
    int p[2];
    if(pipe(p) != 0) return -1;

    pid_t pid = fork();
    if(pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        close(p[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if(devnull >= 0) dup2(devnull, STDERR_FILENO);
        execl("/usr/bin/arecord", "arecord",
              "-D", "hw:2,0", "-c", "8", "-r", "32000", "-f", "S32_LE",
              "--period-size=2400", "--buffer-size=14400",
              "-t", "raw", "-q", "-", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    *fd_out = p[0];
    return pid;
}

static void stop_capture(void)
{
    if(g_cap_fd >= 0) { close(g_cap_fd); g_cap_fd = -1; }
    if(g_cap_pid > 0) {
        kill(g_cap_pid, SIGTERM);
        int st;
        waitpid(g_cap_pid, &st, WNOHANG);
        g_cap_pid = -1;
    }
    g_cap_mode = CAP_NONE;
}

static void meter_reset(void)
{
    for(int i = 0; i < NCH; i++) {
        g_lvl[i] = -90.0f;
        g_peak[i] = -90.0f;
        g_peak_ms[i] = 0;
        g_clip[i] = false;
    }
    g_warm_seen = 0;
}

void audio_init(void)
{
    meter_reset();
    g_rec = NULL;
    /* keep a sane output level; the DAC itself is armed before each play */
    if(system("/usr/bin/amixer -q -c 0 sset Master 80 >/dev/null 2>&1") < 0) {}
}

static void meter_feed(const int32_t *fr, int n)
{
    if(g_warm_seen < WARMUP_FRAMES) {
        uint32_t skip = WARMUP_FRAMES - g_warm_seen;
        if((uint32_t)n <= skip) { g_warm_seen += (uint32_t)n; return; }
        fr += (size_t)skip * NCH;
        n -= (int)skip;
        g_warm_seen = WARMUP_FRAMES;
    }

    double ss[NCH];
    int32_t pk[NCH];
    for(int c = 0; c < NCH; c++) { ss[c] = 0.0; pk[c] = 0; }

    for(int i = 0; i < n; i++) {
        for(int c = 0; c < NCH; c++) {
            int32_t v = fr[(size_t)i * NCH + c];
            ss[c] += (double)v * (double)v;
            int32_t a = v < 0 ? -v : v;
            if(a > pk[c]) pk[c] = a;
        }
    }

    uint32_t now = e1_now_ms();
    for(int c = 0; c < NCH; c++) {
        double rms = sqrt(ss[c] / (double)n) / 2147483648.0;
        float db = (float)(20.0 * log10(rms + 1e-12));
        if(db < -90.0f) db = -90.0f;
        if(db > g_lvl[c]) g_lvl[c] = db;
        else g_lvl[c] -= 20.0f * (float)n / (float)RATE;   /* 20 dB/s release */
        if(g_lvl[c] < -90.0f) g_lvl[c] = -90.0f;

        float pdb = (float)(20.0 * log10((double)pk[c] / 2147483648.0 + 1e-12));
        if(pdb > g_peak[c] || (uint32_t)(now - g_peak_ms[c]) > 1500u) {
            g_peak[c] = pdb;
            g_peak_ms[c] = now;
        }
        if(pk[c] > (int32_t)(0.98 * 2147483647.0)) g_clip[c] = true;
    }
}

static void record_feed(const int32_t *fr, int n)
{
    /* discard the PDM power-up transient, exactly like the meter does */
    if(g_rec_skip < WARMUP_FRAMES) {
        uint32_t skip = WARMUP_FRAMES - g_rec_skip;
        if((uint32_t)n <= skip) { g_rec_skip += (uint32_t)n; return; }
        fr += (size_t)skip * NCH;
        n -= (int)skip;
        g_rec_skip = WARMUP_FRAMES;
    }

    /* directional pair is ch6 (left) / ch7 (right) */
    while(n-- > 0 && g_rec_frames < REC_FRAMES) {
        g_rec[(size_t)g_rec_frames * 2 + 0] = fr[6];
        g_rec[(size_t)g_rec_frames * 2 + 1] = fr[7];
        g_rec_frames++;
        fr += NCH;
    }
}

static void parse_frames(void)
{
    int done = 0;
    while(g_rd_len - done >= (int)(CHUNK_FRAMES * FRAME_BYTES)) {
        const int32_t *fr = (const int32_t *)(g_rd + done);
        meter_feed(fr, CHUNK_FRAMES);
        if(g_cap_mode == CAP_RECORD) record_feed(fr, CHUNK_FRAMES);
        done += CHUNK_FRAMES * FRAME_BYTES;
    }
    if(done) {
        memmove(g_rd, g_rd + done, (size_t)(g_rd_len - done));
        g_rd_len -= done;
    }

    if(g_cap_mode == CAP_RECORD && g_rec_frames >= REC_FRAMES) {
        stop_capture();
        /* normalise to -6 dBFS, at most +30 dB */
        double peak = 0.0;
        for(size_t i = 0; i < (size_t)g_rec_frames * 2; i++) {
            double a = fabs((double)g_rec[i]) / 2147483648.0;
            if(a > peak) peak = a;
        }
        float pdb = (float)(20.0 * log10(peak + 1e-12));
        float gain = -6.0f - pdb;
        if(gain > 30.0f) gain = 30.0f;
        if(gain < 0.0f) gain = 0.0f;
        double g = pow(10.0, gain / 20.0);
        for(size_t i = 0; i < (size_t)g_rec_frames * 2; i++) {
            double v = (double)g_rec[i] * g;
            if(v > 2147483647.0) v = 2147483647.0;
            if(v < -2147483648.0) v = -2147483648.0;
            g_rec[i] = (int32_t)v;
        }
        g_rec_peak_db = pdb;
        g_rec_gain_db = gain;
        g_rec_done = true;

        /* 64-bucket peak envelope for the UI */
        for(int b = 0; b < ENV_BUCKETS; b++) {
            size_t a0 = (size_t)g_rec_frames * b / ENV_BUCKETS;
            size_t a1 = (size_t)g_rec_frames * (b + 1) / ENV_BUCKETS;
            double m = 0.0;
            for(size_t i = a0; i < a1; i++) {
                double l = fabs((double)g_rec[i * 2]) / 2147483648.0;
                double r = fabs((double)g_rec[i * 2 + 1]) / 2147483648.0;
                if(l > m) m = l;
                if(r > m) m = r;
            }
            float db = (float)(20.0 * log10(m + 1e-12));
            float x = (db + 60.0f) / 60.0f;
            if(x < 0) x = 0;
            if(x > 1) x = 1;
            g_env[b] = x;
        }
    }
}

void audio_meter_start(void)
{
    stop_capture();
    meter_reset();
    g_rd_len = 0;
    g_cap_pid = spawn_capture(&g_cap_fd);
    g_cap_mode = g_cap_pid > 0 ? CAP_METER : CAP_NONE;
}

void audio_meter_stop(void)
{
    if(g_cap_mode == CAP_METER) stop_capture();
}

bool audio_meter_active(void) { return g_cap_mode == CAP_METER; }

float audio_level_db(int ch) { return (ch >= 0 && ch < NCH) ? g_lvl[ch] : -90.0f; }
float audio_peak_db(int ch)  { return (ch >= 0 && ch < NCH) ? g_peak[ch] : -90.0f; }
bool  audio_clip(int ch)     { return (ch >= 0 && ch < NCH) ? g_clip[ch] : false; }

void audio_rec_reset(void)
{
    if(g_cap_mode == CAP_RECORD) stop_capture();
    g_rec_done = false;
    g_rec_frames = 0;
    g_rec_peak_db = -90.0f;
    g_rec_gain_db = 0.0f;
    memset(g_env, 0, sizeof(g_env));
}

void audio_rec_start(void)
{
    audio_rec_reset();
    if(!g_rec) g_rec = malloc((size_t)REC_FRAMES * 2 * sizeof(int32_t));
    if(!g_rec) return;
    g_rd_len = 0;
    g_rec_skip = 0;
    g_cap_pid = spawn_capture(&g_cap_fd);
    g_cap_mode = g_cap_pid > 0 ? CAP_RECORD : CAP_NONE;
}

void audio_rec_cancel(void)
{
    if(g_cap_mode == CAP_RECORD) stop_capture();
    g_rec_done = true;
}

bool  audio_recording(void)   { return g_cap_mode == CAP_RECORD; }
bool  audio_rec_done(void)    { return g_rec_done; }
float audio_rec_progress(void) { return (float)g_rec_frames / (float)REC_FRAMES; }
float audio_rec_gain_db(void) { return g_rec_gain_db; }
float audio_rec_peak_db(void) { return g_rec_peak_db; }

const float *audio_envelope(int *count)
{
    if(count) *count = ENV_BUCKETS;
    return g_env;
}

static void spawn_player(void)
{
    int p[2];
    if(pipe(p) != 0) return;
    pid_t pid = fork();
    if(pid == 0) {
        dup2(p[0], STDIN_FILENO);
        close(p[0]);
        close(p[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if(devnull >= 0) dup2(devnull, STDERR_FILENO);
        execl("/usr/bin/aplay", "aplay",
              "-D", "hw:0,0", "-c", "2", "-r", "32000", "-f", "S32_LE",
              "-t", "raw", "-q", "-", (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    g_play_pid = pid;
    g_play_fd = p[1];
    g_play_sent = 0;
    g_play_last_ms = e1_now_ms();
    g_playing = true;
}

void audio_play(void)
{
    if(!g_rec || !g_rec_done) return;
    audio_play_stop();

    /* The codec resets HPOUT to -inf when a stream closes, so arming it here
     * is mandatory: otherwise the headphones stay silent. */
    if(system("/usr/bin/amixer -q -c 0 sset 'DAC HPOUT Left' 20 >/dev/null 2>&1") < 0) {}
    if(system("/usr/bin/amixer -q -c 0 sset 'DAC HPOUT Right' 20 >/dev/null 2>&1") < 0) {}

    spawn_player();
}

void audio_play_stop(void)
{
    if(g_play_fd >= 0) { close(g_play_fd); g_play_fd = -1; }
    if(g_play_pid > 0) {
        kill(g_play_pid, SIGTERM);
        int st;
        waitpid(g_play_pid, &st, WNOHANG);
        g_play_pid = -1;
    }
    g_playing = false;
}

bool audio_playing(void) { return g_playing; }

float audio_play_progress(void)
{
    if(!g_rec_frames) return 0.0f;
    return (float)g_play_sent / (float)g_rec_frames;
}

static void playback_poll(void)
{
    if(!g_playing || g_play_fd < 0) return;

    uint32_t now = e1_now_ms();
    uint32_t due = (uint32_t)((uint64_t)(now - g_play_last_ms) * RATE / 1000u);
    if(due == 0) return;

    if(due > 2000) due = 2000;    /* never burst more than 62 ms at once */
    uint32_t left = g_rec_frames - g_play_sent;
    if(due > left) due = left;

    if(due > 0) {
        const uint8_t *src = (const uint8_t *)(g_rec + (size_t)g_play_sent * 2);
        size_t len = (size_t)due * 2 * sizeof(int32_t);
        ssize_t w = write(g_play_fd, src, len);
        if(w > 0) {
            g_play_sent += (uint32_t)((size_t)w / (2 * sizeof(int32_t)));
            g_play_last_ms = now;
        }
    }

    if(g_play_sent >= g_rec_frames) {
        close(g_play_fd);
        g_play_fd = -1;
        g_playing = false;   /* aplay drains the pipe and exits by itself */
    }
}

void audio_poll(void)
{
    if(g_cap_fd >= 0) {
        ssize_t r = read(g_cap_fd, g_rd + g_rd_len, sizeof(g_rd) - (size_t)g_rd_len);
        if(r > 0) {
            g_rd_len += (int)r;
            parse_frames();
        }
    }

    if(g_cap_mode == CAP_NONE && g_cap_pid > 0) {
        int st;
        if(waitpid(g_cap_pid, &st, WNOHANG) == g_cap_pid) g_cap_pid = -1;
    }

    if(g_play_pid > 0) {
        int st;
        if(waitpid(g_play_pid, &st, WNOHANG) == g_play_pid) g_play_pid = -1;
    }

    playback_poll();
}
