#ifndef E1_HAL_HP_H
#define E1_HAL_HP_H

#include <stdbool.h>

/* Headphone jack: /dev/input/event2 carries EV_SW SW_HEADPHONE_INSERT.
 * The initial state is read with EVIOCGSW() because the switch event only
 * fires on change. /proc/headset_inserted is maintained by the factory app
 * and cannot be trusted once it has been replaced. */
void hp_init(void);
void hp_poll(void);
bool hp_inserted(void);

#endif
