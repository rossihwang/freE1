#include "common.h"
void logmsg(const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    struct timeval tv; gettimeofday(&tv,NULL);
    fprintf(stderr,"[%02d:%02d:%02d.%03d] ", (int)(tv.tv_sec/3600%24),(int)(tv.tv_sec/60%60),(int)(tv.tv_sec%60),(int)(tv.tv_usec/1000));
    vfprintf(stderr,fmt,ap); fputc('\n',stderr); fflush(stderr);
    va_end(ap);
}
