#include "hal_hp.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/input.h>

static int g_fd = -1;
static bool g_in;

void hp_init(void)
{
    g_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    if(g_fd < 0) {
        perror("hp: event2");
        return;
    }

    unsigned long sw = 0;
    if(ioctl(g_fd, EVIOCGSW(sizeof(sw)), &sw) == (int)sizeof(sw)) {
        g_in = (sw & (1UL << SW_HEADPHONE_INSERT)) != 0;
    }
    fprintf(stderr, "hp: init state %s\n", g_in ? "IN" : "OUT");
}

void hp_poll(void)
{
    if(g_fd < 0) return;
    struct input_event ev;
    while(read(g_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if(ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT) {
            g_in = ev.value != 0;
        }
    }
}

bool hp_inserted(void)
{
    return g_in;
}
