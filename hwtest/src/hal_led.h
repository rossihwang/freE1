#ifndef E1_HAL_LED_H
#define E1_HAL_LED_H

#include <stdbool.h>

/* Top RGB LED: /proc/awled_{enable,color,pwm}
 *   color 0..6 = off, red, green, blue, yellow, purple, cyan
 *   pwm   0..255
 * Key LED: /proc/leds_gpio 0/1 (single red LED next to the REC key) */
void led_init(void);
void led_top(bool on);
void led_color(int c);
void led_pwm(int v);
void led_key(bool on);
int  led_color_get(void);
int  led_pwm_get(void);
bool led_top_get(void);
bool led_key_get(void);

#endif
