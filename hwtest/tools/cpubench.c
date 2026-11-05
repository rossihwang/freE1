#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
    long n = argc>1 ? atol(argv[1]) : 50000000L;
    volatile unsigned long acc=1;
    double t0=now();
    for(long i=0;i<n;i++){ acc=acc*6364136223846793005UL+1442695040888963407UL; acc^=acc>>17; }
    double t1=now();
    printf("iters=%ld time=%.3f s  acc=%lu\n", n, t1-t0, (unsigned long)acc);
    return 0;
}
