#include "report.h"
#include "app.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#define RESULTS_DIR "/mnt/sdcard/e1hwtest/results"

static void trim(char *s)
{
    size_t n = strlen(s);
    while(n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

static void read_first_line(const char *path, char *out, int len)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if(!f) return;
    if(!fgets(out, len, f)) out[0] = 0;
    fclose(f);
    trim(out);
}

void report_ids(char *cpu, int cpu_len, char *sn, int sn_len)
{
    cpu[0] = 0;
    sn[0] = 0;

    FILE *f = fopen("/proc/cpuinfo", "r");
    if(f) {
        char line[256];
        while(fgets(line, sizeof(line), f)) {
            if(strncmp(line, "Serial", 6) == 0) {
                char *p = strchr(line, ':');
                if(p) {
                    p++;
                    while(*p == ' ' || *p == '\t') p++;
                    snprintf(cpu, (size_t)cpu_len, "%s", p);
                    trim(cpu);
                    break;
                }
            }
        }
        fclose(f);
    }

    f = fopen("/mnt/sdcard/GUIDE.txt", "r");
    if(f) {
        char line[256];
        while(fgets(line, sizeof(line), f)) {
            char *p = strstr(line, "sn");
            if(p) {
                p = strchr(p, ':');
                if(p) {
                    p++;
                    while(*p == ' ' || *p == '\t') p++;
                    snprintf(sn, (size_t)sn_len, "%s", p);
                    trim(sn);
                    break;
                }
            }
        }
        fclose(f);
    }
    if(!sn[0]) snprintf(sn, (size_t)sn_len, "unknown");
    if(!cpu[0]) snprintf(cpu, (size_t)cpu_len, "unknown");
}

static const char *verdict_str(verdict_t v)
{
    switch(v) {
        case VERDICT_PASS: return "PASS";
        case VERDICT_FAIL: return "FAIL";
        default: return "SKIP";
    }
}

void report_write(void)
{
    char cpu[64], sn[64], mac[32] = "unknown", fw[32] = "unknown";
    report_ids(cpu, sizeof(cpu), sn, sizeof(sn));
    read_first_line("/sys/class/net/wlan0/address", mac, sizeof(mac));

    FILE *f = fopen("/etc/sogou/system_version.info", "r");
    if(f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        char *p = strstr(buf, "\"system_version\"");
        if(p) {
            p = strchr(p, ':');
            if(p) {
                p = strchr(p, '"');
                if(p) {
                    p++;
                    char *q = strchr(p, '"');
                    if(q && (size_t)(q - p) < sizeof(fw)) {
                        memcpy(fw, p, (size_t)(q - p));
                        fw[q - p] = 0;
                    }
                }
            }
        }
    }

    mkdir("/mnt/sdcard/e1hwtest", 0777);
    mkdir(RESULTS_DIR, 0777);

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tmv);
    char iso[40];
    strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S", &tmv);

    char path[160];
    snprintf(path, sizeof(path), RESULTS_DIR "/%s-%s.json", cpu, ts);

    f = fopen(path, "w");
    if(f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"sn\": \"%s\",\n", sn);
        fprintf(f, "  \"cpu_serial\": \"%s\",\n", cpu);
        fprintf(f, "  \"mac\": \"%s\",\n", mac);
        fprintf(f, "  \"firmware\": \"%s\",\n", fw);
        fprintf(f, "  \"time\": \"%s\",\n", iso);
        fprintf(f, "  \"items\": [\n");
        for(int i = 0; i < ITEM_COUNT; i++) {
            fprintf(f, "    {\"id\": %d, \"result\": \"%s\", \"evidence\": \"%s\"}%s\n",
                    i, verdict_str(g_app.items[i].verdict), g_app.items[i].evidence,
                    i + 1 < ITEM_COUNT ? "," : "");
        }
        fprintf(f, "  ]\n}\n");
        fclose(f);
    }

    f = fopen(RESULTS_DIR "/../results.csv", "a");
    if(f) {
        fprintf(f, "%s,%s,%s,%s,%s", iso, sn, cpu, mac, fw);
        for(int i = 0; i < ITEM_COUNT; i++) fprintf(f, ",%s", verdict_str(g_app.items[i].verdict));
        fprintf(f, "\n");
        fclose(f);
    }

    fprintf(stderr, "report: %s\n", path);
}
