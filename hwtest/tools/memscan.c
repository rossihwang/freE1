#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: memscan pid keyword out.bin\n");return 2;}
    int pid=atoi(argv[1]); const char*kw=argv[2]; const char*out=argv[3];
    char mp[64]; snprintf(mp,sizeof mp,"/proc/%d/maps",pid);
    FILE*m=fopen(mp,"r"); if(!m){perror("maps");return 1;}
    char line[512]; int memfd=-1; long total=0; FILE*o=fopen(out,"wb");
    int kwlen=strlen(kw);
    while(fgets(line,sizeof line,m)){
        unsigned long a,b; char perms[8];
        if(sscanf(line,"%lx-%lx %7s",&a,&b,perms)!=3) continue;
        if(perms[0]!='r') continue;
        if(memfd<0){char p[64];snprintf(p,sizeof p,"/proc/%d/mem",pid);memfd=open(p,O_RDONLY);if(memfd<0){perror("mem");break;}}
        size_t len=b-a; unsigned char*buf=malloc(len);
        if(!buf) continue;
        ssize_t r=pread(memfd,buf,len,(off_t)a);
        if(r>0){
            for(ssize_t i=0;i+kwlen<(ssize_t)r;i++){
                if(memcmp(buf+i,kw,kwlen)==0){
                    unsigned long addr=a+i;
                    printf("hit %s @ %lx\n", kw, addr);
                    fwrite(buf+i,(size_t)((r-i)<8192? (r-i):8192),1,o);
                    total+=8192;
                }
            }
        }
        free(buf);
    }
    fclose(o); if(memfd>=0)close(memfd); fclose(m);
    fprintf(stderr,"dumped %ld bytes\n",total);
    return 0;
}
