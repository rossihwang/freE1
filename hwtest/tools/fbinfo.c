#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
int main(void){
    int fd=open("/dev/fb0",O_RDWR);
    if(fd<0){perror("open");return 1;}
    struct fb_fix_screeninfo f; struct fb_var_screeninfo v;
    memset(&f,0,sizeof f); memset(&v,0,sizeof v);
    printf("ioctl fix=%d var=%d\n", ioctl(fd,FBIOGET_FSCREENINFO,&f), ioctl(fd,FBIOGET_VSCREENINFO,&v));
    printf("id=%s smem_start=%lx smem_len=%u line_length=%u accel=%u\n", f.id,(unsigned long)f.smem_start,f.smem_len,f.line_length,f.accel);
    printf("xres=%u yres=%u xres_v=%u yres_v=%u bpp=%u\n", v.xres,v.yres,v.xres_virtual,v.yres_virtual,v.bits_per_pixel);
    printf("rgba %u/%u %u/%u %u/%u %u/%u\n", v.red.offset,v.red.length,v.green.offset,v.green.length,v.blue.offset,v.blue.length,v.transp.offset,v.transp.length);
    unsigned len = f.smem_len ? f.smem_len : 320u*320u*4u;
    void *p = mmap(0, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    printf("mmap(len=%u)=%p errno=%d(%s)\n", len, p, p==MAP_FAILED?errno:0, p==MAP_FAILED?strerror(errno):"ok");
    if(p!=MAP_FAILED){ printf("first word=%08x\n", *(unsigned*)p); munmap(p,len);}
    return 0;
}
