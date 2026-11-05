#ifndef E1_HAL_MOTOR_H
#define E1_HAL_MOTOR_H

#include <stdbool.h>

/* Vibration motor: /proc/motor_vibr 0/1.
 * /proc/motor_time is a trap: writing it can inhibit vibration, so we only
 * ever toggle motor_vibr and time the pulses in software. */
void motor_init(void);
void motor_pulse(int on_ms);       /* single pulse */
void motor_pattern(int kind);      /* 0 short, 1 double, 2 long */
void motor_poll(void);
bool motor_busy(void);
int  motor_patterns_done(void);
void motor_reset(void);

#endif
