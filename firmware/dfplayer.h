/**
 ******************************************************************************
 * @file    dfplayer.h
 * @brief   DFPlayer Mini MP3 module driver (STM32 HAL, UART).
 *
 *          Physical wiring (USART1 on STM32F103):
 *              STM32 PA9  (TX) --> DFPlayer RX (pin 2)
 *              STM32 PA10 (RX) <-- DFPlayer TX (pin 3)
 *              DFPlayer VCC (pin 1) --> 5 V
 *              DFPlayer GND (pin 7 and pin 10) --> GND
 *              DFPlayer SPK_1 (pin 6) --> speaker +
 *              DFPlayer SPK_2 (pin 8) --> speaker -
 *              (Pin 16 BUSY is optional; this driver doesn't use it.)
 *
 *          UART config: 9600 8N1, TX one-way.
 *
 *          CRITICAL: DFPlayer Mini logic is 3.3 V but some clones behave
 *          flakily when the STM32's 3.3 V TX drives their 5 V-level RX
 *          directly. If command uptake is unreliable, add a 1 kOhm series
 *          resistor on STM32 TX -> DFPlayer RX. This protects the DFPlayer
 *          and cleans up the signal. MOST boards work without it.
 ******************************************************************************
 */

#ifndef DFPLAYER_H
#define DFPLAYER_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 
 *  MP3 file map (must match /mp3/ folder on the SD card):
 *    0001.mp3  startup announcement
 *    0002.mp3  system ready
 *    0003.mp3  MQ-7 CO warning         (unused in this build)
 *    0004.mp3  MQ-7 CO critical        <-- played
 *    0005.mp3  MQ-2 LPG warning        (unused in this build)
 *    0006.mp3  MQ-2 LPG critical       <-- played
 *    0007.mp3  smoke warning           (unused)
 *    0008.mp3  smoke critical          (unused)
 *    0009.mp3  air quality warning     (unused)
 *    0010.mp3  MQ-135 CO2 critical     <-- played
 *    0011.mp3  all clear               <-- played on return-to-normal
 *    0012.mp3  sensor fault            (unused)
 *    0013.mp3  test mode               (unused)
  */
#define DF_TRACK_STARTUP      1
#define DF_TRACK_READY        2
#define DF_TRACK_CO_CRIT      4
#define DF_TRACK_LPG_CRIT     6
#define DF_TRACK_CO2_CRIT    10
#define DF_TRACK_ALL_CLEAR   11

/*
 *  Low-level API — direct DFPlayer control.
 *  Most callers should prefer the high-level Alert API below.
*/

/** Bind the driver to a UART handle. Also sends initial commands:
 *    - reset the chip
 *    - wait for boot (blocks ~1500 ms, the datasheet-recommended delay)
 *    - set output device = SD card
 *    - set volume (0..30)  */
void DF_Init(UART_HandleTypeDef *huart, uint8_t volume_0_30);

/** Set playback volume. Clamped to 0..30. */
void DF_SetVolume(uint8_t volume_0_30);

/** Play /mp3/0xxx.mp3 where xxx = track_number (1..9999).
 *  Returns immediately — DFPlayer plays asynchronously. */
void DF_PlayMp3(uint16_t track_number);

/** Stop current playback. */
void DF_Stop(void);

/* High-level Alert API: this is what main.c actually uses.
 *
 *  Usage pattern:
 *      DF_Alerts_Init(&huart1, 25);
 *      DF_Alerts_PlayStartup();          // at boot
 *      DF_Alerts_PlayReady();            // after calibration
 *      ...
 *      // every sensor cycle (500 ms):
 *      DF_Alerts_Update(worst_severity, worst_channel_id, now_ms);
 *
 *  Update() is idempotent: call it every cycle, it only sends a DFPlayer
 *  command when the announced state actually needs to change, or when
 *  the replay cooldown has expired on a sustained alert.*/

/* Channel identifiers for the Update call — order matches priority:
 *   CO (MQ-7) is most dangerous, announced first if tied */
typedef enum {
    DF_CH_NONE   = 0,
    DF_CH_CO     = 1,   /* MQ-7   */
    DF_CH_LPG    = 2,   /* MQ-2   */
    DF_CH_CO2    = 3,   /* MQ-135 */
} DF_Channel_t;

/* Severity as used by the rest of your codebase (matches Severity_t
 * values from thresholds.h; duplicated here to avoid a circular include) */
typedef enum {
    DF_SEV_NORMAL   = 0,
    DF_SEV_WARNING  = 1,
    DF_SEV_CRITICAL = 2,
} DF_Severity_t;

/** Initialise the alerts layer. Pass a UART handle and default volume. */
void DF_Alerts_Init(UART_HandleTypeDef *huart, uint8_t volume_0_30);

/** One-shot boot announcement: "System activated. Warming up..." */
void DF_Alerts_PlayStartup(void);

/** One-shot after calibration: "System ready. Monitoring..." */
void DF_Alerts_PlayReady(void);

/** Call every sensor cycle. Announces critical alerts, re-announces
 *  sustained alerts on cooldown, and plays "all clear" on return to normal.
 *  `now_ms` = HAL_GetTick(). */
void DF_Alerts_Update(DF_Severity_t worst_severity,
                      DF_Channel_t  worst_channel,
                      uint32_t      now_ms);

#ifdef __cplusplus
}
#endif

#endif /* DFPLAYER_H */
