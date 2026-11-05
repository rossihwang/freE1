#pragma once
#include <stdbool.h>

/* 搜狗 E1 的 sysfs/procfs 设备操作，freE1 与 e1hwtest 共用。
   只做“读写一个文件”的事，不含任何 UI/业务逻辑。 */

/* ---- 顶部 RGB LED：/proc/awled_{enable,color,pwm} ---- */
void hs_led_top(bool on);
void hs_led_color(int c);       /* 0..6 = off,red,green,blue,yellow,purple,cyan */
void hs_led_pwm(int v);         /* 0..255 */
int  hs_led_color_get(void);
int  hs_led_pwm_get(void);
bool hs_led_top_get(void);

/* ---- 按键红灯：/proc/leds_gpio ---- */
void hs_led_key(bool on);
bool hs_led_key_get(void);

/* ---- 马达：/proc/motor_vibr（只切换 0/1，时间在软件里算） ---- */
void hs_motor(bool on);
void hs_motor_pattern(int kind);   /* 0 short, 1 double, 2 long */
void hs_motor_poll(void);
bool hs_motor_busy(void);
int  hs_motor_done(void);
void hs_motor_reset(void);

/* ---- 背光：/sys/class/backlight/backlight/brightness ---- */
int  hs_backlight_set(int v);
int  hs_backlight_get(void);

/* ---- 耳机：/proc/headset_inserted（原厂 App 维护，替换后可能不可信） ---- */
int  hs_hp_inserted(void);

/* ---- 电池 ---- */
int  hs_battery_capacity(void);
int  hs_battery_voltage_uv(void);
int  hs_battery_charging(void);

/* ---- CPU 频点上限 ---- */
void hs_cpu_max_freq(int khz);
