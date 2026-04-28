/**
 ******************************************************************************
 * @file    mq_sensor.h
 * @brief   MQ-series gas sensor driver (integer-only, multi-channel ADC).
 *
 *          Sensor instance carries its own ADC channel so one ADC handle
 *          can drive all three MQ sensors.
 ******************************************************************************
 */

#ifndef MQ_SENSOR_H
#define MQ_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"
#include <stdint.h>


#define MQ_VCC_MV            5000U    /* module Vcc, mV                     */
#define MQ_ADC_VREF_MV       3300U    /* MCU ADC ref, mV                    */
#define MQ_ADC_MAX           4095U    /* 12-bit ADC                         */
#define MQ_RL_OHM            10000U   /* load resistor on breakout          */

/* Resistor divider 10k/20k gives V_adc = V_AO * 0.667 */
#define MQ_VDIV_NUM          667U
#define MQ_VDIV_DEN          1000U

#define MQ_SAMPLES           16U
#define MQ_CALIB_SAMPLES     64U


#define MQ2_CLEAN_AIR_X100     983U   /* 9.83   */
#define MQ7_CLEAN_AIR_X100     2750U  /* 27.5   */
#define MQ135_CLEAN_AIR_X100   360U   /* 3.60   */


typedef struct {
    uint16_t ratio_x100;
    uint16_t ppm;
} MQ_CurvePoint_t;

typedef struct {
    const MQ_CurvePoint_t *points;
    uint8_t  count;
} MQ_Curve_t;

extern const MQ_Curve_t MQ2_Curve_LPG;
extern const MQ_Curve_t MQ2_Curve_CO;
extern const MQ_Curve_t MQ2_Curve_Smoke;

extern const MQ_Curve_t MQ7_Curve_CO;
extern const MQ_Curve_t MQ7_Curve_H2;

extern const MQ_Curve_t MQ135_Curve_CO2;
extern const MQ_Curve_t MQ135_Curve_NH3;
extern const MQ_Curve_t MQ135_Curve_Alcohol;


typedef struct {
    ADC_HandleTypeDef *hadc;
    uint32_t  adc_channel;
    uint16_t  clean_air_x100;
    uint32_t  R0_ohm;

    /* Cached last reading */
    uint16_t  raw;
    uint16_t  v_pin_mv;
    uint16_t  v_sensor_mv;
    uint32_t  Rs_ohm;
    uint16_t  ratio_x100;
} MQ_Sensor_t;


void     MQ_Init       (MQ_Sensor_t *s, ADC_HandleTypeDef *hadc,
                        uint32_t adc_channel, uint16_t clean_air_x100);
void     MQ_Calibrate  (MQ_Sensor_t *s);
void     MQ_SetR0      (MQ_Sensor_t *s, uint32_t R0_ohm);
uint16_t MQ_ReadRaw    (MQ_Sensor_t *s);
uint16_t MQ_ReadVmv    (MQ_Sensor_t *s);
uint32_t MQ_ReadRs     (MQ_Sensor_t *s);
uint16_t MQ_ReadRatio  (MQ_Sensor_t *s);
uint16_t MQ_ReadPPM    (MQ_Sensor_t *s, const MQ_Curve_t *curve);

#ifdef __cplusplus
}
#endif

#endif /* MQ_SENSOR_H */
