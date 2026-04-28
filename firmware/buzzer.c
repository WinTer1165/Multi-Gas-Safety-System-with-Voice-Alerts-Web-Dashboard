/**
 ******************************************************************************
 * @file    buzzer.c
 * @brief   Passive buzzer via TIM PWM. CCR = 0 silences, CCR = ARR/2 beeps.
 ******************************************************************************
 */

#include "buzzer.h"

/* 50% duty is loudest; drop to 1/4 or 1/8 if piercing in a small demo room. */
#define BUZZER_DUTY_NUM   1
#define BUZZER_DUTY_DEN   2

static TIM_HandleTypeDef *b_htim     = 0;
static uint32_t           b_channel  = 0;
static Buzzer_Mode_t      b_mode     = BUZZER_OFF;
static uint32_t           b_tone_ccr = 0;
static uint8_t            b_last_on  = 0;

static inline void set_pulse(uint32_t ccr) {
    __HAL_TIM_SET_COMPARE(b_htim, b_channel, ccr);
}

void Buzzer_Init(TIM_HandleTypeDef *htim, uint32_t channel)
{
    b_htim    = htim;
    b_channel = channel;
    b_mode    = BUZZER_OFF;
    b_last_on = 0;

    /* Derive tone CCR from the configured ARR — frequency-agnostic. */
    uint32_t arr = __HAL_TIM_GET_AUTORELOAD(b_htim);
    b_tone_ccr = ((arr + 1U) * BUZZER_DUTY_NUM) / BUZZER_DUTY_DEN;

    set_pulse(0);                     /* ensure silent before starting PWM */
    HAL_TIM_PWM_Start(b_htim, b_channel);
}

void Buzzer_SetMode(Buzzer_Mode_t mode)
{
    if (mode == b_mode) return;
    b_mode = mode;
    if (mode == BUZZER_OFF) {
        set_pulse(0);                 /* instant silence on mode-off       */
        b_last_on = 0;
    }
}

void Buzzer_Update(void)
{
    if (b_htim == 0) return;

    uint32_t t = HAL_GetTick();
    uint8_t  want_on = 0;

    switch (b_mode) {
        case BUZZER_WARN:
            /* 1 Hz, 25% duty: beep ... beep ... beep */
            want_on = ((t % 1000U) < 250U);
            break;
        case BUZZER_CRIT:
            /* 5 Hz, 50% duty: urgent rapid beeping */
            want_on = ((t % 200U) < 100U);
            break;
        case BUZZER_OFF:
        default:
            want_on = 0;
            break;
    }

    /* Write CCR only on transitions to avoid hundreds of redundant writes. */
    if (want_on != b_last_on) {
        set_pulse(want_on ? b_tone_ccr : 0);
        b_last_on = want_on;
    }
}
