/**
 ******************************************************************************
 * @file    display_ui.h
 * @brief   SSD1306 page renderer for the gas safety system.
 ******************************************************************************
 */

#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <stdint.h>
#include "thresholds.h"
#include "mq_sensor.h"

#define UI_PAGE_COUNT  5

typedef enum {
    UI_PAGE_OVERVIEW = 0,
    UI_PAGE_MQ2      = 1,
    UI_PAGE_MQ7      = 2,
    UI_PAGE_MQ135    = 3,
    UI_PAGE_SYSTEM   = 4
} UI_Page_t;

typedef struct {
    MQ_Sensor_t *sensor;
    const char  *name;     /* "MQ-2", "MQ-7", "MQ-135" */
    const char  *gas;      /* "LPG", "CO", "CO2"       */
    uint16_t     ppm;
    Severity_t   severity;
} UI_SensorView_t;

/* Boot / mode screens */
void UI_ShowSplash         (void);
void UI_ShowWarmupFrame    (uint32_t seconds_left, uint32_t total_seconds);
void UI_ShowCalibrating    (void);
void UI_ShowCalibrationDone(void);

/* Live screens */
void UI_RenderPage(UI_Page_t page,
                   const UI_SensorView_t *mq2,
                   const UI_SensorView_t *mq7,
                   const UI_SensorView_t *mq135,
                   uint32_t uptime_ms);

/* Focused WARN view — dominant gas featured, others summarised.
 * `others` must point to exactly 2 UI_SensorView_t pointers. */
void UI_RenderFocusedAlert(const UI_SensorView_t *primary,
                           const UI_SensorView_t * const others[2],
                           uint32_t uptime_ms);

/* Full-screen CRITICAL overlay — flashes via blink_on. */
void UI_RenderCriticalOverlay(const UI_SensorView_t *offender, uint8_t blink_on);

#endif /* DISPLAY_UI_H */
