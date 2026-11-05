/* Measure page-flip completion latency with two same-colour dumb buffers,
 * then restore the original CRTC so the factory UI is not left broken. */
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
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

typedef struct { uint32_t handle; uint32_t fb; void *map; uint32_t size; } buf_t;

static int make_buf(int fd, buf_t *b)
{
    struct drm_mode_create_dumb d;
    memset(&d, 0, sizeof(d));
    d.width = 320; d.height = 320; d.bpp = 32;
    if(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d)) { perror("create"); return -1; }
    struct drm_mode_map_dumb m;
    memset(&m, 0, sizeof(m));
    m.handle = d.handle;
    if(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { perror("map"); return -1; }
    b->map = mmap(NULL, d.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    if(b->map == MAP_FAILED) { perror("mmap"); return -1; }
    memset(b->map, 0x20, d.size);
    struct drm_mode_fb_cmd f;
    memset(&f, 0, sizeof(f));
    f.width = 320; f.height = 320; f.pitch = d.pitch; f.bpp = 32; f.depth = 24; f.handle = d.handle;
    if(ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f)) { perror("addfb"); return -1; }
    b->handle = d.handle; b->fb = f.fb_id; b->size = d.size;
    return 0;
}

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_NONBLOCK);
    if(fd < 0) { perror("open"); return 1; }

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    uint32_t *crtcs = calloc(res.count_crtcs + 1, 4);
    uint32_t *conns = calloc(res.count_connectors + 1, 4);
    uint32_t *encs = calloc(res.count_encoders + 1, 4);
    uint32_t *fbs = calloc(res.count_fbs + 1, 4);
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    struct drm_mode_crtc orig;
    memset(&orig, 0, sizeof(orig));
    orig.crtc_id = crtcs[0];
    ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &orig);
    printf("orig crtc=%u fb=%u valid=%u\n", orig.crtc_id, orig.fb_id, orig.mode_valid);

    buf_t a, b;
    if(make_buf(fd, &a) || make_buf(fd, &b)) return 1;
    printf("buffers: A fb=%u B fb=%u\n", a.fb, b.fb);

    struct drm_mode_crtc set;
    memset(&set, 0, sizeof(set));
    set.crtc_id = orig.crtc_id;
    set.fb_id = a.fb;
    set.mode_valid = 1;
    memcpy(&set.mode, &orig.mode, sizeof(set.mode));
    set.count_connectors = 1;
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&conns[0];
    int r = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set);
    printf("SETCRTC -> A rc=%d errno=%d\n", r, r ? errno : 0);
    usleep(50000);

    int cur = 0;
    for(int i = 0; i < 10; i++) {
        uint32_t fb = (cur ? a.fb : b.fb);
        struct drm_mode_crtc_page_flip flip;
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = orig.crtc_id;
        flip.fb_id = fb;
        flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
        unsigned long long t0 = now_ns();
        r = ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
        if(r) { printf("flip %d rc=%d errno=%d\n", i, r, errno); break; }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, 500);
        char evb[128];
        ssize_t n = (pr > 0 && (p.revents & POLLIN)) ? read(fd, evb, sizeof(evb)) : -1;
        unsigned long long dt = now_ns() - t0;
        if(n > 0) {
            struct drm_event *ev = (struct drm_event *)evb;
            printf("flip %d -> %.2f ms (event type %u)\n", i, dt / 1e6, ev->type);
        }
        else printf("flip %d no event poll=%d errno=%d\n", i, pr, errno);
        cur = !cur;
    }

    /* restore the factory CRTC */
    struct drm_mode_crtc back;
    memset(&back, 0, sizeof(back));
    back.crtc_id = orig.crtc_id;
    back.fb_id = orig.fb_id;
    back.mode_valid = orig.mode_valid;
    memcpy(&back.mode, &orig.mode, sizeof(back.mode));
    back.count_connectors = 1;
    back.set_connectors_ptr = (uint64_t)(uintptr_t)&conns[0];
    r = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &back);
    printf("restore SETCRTC rc=%d errno=%d\n", r, r ? errno : 0);
    close(fd);
    return 0;
}
