/* display_drm.c - tear-free DRM/KMS backend for the E1 (no libdrm). */

#include "display_drm.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define W 320
#define H 320
#define FRAME_SIZE (W * H * 4)

typedef struct {
    uint32_t handle;
    uint32_t fb;
    uint8_t *map;
    uint32_t size;
} drm_buf_t;

static int g_fd = -1;
static lv_display_t *g_disp;
static uint8_t *g_shadow;
static drm_buf_t g_buf[2];
static int g_back = 1;                 /* first frame goes to buf[1] */
static uint32_t g_flips, g_fps_ms;

static struct drm_mode_crtc g_orig;
static uint32_t g_crtc_id, g_conn_id;
static bool g_saved;
static bool g_created;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static int make_buf(drm_buf_t *b)
{
    struct drm_mode_create_dumb d;
    memset(&d, 0, sizeof(d));
    d.width = W; d.height = H; d.bpp = 32;
    if(ioctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &d)) { perror("drm: create_dumb"); return -1; }

    struct drm_mode_map_dumb m;
    memset(&m, 0, sizeof(m));
    m.handle = d.handle;
    if(ioctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { perror("drm: map_dumb"); return -1; }

    b->map = mmap(NULL, d.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, m.offset);
    if(b->map == MAP_FAILED) { perror("drm: mmap"); return -1; }
    memset(b->map, 0, d.size);

    struct drm_mode_fb_cmd f;
    memset(&f, 0, sizeof(f));
    f.width = W; f.height = H; f.pitch = d.pitch; f.bpp = 32; f.depth = 24; f.handle = d.handle;
    if(ioctl(g_fd, DRM_IOCTL_MODE_ADDFB, &f)) { perror("drm: addfb"); return -1; }

    b->handle = d.handle;
    b->fb = f.fb_id;
    b->size = d.size;
    return 0;
}

static void set_crtc(uint32_t fb, const struct drm_mode_modeinfo *mode, bool valid)
{
    struct drm_mode_crtc c;
    memset(&c, 0, sizeof(c));
    c.crtc_id = g_crtc_id;
    c.fb_id = fb;
    if(valid) {
        c.mode_valid = 1;
        memcpy(&c.mode, mode, sizeof(c.mode));
        c.count_connectors = 1;
        c.set_connectors_ptr = (uint64_t)(uintptr_t)&g_conn_id;
    }
    if(ioctl(g_fd, DRM_IOCTL_MODE_SETCRTC, &c))
        fprintf(stderr, "drm: setcrtc(fb=%u) failed: %s\n", fb, strerror(errno));
}

void e1_drm_restore(void)
{
    if(g_fd >= 0 && g_saved) {
        set_crtc(g_orig.fb_id, &g_orig.mode, g_orig.mode_valid != 0);
        fprintf(stderr, "drm: crtc restored (fb=%u)\n", g_orig.fb_id);
        g_saved = false;
    }
}

