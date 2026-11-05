/* Probe the E1 display stack for tear-free options:
 *   1. /dev/fb0 FBIO_WAITFORVSYNC support + measured vblank period
 *   2. /dev/dri/card0: master, resources, dumb buffer, setcrtc, page flip
 */
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static void fb_probe(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if(fd < 0) { perror("fb open"); return; }

    unsigned int arg = 0;
    errno = 0;
    int r = ioctl(fd, FBIO_WAITFORVSYNC, &arg);
    printf("[fb] FBIO_WAITFORVSYNC rc=%d errno=%d(%s)\n", r, r ? errno : 0, r ? strerror(errno) : "ok");

    if(r == 0) {
        unsigned long long t0 = now_ns();
        for(int i = 0; i < 10; i++) {
            int rr = ioctl(fd, FBIO_WAITFORVSYNC, &arg);
            if(rr) { printf("[fb] wait %d failed errno=%d\n", i, errno); break; }
        }
        unsigned long long dt = now_ns() - t0;
        printf("[fb] 10 waits took %.1f ms -> %.2f Hz\n", dt / 1e6, 10.0e9 / (double)dt);
    }
    close(fd);
}

static int drm_probe(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if(fd < 0) { perror("[drm] open"); return -1; }

    struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
    printf("[drm] GET_CAP dumb: rc=%d value=%u\n", ioctl(fd, DRM_IOCTL_GET_CAP, &cap), cap.value);

    int r = ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
    printf("[drm] SET_MASTER rc=%d errno=%d(%s)\n", r, r ? errno : 0, r ? strerror(errno) : "ok");

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    r = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    printf("[drm] GETRESOURCES rc=%d fbs=%u crtcs=%u conns=%u encs=%u\n",
           r, res.count_fbs, res.count_crtcs, res.count_connectors, res.count_encoders);
    if(r) return -1;

    uint32_t *crtcs = calloc(res.count_crtcs + 1, 4);
    uint32_t *conns = calloc(res.count_connectors + 1, 4);
    uint32_t *encs  = calloc(res.count_encoders + 1, 4);
    uint32_t *fbs   = calloc(res.count_fbs + 1, 4);
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    r = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    printf("[drm] GETRESOURCES(2) rc=%d crtc0=%u conn0=%u\n", r, crtcs[0], conns[0]);

    struct drm_mode_get_connector conn;
    memset(&conn, 0, sizeof(conn));
    conn.connector_id = conns[0];
    ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
    struct drm_mode_modeinfo *modes = calloc(conn.count_modes + 1, sizeof(*modes));
    uint32_t *cenc = calloc(conn.count_encoders + 1, 4);
    conn.modes_ptr = (uint64_t)(uintptr_t)modes;
    conn.encoders_ptr = (uint64_t)(uintptr_t)cenc;
    ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
    printf("[drm] connector %u type=%u status=%u modes=%u enc=%u\n",
           conn.connector_id, conn.connector_type, conn.connection, conn.count_modes, conn.count_encoders);
    if(conn.count_modes)
        printf("[drm] mode0: %s %ux%u clock=%u\n", modes[0].name, modes[0].hdisplay, modes[0].vdisplay, modes[0].clock);

    struct drm_mode_create_dumb dumb;
    memset(&dumb, 0, sizeof(dumb));
    dumb.width = 320; dumb.height = 320; dumb.bpp = 32;
    r = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
    printf("[drm] CREATE_DUMB rc=%d handle=%u pitch=%u size=%llu errno=%d\n",
           r, dumb.handle, dumb.pitch, (unsigned long long)dumb.size, r ? errno : 0);
    if(r) return -1;

    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = dumb.handle;
    r = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    printf("[drm] MAP_DUMB rc=%d offset=%llu errno=%d\n", r, (unsigned long long)map.offset, r ? errno : 0);
    void *p = MAP_FAILED;
    if(!r) {
        p = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
        printf("[drm] mmap=%p errno=%d\n", p, p == MAP_FAILED ? errno : 0);
    }

    struct drm_mode_fb_cmd fbcmd;
    memset(&fbcmd, 0, sizeof(fbcmd));
    fbcmd.width = 320; fbcmd.height = 320; fbcmd.pitch = dumb.pitch;
    fbcmd.bpp = 32; fbcmd.depth = 24; fbcmd.handle = dumb.handle;
    r = ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbcmd);
    printf("[drm] ADDFB rc=%d fb_id=%u errno=%d\n", r, fbcmd.fb_id, r ? errno : 0);

    if(p != MAP_FAILED) {
        memset(p, 0x20, dumb.size);
        printf("[drm] buffer painted\n");
    }

    struct drm_mode_crtc crtc;
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtcs[0];
    crtc.fb_id = fbcmd.fb_id;
    if(conn.count_modes) {
        crtc.mode_valid = 1;
        memcpy(&crtc.mode, &modes[0], sizeof(crtc.mode));
        crtc.count_connectors = 1;
        crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn.connector_id;
    }
    r = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
    printf("[drm] SETCRTC rc=%d errno=%d(%s)\n", r, r ? errno : 0, r ? strerror(errno) : "ok");

    struct drm_mode_crtc_page_flip flip;
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = crtcs[0];
    flip.fb_id = fbcmd.fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    flip.user_data = 0x1234;
    r = ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
    printf("[drm] PAGE_FLIP rc=%d errno=%d(%s)\n", r, r ? errno : 0, r ? strerror(errno) : "ok");

    if(r == 0) {
        struct drm_event ev;
        ssize_t n = read(fd, &ev, sizeof(ev));
        printf("[drm] flip event read=%zd type=%u\n", n, n > 0 ? ev.type : 0);
    }

    if(r == 0) {
        struct drm_mode_crtc_page_flip f2;
        memset(&f2, 0, sizeof(f2));
        f2.crtc_id = crtcs[0];
        f2.fb_id = fbcmd.fb_id;
        f2.flags = DRM_MODE_PAGE_FLIP_EVENT;
        int rr = ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &f2);
        printf("[drm] second flip rc=%d\n", rr);
        (void)rr;
    }
    close(fd);
    return 0;
}

int main(void)
{
    fb_probe();
    drm_probe();
    return 0;
}
