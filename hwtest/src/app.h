#ifndef E1_APP_H
#define E1_APP_H

#include <stdbool.h>

#define ITEM_COUNT 5

typedef enum {
    ITEM_TOUCH = 0,
    ITEM_KEYS,
    ITEM_LOOP,
    ITEM_MICS,
    ITEM_VIB,
} item_id_t;

typedef enum {
    VERDICT_NONE = 0,
    VERDICT_PASS,
    VERDICT_FAIL,
} verdict_t;

typedef struct {
    verdict_t verdict;
    char evidence[192];
} item_result_t;

typedef struct {
    item_result_t items[ITEM_COUNT];
    int page;   /* 0..ITEM_COUNT = summary page at ITEM_COUNT */
    bool hp_in;
} app_state_t;

extern app_state_t g_app;

/* latest rendered frame, BGRA (320*320*4), maintained by the display backend */
extern const unsigned char *g_display_frame;

#endif
