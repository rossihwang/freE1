#ifndef E1_INPUT_H
#define E1_INPUT_H

#include <stdint.h>
#include <lvgl.h>

/* Sogou E1 touch panel: st16xx, MT protocol B only (no ABS_X/ABS_Y).
 * Slots: 0..1 (2 fingers). Two-finger vertical swipe = page change,
 * single finger is forwarded to LVGL untouched. */

typedef struct {
    int x;              /* last primary contact position */
    int y;
    int contacts;       /* active contacts now */
    int max_contacts;   /* max seen since reset */
    int cells;          /* 8x8 coverage cells hit since reset */
} e1_input_stats_t;

int  e1_input_init(const char *dev);
void e1_input_close(void);
void e1_input_poll(void);
void e1_input_read(lv_indev_t *indev, lv_indev_data_t *data);
int  e1_input_take_swipe(void);   /* -1 = prev, +1 = next, 0 = none */
void e1_input_reset_stats(void);
const e1_input_stats_t *e1_input_stats(void);
int  e1_input_cell_mask(void);

#endif
