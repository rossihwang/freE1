#include "marks.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

static void sidecar(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/%s.marks", dir, name);
}

int marks_append(const char *dir, const char *name, uint64_t ms)
{
    if(!dir || !name || !name[0]) return -1;
    char path[700];
    sidecar(path, sizeof path, dir, name);
    FILE *f = fopen(path, "a");
    if(!f){ logmsg("marks: cannot open %s", path); return -1; }
    int ok = (fprintf(f, "%llu\n", (unsigned long long)ms) > 0);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if(ok) logmsg("marks: %s +%llu ms", name, (unsigned long long)ms);
    return ok ? 0 : -1;
}

int marks_count(const char *dir, const char *name)
{
    char path[700];
    sidecar(path, sizeof path, dir, name);
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    char line[64];
    int n = 0;
    while(fgets(line, sizeof line, f)){
        /* 只认"数字 + 可选空白"的行：断电写坏的行直接跳过 */
        char *p = line;
        while(*p == ' ' || *p == '\t') p++;
        if(*p < '0' || *p > '9') continue;
        while(*p >= '0' && *p <= '9') p++;
        while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if(*p == 0) n++;
    }
    fclose(f);
    return n;
}

int marks_has(const char *dir, const char *name)
{
    return marks_count(dir, name) > 0;
}

void marks_remove(const char *dir, const char *name)
{
    char path[700];
    sidecar(path, sizeof path, dir, name);
    if(unlink(path) == 0) logmsg("marks: removed %s.marks", name);
}
