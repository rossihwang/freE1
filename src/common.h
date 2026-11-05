#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

void logmsg(const char *fmt, ...);
static inline int64_t mono_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000; }
static inline uint32_t wall_sec(void){ return (uint32_t)time(NULL); }
