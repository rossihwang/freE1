#ifndef E1_HAL_KEYS_H
#define E1_HAL_KEYS_H

#include <stdbool.h>
#include <stdint.h>

/* 6 physical keys of the Sogou E1:
 *   KEY_1 = MARK  (left)     KEY_2 = STOP (middle)   KEY_3 = REC (right)
 *   KEY_4 = VOL+             KEY_5 = VOL-            KEY_6 = POWER (PMIC)
 * KEY_1..5 arrive on /dev/input/event3 (adc-keys), KEY_6 on event1.
 */
typedef enum {
    E1K_MARK = 0,
    E1K_STOP,
    E1K_REC,
    E1K_VOLUP,
    E1K_VOLDN,
    E1K_PWR,
    E1K_COUNT
} e1_key_t;

void        keys_init(void);
void        keys_poll(void);
bool        keys_down(e1_key_t k);
int         keys_count(e1_key_t k);
uint32_t    keys_hold_ms(e1_key_t k);   /* live hold, 0 when released */
uint32_t    keys_last_hold(e1_key_t k);
const char *keys_name(e1_key_t k);
void        keys_reset(void);
uint32_t    e1_now_ms(void);

#endif
