#include "hal.h"
#include "lvgl.h"
#include "common.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <time.h>
#include <errno.h>

#define W 320
#define H 320

static int g_fd = -1;
static struct fb_fix_screeninfo g_fi;
static struct fb_var_screeninfo g_vi;
static uint32_t g_shadow[W*H];
static int g_flush_on = 1;
void hal_disp_set_flush_enabled(int on){ g_flush_on = on; }
static uint8_t  g_buf[W*40*4];   /* 1/8 屏绘制缓冲 */

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px){
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    if(!g_flush_on){ lv_display_flush_ready(d); return; }   /* 熄屏：不写 fb，省电 */
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    if(w <= 0 || h <= 0){ lv_display_flush_ready(d); return; }
    for(int y=0;y<h;y++)
        memcpy(&g_shadow[(size_t)(a->y1+y)*W + a->x1], px + (size_t)y*w*4, (size_t)w*4);

    /* 把脏区扩成整行带宽，一次性 pwrite：
       行内数据在 memory 里是连续的，一条 write 取代原来的 h 条，
       在 25 Hz 面板 + 慢速 fb0 上这是主要的延迟来源。 */
    off_t off = (off_t)a->y1 * g_fi.line_length;
    size_t len = (size_t)h * g_fi.line_length;
    if(pwrite(g_fd, &g_shadow[(size_t)a->y1*W], len, off) < 0){
        for(int y=0;y<h;y++){
            off_t o2 = (off_t)(a->y1+y)*g_fi.line_length;
            pwrite(g_fd, &g_shadow[(size_t)(a->y1+y)*W], (size_t)W*4, o2);
        }
    }
    lv_display_flush_ready(d);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us = (t1.tv_sec-t0.tv_sec)*1000000L + (t1.tv_nsec-t0.tv_nsec)/1000L;
    { static long acc=0, area=0; static int n=0; static int64_t last=0;
      acc+=us; area+=(long)w*h; n++;
      if(n>=30){
          int64_t now=mono_ms();
          /* 睡眠/让出显示后 last 会是很久以前的值，直接算会得出"frame gap 6159 ms"这种假数据 */
          long gap = (last && now-last < 60000) ? (long)(now-last)/30 : 0;
          last=now;
          logmsg("fb: flush avg %ld us, area avg %ld px, frame gap %ld ms", acc/n, area/n, gap);
          acc=0; area=0; n=0;
      } }
}

/* 从别的程序手里收回显示：重新 pan 到 fb0 并整屏重画 */
void hal_disp_fb_reclaim(void){
    if(g_fd < 0) return;
    struct fb_var_screeninfo v = g_vi;
    v.yoffset = 0; v.xoffset = 0; v.activate = FB_ACTIVATE_NOW;
    /* 先重设 var（强制把 CRTC 切回 fb0），再 pan + unblank */
    ioctl(g_fd, FBIOPUT_VSCREENINFO, &v);
    ioctl(g_fd, FBIOPAN_DISPLAY, &v);
    ioctl(g_fd, FBIOBLANK, FB_BLANK_UNBLANK);
    g_flush_on = 1;
    logmsg("fb0: reclaimed");
}

lv_display_t *hal_disp_fb_init(void){
    g_fd = open("/dev/fb0", O_RDWR);
    if(g_fd < 0){ logmsg("fb0 open failed"); return NULL; }
    if(ioctl(g_fd, FBIOGET_FSCREENINFO, &g_fi) < 0 || ioctl(g_fd, FBIOGET_VSCREENINFO, &g_vi) < 0){
        logmsg("fb0 ioctl failed"); close(g_fd); g_fd=-1; return NULL;
    }
    logmsg("fb0: %ux%u virt %ux%u bpp=%u line=%u", g_vi.xres,g_vi.yres,g_vi.xres_virtual,g_vi.yres_virtual,
           g_vi.bits_per_pixel, g_fi.line_length);
    if(g_vi.bits_per_pixel != 32){ logmsg("fb0: expect 32bpp"); close(g_fd); g_fd=-1; return NULL; }

    /* 强制把 VOP 切到 fb0：某些启动流程下内核仍在扫描 u-boot 的 logo 缓冲，
       不 pan 一次屏幕就一直是 logo。
       开机早期 pan 可能返回 EBUSY(16)——VOP 还没准备好吃新配置——
       所以先重设 var，再重试几次，别只试一次就放弃。 */
    {
        struct fb_var_screeninfo v = g_vi;
        v.yoffset = 0; v.xoffset = 0; v.activate = FB_ACTIVATE_NOW;
        ioctl(g_fd, FBIOPUT_VSCREENINFO, &v);
        /* 实测这台机器开机时 pan 一直 EBUSY(16)：VOP 不接受这个 ioctl。
           显示仍然正常（直接写 fb0，再 unblank 就够了），所以只试两次、
           并且只记一条，免得每次启动刷五行没用的日志。 */
        int ok = 0;
        for(int i = 0; i < 2 && !ok; i++){
            if(ioctl(g_fd, FBIOPAN_DISPLAY, &v) == 0) ok = 1;
            else usleep(30000);
        }
        if(ok) logmsg("fb0: pan to yoffset=0");
        else   logmsg("fb0: pan unsupported (%d), relying on var+unblank", errno);
        if(ioctl(g_fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) logmsg("fb0: unblank failed (%d)", errno);
        else logmsg("fb0: unblanked");
    }

    lv_display_t *d = lv_display_create(W,H);
    lv_display_set_flush_cb(d,flush_cb);
    lv_display_set_buffers(d,g_buf,NULL,sizeof g_buf,LV_DISPLAY_RENDER_MODE_PARTIAL);
    return d;
}
