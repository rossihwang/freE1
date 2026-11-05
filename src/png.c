#include "png.h"
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static void be32(uint8_t *p, uint32_t v){ p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len){
    uint8_t b[8]; be32(b,len); memcpy(b+4,type,4);
    fwrite(b,1,8,f);
    if(len) fwrite(data,1,len,f);
    uLong c = crc32(0L,Z_NULL,0);
    c = crc32(c,(const Bytef*)type,4);
    if(len) c = crc32(c,data,len);
    be32(b,c); fwrite(b,1,4,f);
}

int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h){
    size_t rawlen = (size_t)h*(1+(size_t)w*3);
    uint8_t *raw = malloc(rawlen);
    if(!raw) return -1;
    for(int y=0;y<h;y++){
        raw[(size_t)y*(1+(size_t)w*3)] = 0;
        memcpy(raw+(size_t)y*(1+(size_t)w*3)+1, rgb+(size_t)y*w*3, (size_t)w*3);
    }
    uLongf clen = compressBound(rawlen);
    uint8_t *comp = malloc(clen);
    if(!comp || compress2(comp,&clen,raw,rawlen,6)!=Z_OK){ free(raw); free(comp); return -1; }
    FILE *f=fopen(path,"wb"); if(!f){ free(raw); free(comp); return -1; }
    static const uint8_t sig[8]={137,80,78,71,13,10,26,10};
    fwrite(sig,1,8,f);
    uint8_t ihdr[13]; be32(ihdr,(uint32_t)w); be32(ihdr+4,(uint32_t)h);
    ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    chunk(f,"IHDR",ihdr,13);
    chunk(f,"IDAT",comp,(uint32_t)clen);
    chunk(f,"IEND",NULL,0);
    fclose(f); free(raw); free(comp);
    return 0;
}
