/* Probe vblank timing and page-flip events without disturbing the scanout:
 * flips to the current fb only. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_NONBLOCK);
    if(fd < 0) { perror("open"); return 1; }

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getres"); return 1; }
    uint32_t *crtcs = calloc(res.count_crtcs + 1, 4);
    uint32_t *conns = calloc(res.count_connectors + 1, 4);
    uint32_t *encs = calloc(res.count_encoders + 1, 4);
    uint32_t *fbs = calloc(res.count_fbs + 1, 4);
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    struct drm_mode_crtc crtc;
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtcs[0];
    if(ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc)) perror("getcrtc");
    printf("crtc=%u fb=%u valid=%u %ux%u clock=%u\n",
           crtc.crtc_id, crtc.fb_id, crtc.mode_valid, crtc.mode.hdisplay, crtc.mode.vdisplay, crtc.mode.clock);

    union drm_wait_vblank vbl;
    memset(&vbl, 0, sizeof(vbl));
    vbl.request.type = _DRM_VBLANK_RELATIVE;
    unsigned long long t0 = now_ns();
    int ok = 0;
    for(int i = 0; i < 10; i++) {
        vbl.request.sequence = 1;
        if(ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl) == 0) ok++;
    }
    unsigned long long dt = now_ns() - t0;
    printf("WAIT_VBLANK ok=%d/10  %.1f ms/event -> %.2f Hz\n", ok, dt / 1e6 / 10.0, 10.0e9 / (double)dt);

    /* event at next vblank */
    memset(&vbl, 0, sizeof(vbl));
    vbl.request.type = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT;
    int r = ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
    printf("WAIT_VBLANK|EVENT rc=%d errno=%d\n", r, r ? errno : 0);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 500);
    char evbuf[128];
    ssize_t n = (pr > 0 && (pfd.revents & POLLIN)) ? read(fd, evbuf, sizeof(evbuf)) : -1;
    if(n > 0) {
        struct drm_event *ev = (struct drm_event *)evbuf;
        struct drm_event_vblank *vb = (struct drm_event_vblank *)evbuf;
        printf("event: poll=%d read=%zd type=%u len=%u seq=%u\n", pr, n, ev->type, ev->length, vb->sequence);
    }
    else {
        printf("event: poll=%d revents=0x%x read=%zd errno=%d\n", pr, pfd.revents, n, errno);
    }

    /* queue 10 vblank events and measure the real refresh period */
    {
        unsigned long long ts[12]; int nts = 0;
        for(int i = 0; i < 10; i++) {
            union drm_wait_vblank ev;
            memset(&ev, 0, sizeof(ev));
            ev.request.type = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT;
            ev.request.sequence = 1;
            if(ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &ev)) { printf("vbl event ioctl fail %d\n", errno); break; }
            struct pollfd p2 = { .fd = fd, .events = POLLIN };
            if(poll(&p2, 1, 500) <= 0) { printf("vbl poll timeout\n"); break; }
            char b[128];
            ssize_t r2 = read(fd, b, sizeof(b));
            if(r2 < (ssize_t)sizeof(struct drm_event_vblank)) break;
            struct drm_event_vblank *vb = (struct drm_event_vblank *)b;
            if(vb->base.type != DRM_EVENT_VBLANK) continue;
            unsigned long long t = (unsigned long long)vb->tv_sec * 1000000ull + vb->tv_usec;
            ts[nts++] = t;
        }
        if(nts >= 2) {
            double total = (double)(ts[nts - 1] - ts[0]);
            printf("vblank period: %d events over %.2f ms -> %.3f ms (%.2f Hz)\n",
                   nts, total / 1000.0, total / 1000.0 / (nts - 1), (nts - 1) * 1e6 / total);
        }
    }

    /* flip to the fb that is already scanout: should be harmless */
    if(crtc.fb_id) {
        struct drm_mode_crtc_page_flip flip;
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = crtc.crtc_id;
        flip.fb_id = crtc.fb_id;
        flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
        r = ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
        printf("PAGE_FLIP(same fb) rc=%d errno=%d\n", r, r ? errno : 0);
        if(r == 0) {
            pr = poll(&pfd, 1, 500);
            n = (pr > 0 && (pfd.revents & POLLIN)) ? read(fd, evbuf, sizeof(evbuf)) : -1;
            if(n > 0) {
                struct drm_event *ev = (struct drm_event *)evbuf;
                printf("flip event: read=%zd type=%u\n", n, ev->type);
            }
            else {
                printf("flip event: poll=%d revents=0x%x read=%zd errno=%d\n", pr, pfd.revents, n, errno);
            }
        }
    }
    close(fd);
    return 0;
}
