/**
 ******************************************************************************
 * @file    buzzer.h
 * @brief   Passive buzzer on TIM3 PWM, gated by tick-based patterns.
 *
 *          Configure TIM3 in CubeMX:
 *              PSC = 71, ARR = 499  -> 2 kHz PWM @ 72 MHz SYSCLK
 *              Channel 3 = PWM mode 1, Pulse = 0 (starts silent)
 *
 *          Call Buzzer_Update() every ~10 ms for smooth patterns.
 ******************************************************************************
 */

#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUZZER_OFF  = 0,  /* silent                            */
    BUZZER_WARN = 1,  /* 250 ms tone per second (1 Hz)     */
    BUZZER_CRIT = 2   /* 100 ms on / 100 ms off (5 Hz)     */
} Buzzer_Mode_t;

void Buzzer_Init   (TIM_HandleTypeDef *htim, uint32_t channel);
void Buzzer_SetMode(Buzzer_Mode_t mode);
void Buzzer_Update (void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H */
