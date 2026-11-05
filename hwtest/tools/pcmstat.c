/* pcmstat - analyse a captured WAV: per-channel RMS + dominant frequency.
 * Used to tell "the driver accepted the format" from "the hardware actually
 * delivered audio at the requested rate".
 *
 * Build: aarch64-linux-gcc -O2 -static -o pcmstat pcmstat.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct {
    int ch, rate, bits, block, bpf;
    long frames;
    unsigned char *data;
} wav_t;

static int parse_wav(const char *path, wav_t *w)
{
    FILE *f = fopen(path, "rb");
    if(!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz);
    if(!buf) { fclose(f); return -1; }
    if(fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
    fclose(f);

    if(sz < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return -1; }

    long pos = 12;
    int have_fmt = 0;
    while(pos + 8 <= sz) {
        char id[5] = {0};
        memcpy(id, buf + pos, 4);
        uint32_t len = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
                       ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
        if(!memcmp(id, "fmt ", 4) && len >= 16) {
            const unsigned char *p = buf + pos + 8;
            w->ch = p[2] | (p[3] << 8);
            w->rate = p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24);
            w->bits = p[14] | (p[15] << 8);
            w->block = p[12] | (p[13] << 8);   /* block align: 3 or 4 bytes for 24-bit */
            have_fmt = 1;
        }
        else if(!memcmp(id, "data", 4)) {
            w->data = buf + pos + 8;
            w->frames = (long)len;
            if(!have_fmt) { free(buf); return -1; }
            if(w->block <= 0) w->block = w->ch * (w->bits / 8);
            w->bpf = w->block / w->ch;
            if(w->block <= 0) { free(buf); return -1; }
            w->frames /= w->block;
            return 0;   /* keep buf alive */
        }
        pos += 8 + len + (len & 1);
    }
    free(buf);
    return -1;
}

static double sample_at(const wav_t *w, long frame, int ch)
{
    const unsigned char *p = w->data + (size_t)frame * w->block + (size_t)ch * w->bpf;
    if(w->bpf == 2) {
        int16_t v;
        memcpy(&v, p, 2);
        return v / 32768.0;
    }
    if(w->bpf == 3) {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if(v & 0x800000) v |= ~0xffffff;
        return v / 8388608.0;
    }
    if(w->bpf == 4) {
        int32_t v;
        memcpy(&v, p, 4);
        return v / 2147483648.0;
    }
    return 0.0;
}

static double goertzel(const wav_t *w, int ch, double freq, long n)
{
    if(n > w->frames) n = w->frames;
    if(n <= 0) return 0.0;
    double w0 = 2.0 * M_PI * freq / w->rate;
    double c = 2.0 * cos(w0);
    double s1 = 0, s2 = 0;
    for(long i = 0; i < n; i++) {
        double s0 = sample_at(w, i, ch) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (double)n;
}

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: pcmstat file.wav\n"); return 2; }
    wav_t w;
    if(parse_wav(argv[1], &w)) { printf("PARSE_FAIL\n"); return 1; }

    long n = w.frames < w.rate / 2 ? w.frames : w.rate / 2;   /* 0.5 s window */
    double best_rms = 0;
    int best_ch = 0;
    for(int c = 0; c < w.ch; c++) {
        double ss = 0;
        for(long i = 0; i < n; i++) { double v = sample_at(&w, i, c); ss += v * v; }
        double rms = sqrt(ss / (double)n);
        if(rms > best_rms) { best_rms = rms; best_ch = c; }
    }

    double bestf = 0, bestm = 0;
    for(double f = 100; f <= 5000; f += 5.0) {
        double m = goertzel(&w, best_ch, f, n);
        if(m > bestm) { bestm = m; bestf = f; }
    }

    double rms_db = 20.0 * log10(best_rms + 1e-12);
    double mag_db = 20.0 * log10(bestm + 1e-12);
    printf("ch=%d bits=%d bpf=%d rate=%d best=ch%d rms=%.1fdB tone=%.1fdB f=%.0fHz",
           w.ch, w.bits, w.bpf, w.rate, best_ch, rms_db, mag_db, bestf);
    if(w.ch <= 8) {
        printf(" |");
        for(int c = 0; c < w.ch; c++) {
            double ss = 0;
            for(long i = 0; i < n; i++) { double v = sample_at(&w, i, c); ss += v * v; }
            double r = 20.0 * log10(sqrt(ss / (double)n) + 1e-12);
            double m = goertzel(&w, c, bestf, n);
            printf(" c%d:%.0f/%.0f", c, r, 20.0 * log10(m + 1e-12));
        }
    }
    printf("\n");
    return 0;
}