static void on_signal(int sig)
{
    (void)sig;
    e1_drm_restore();
    /* let the default action continue */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* LVGL in single-buffer mode busy-waits on disp->flushing inside its own
 * refresh, so an asynchronous flip can never be acknowledged from the main
 * loop (deadlock). The supported way is flush_wait_cb: while LVGL waits for the
 * previous flush it blocks here until the page flip completes. */
static bool g_flip_queued;
static uint32_t g_pace_ms = 40;      /* real panel is 25.56 Hz -> 39.1 ms */
static uint32_t g_last_flip_ms;

static void drain_flip_events(void)
{
    char buf[256];
    ssize_t n = read(g_fd, buf, sizeof(buf));
    if(n <= 0) return;
    size_t off = 0;
    while(off + sizeof(struct drm_event) <= (size_t)n) {
        struct drm_event *ev = (struct drm_event *)(buf + off);
        if(ev->type == DRM_EVENT_FLIP_COMPLETE) g_flip_queued = false;
        if(ev->length < sizeof(struct drm_event)) break;
        off += ev->length;
    }
}

static void wait_cb(lv_display_t *disp)
{
    (void)disp;
    if(!g_flip_queued) return;

    /* The driver reports flip completion every ~11.7 ms (fake vblank), but the
     * panel really scans at 25.56 Hz. Trust the panel timing: do not touch the
     * other buffer until one full panel frame has elapsed. Drain events so the
     * queue cannot grow. */
    uint32_t target = g_last_flip_ms + g_pace_ms;
    for(;;) {
        uint32_t t = now_ms();
        if((int32_t)(t - target) >= 0) break;
        uint32_t left = target - t;
        struct pollfd p = { .fd = g_fd, .events = POLLIN };
        int pr = poll(&p, 1, (int)(left > 10 ? 10 : left));
        if(pr > 0 && (p.revents & POLLIN)) drain_flip_events();
    }
    drain_flip_events();
    g_flip_queued = false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;

    if(g_fd < 0 || !g_buf[g_back].map) return;

    memcpy(g_buf[g_back].map, g_shadow, FRAME_SIZE);

    struct drm_mode_crtc_page_flip flip;
    memset(&flip, 0, sizeof(flip));
    flip.crtc_id = g_crtc_id;
    flip.fb_id = g_buf[g_back].fb;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;

    if(ioctl(g_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip)) {
        fprintf(stderr, "drm: page flip failed: %s\n", strerror(errno));
        return;
    }

    g_flip_queued = true;
    g_last_flip_ms = now_ms();
    g_flips++;
    g_back ^= 1;
}

void e1_drm_poll(void)
{
    if(g_fd < 0 || !g_created) return;

    if(g_fps_ms == 0) g_fps_ms = now_ms();
    uint32_t t = now_ms();
    if(t - g_fps_ms >= 5000) {
        fprintf(stderr, "drm: %.1f fps, queued=%d\n",
                g_flips * 1000.0 / (double)(t - g_fps_ms), (int)g_flip_queued);
        g_flips = 0;
        g_fps_ms = t;
    }
}

const uint8_t *e1_drm_frame(void)
{
    return g_shadow;
}

lv_display_t *e1_drm_create(const char *card)
{
    g_fd = open(card, O_RDWR | O_NONBLOCK);
    if(g_fd < 0) { perror("drm: open"); return NULL; }

    struct drm_mode_card_res res;
    uint32_t crtcs[8] = { 0 }, conns[8] = { 0 }, encs[8] = { 0 }, fbs[8] = { 0 };
    memset(&res, 0, sizeof(res));
    res.count_crtcs = 8;
    res.count_connectors = 8;
    res.count_encoders = 8;
    res.count_fbs = 8;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    int r2 = ioctl(g_fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    fprintf(stderr, "drm: getres rc=%d crtcs=%u conns=%u encs=%u\n",
            r2, res.count_crtcs, res.count_connectors, res.count_encoders);
    if(r2 || res.count_crtcs == 0 || res.count_connectors == 0) { close(g_fd); g_fd = -1; return NULL; }

    g_crtc_id = crtcs[0];
    g_conn_id = conns[0];

    struct drm_mode_get_connector conn;
    struct drm_mode_modeinfo modes[32];
    uint32_t cenc[8] = { 0 };
    memset(&conn, 0, sizeof(conn));
    memset(modes, 0, sizeof(modes));
    conn.connector_id = g_conn_id;
    conn.count_modes = 32;
    conn.modes_ptr = (uint64_t)(uintptr_t)modes;
    conn.count_encoders = 8;
    conn.encoders_ptr = (uint64_t)(uintptr_t)cenc;
    int r3 = ioctl(g_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
    fprintf(stderr, "drm: getconn rc=%d id=%u conn=%u modes=%u errno=%d\n",
            r3, g_conn_id, conn.connection, conn.count_modes, r3 ? errno : 0);
    if(r3 || conn.count_modes == 0) {
        fprintf(stderr, "drm: no modes\n");
        close(g_fd); g_fd = -1;
        return NULL;
    }

    struct drm_mode_crtc orig;
    memset(&orig, 0, sizeof(orig));
    orig.crtc_id = g_crtc_id;
    if(ioctl(g_fd, DRM_IOCTL_MODE_GETCRTC, &orig) == 0) {
        memcpy(&g_orig, &orig, sizeof(orig));
        g_saved = true;
    }

    if(make_buf(&g_buf[0]) || make_buf(&g_buf[1])) { close(g_fd); g_fd = -1; return NULL; }

    g_shadow = calloc(1, FRAME_SIZE);
    if(!g_shadow) { close(g_fd); g_fd = -1; return NULL; }

    set_crtc(g_buf[0].fb, &modes[0], true);
    usleep(30000);

    FILE *pf = fopen("/mnt/sdcard/e1hwtest/pace_ms", "r");
    if(pf) {
        int v = 0;
        if(fscanf(pf, "%d", &v) == 1 && v >= 20 && v <= 200) g_pace_ms = (uint32_t)v;
        fclose(pf);
    }

    g_disp = lv_display_create(W, H);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(g_disp, g_shadow, NULL, FRAME_SIZE, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(g_disp, flush_cb);
    lv_display_set_flush_wait_cb(g_disp, wait_cb);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    g_created = true;
    fprintf(stderr, "drm: %s crtc=%u conn=%u mode=%ux%u fb=%u/%u pace=%ums\n",
            card, g_crtc_id, g_conn_id, modes[0].hdisplay, modes[0].vdisplay,
            g_buf[0].fb, g_buf[1].fb, (unsigned)g_pace_ms);
    return g_disp;
}
