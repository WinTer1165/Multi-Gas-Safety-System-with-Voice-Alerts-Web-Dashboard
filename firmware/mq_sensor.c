/**
 ******************************************************************************
 * @file    mq_sensor.c
 * @brief   MQ gas sensor driver. Integer-only, multi-channel ADC.
 *
 ******************************************************************************
 */

#include "mq_sensor.h"



/* ---------- MQ-2 ---------- */
static const MQ_CurvePoint_t mq2_lpg_pts[] = {
    {   26, 10000 }, {   40,  5000 }, {   60,  2000 }, {   80,  1000 },
    {  120,   500 }, {  180,   300 }, {  300,   200 },
    /* extrapolated: clean-air region */
    {  500,    50 }, {  983,     0 }, { 2000,     0 },
};
const MQ_Curve_t MQ2_Curve_LPG =
    { mq2_lpg_pts, sizeof(mq2_lpg_pts)/sizeof(mq2_lpg_pts[0]) };

static const MQ_CurvePoint_t mq2_co_pts[] = {
    {   80, 10000 }, {  120,  5000 }, {  200,  2000 }, {  300,  1000 },
    {  450,   500 }, {  750,   200 },
    /* extrapolated */
    { 1200,    50 }, { 2000,     0 },
};
const MQ_Curve_t MQ2_Curve_CO =
    { mq2_co_pts, sizeof(mq2_co_pts)/sizeof(mq2_co_pts[0]) };

static const MQ_CurvePoint_t mq2_smoke_pts[] = {
    {   40, 10000 }, {   55,  5000 }, {   80,  2000 }, {  110,  1000 },
    {  160,   500 }, {  260,   200 },
    /* extrapolated */
    {  500,    50 }, { 1500,     0 },
};
const MQ_Curve_t MQ2_Curve_Smoke =
    { mq2_smoke_pts, sizeof(mq2_smoke_pts)/sizeof(mq2_smoke_pts[0]) };

/* ---------- MQ-7 ----------
 */
static const MQ_CurvePoint_t mq7_co_pts[] = {
    {   15, 4000 }, {   20, 2000 }, {   30, 1000 }, {   45,  500 },
    {   70,  200 }, {  100,  100 }, {  150,   50 },
    /* extrapolated: bring clean air to ~0 ppm */
    {  250,   20 }, {  500,   10 }, { 1000,    5 }, { 2750,    0 },
    { 5000,    0 },
};
const MQ_Curve_t MQ7_Curve_CO =
    { mq7_co_pts, sizeof(mq7_co_pts)/sizeof(mq7_co_pts[0]) };

static const MQ_CurvePoint_t mq7_h2_pts[] = {
    {   20, 4000 }, {   30, 2000 }, {   50, 1000 }, {   75,  500 },
    {  130,  200 }, {  200,  100 },
    /* extrapolated */
    {  500,   20 }, { 2000,    0 },
};
const MQ_Curve_t MQ7_Curve_H2 =
    { mq7_h2_pts, sizeof(mq7_h2_pts)/sizeof(mq7_h2_pts[0]) };

/* ---------- MQ-135 ---------- */
static const MQ_CurvePoint_t mq135_co2_pts[] = {
    {   70, 1000 }, {   85,  400 }, {  100,  200 }, {  130,  100 },
    {  170,   50 }, {  230,   20 }, {  300,   10 },
    /* extrapolated */
    {  360,    5 }, {  800,    0 },
};
const MQ_Curve_t MQ135_Curve_CO2 =
    { mq135_co2_pts, sizeof(mq135_co2_pts)/sizeof(mq135_co2_pts[0]) };

static const MQ_CurvePoint_t mq135_nh3_pts[] = {
    {   35, 300 }, {   50, 200 }, {   75, 100 }, {  120,  50 },
    {  200,  20 }, {  300,  10 },
    /* extrapolated */
    {  500,   2 }, { 1000,   0 },
};
const MQ_Curve_t MQ135_Curve_NH3 =
    { mq135_nh3_pts, sizeof(mq135_nh3_pts)/sizeof(mq135_nh3_pts[0]) };

static const MQ_CurvePoint_t mq135_alcohol_pts[] = {
    {   20, 300 }, {   35, 100 }, {   60,  50 }, {  100,  20 }, {  170,  10 },
    /* extrapolated */
    {  300,   2 }, {  700,   0 },
};
const MQ_Curve_t MQ135_Curve_Alcohol =
    { mq135_alcohol_pts, sizeof(mq135_alcohol_pts)/sizeof(mq135_alcohol_pts[0]) };


/* ADC helpers
  */

static void adc_select_channel(ADC_HandleTypeDef *hadc, uint32_t channel)
{
    ADC_ChannelConfTypeDef s = {0};
    s.Channel      = channel;
    s.Rank         = ADC_REGULAR_RANK_1;
    s.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
    HAL_ADC_ConfigChannel(hadc, &s);
}

/* Channel-configured burst read with averaging. */
static uint16_t adc_read_avg(ADC_HandleTypeDef *hadc, uint32_t channel,
                             uint16_t samples)
{
    adc_select_channel(hadc, channel);

    uint32_t acc = 0;
    for (uint16_t i = 0; i < samples; i++) {
        uint16_t v = 0;
        if (HAL_ADC_Start(hadc) == HAL_OK &&
            HAL_ADC_PollForConversion(hadc, 10) == HAL_OK) {
            v = (uint16_t)HAL_ADC_GetValue(hadc);
        }
        HAL_ADC_Stop(hadc);
        acc += v;
    }
    return (uint16_t)(acc / samples);
}

/* --- unit conversions --- */
static inline uint16_t code_to_vmv(uint16_t code) {
    return (uint16_t)(((uint32_t)code * MQ_ADC_VREF_MV) / MQ_ADC_MAX);
}

static inline uint16_t vpin_to_vsensor(uint16_t v_pin_mv) {
    return (uint16_t)(((uint32_t)v_pin_mv * MQ_VDIV_DEN) / MQ_VDIV_NUM);
}

static uint32_t vsensor_to_rs(uint16_t v_sensor_mv)
{
    /* Guard near-zero and near-Vcc to avoid divide-by-zero or negative Rs */
    if (v_sensor_mv < 10)              v_sensor_mv = 10;
    if (v_sensor_mv > MQ_VCC_MV - 10)  v_sensor_mv = MQ_VCC_MV - 10;

    uint32_t num = (uint32_t)MQ_RL_OHM * (uint32_t)(MQ_VCC_MV - v_sensor_mv);
    return num / (uint32_t)v_sensor_mv;
}

/* Curve interpolation — linear between tabulated points.
 */
static uint16_t curve_lookup(const MQ_Curve_t *curve, uint16_t ratio_x100)
{
    if (curve == 0 || curve->count == 0) return 0;

    /* Clamp at endpoints (now safe thanks to extended curves) */
    if (ratio_x100 <= curve->points[0].ratio_x100)
        return curve->points[0].ppm;
    if (ratio_x100 >= curve->points[curve->count - 1].ratio_x100)
        return curve->points[curve->count - 1].ppm;

    /* Find bracketing pair and linearly interpolate. */
    for (uint8_t i = 1; i < curve->count; i++) {
        if (ratio_x100 <= curve->points[i].ratio_x100) {
            int32_t r0 = curve->points[i - 1].ratio_x100;
            int32_t r1 = curve->points[i].ratio_x100;
            int32_t p0 = curve->points[i - 1].ppm;
            int32_t p1 = curve->points[i].ppm;

            int32_t num  = (p1 - p0) * ((int32_t)ratio_x100 - r0);
            int32_t ppm  = p0 + (num / (r1 - r0));
            if (ppm < 0)     ppm = 0;
            if (ppm > 65535) ppm = 65535;
            return (uint16_t)ppm;
        }
    }
    return 0;
}

/*
 API
 */

void MQ_Init(MQ_Sensor_t *s, ADC_HandleTypeDef *hadc,
             uint32_t adc_channel, uint16_t clean_air_x100)
{
    s->hadc           = hadc;
    s->adc_channel    = adc_channel;
    s->clean_air_x100 = clean_air_x100;
    s->R0_ohm         = 10000U;  /* safe default until calibrated */
    s->raw = s->v_pin_mv = s->v_sensor_mv = s->ratio_x100 = 0;
    s->Rs_ohm = 0;
}

void MQ_Calibrate(MQ_Sensor_t *s)
{
    if (s->clean_air_x100 == 0) return;

    uint16_t code     = adc_read_avg(s->hadc, s->adc_channel, MQ_CALIB_SAMPLES);
    uint16_t v_pin    = code_to_vmv(code);
    uint16_t v_sensor = vpin_to_vsensor(v_pin);
    uint32_t rs       = vsensor_to_rs(v_sensor);

    /* R0 = Rs / clean_air_ratio = Rs * 100 / clean_air_x100 */
    s->R0_ohm = (rs * 100U) / (uint32_t)s->clean_air_x100;
}

void MQ_SetR0(MQ_Sensor_t *s, uint32_t R0_ohm)
{
    if (R0_ohm > 0) s->R0_ohm = R0_ohm;
}

uint16_t MQ_ReadRaw(MQ_Sensor_t *s)
{
    s->raw = adc_read_avg(s->hadc, s->adc_channel, MQ_SAMPLES);
    return s->raw;
}

uint16_t MQ_ReadVmv(MQ_Sensor_t *s)
{
    (void)MQ_ReadRaw(s);
    s->v_pin_mv    = code_to_vmv(s->raw);
    s->v_sensor_mv = vpin_to_vsensor(s->v_pin_mv);
    return s->v_pin_mv;
}

uint32_t MQ_ReadRs(MQ_Sensor_t *s)
{
    (void)MQ_ReadVmv(s);
    s->Rs_ohm = vsensor_to_rs(s->v_sensor_mv);
    return s->Rs_ohm;
}

uint16_t MQ_ReadRatio(MQ_Sensor_t *s)
{
    uint32_t rs = MQ_ReadRs(s);
    if (s->R0_ohm == 0) { s->ratio_x100 = 0; return 0; }

    uint32_t r = (rs * 100U) / s->R0_ohm;
    if (r > 65535U) r = 65535U;
    s->ratio_x100 = (uint16_t)r;
    return s->ratio_x100;
}

uint16_t MQ_ReadPPM(MQ_Sensor_t *s, const MQ_Curve_t *curve)
{
    return curve_lookup(curve, MQ_ReadRatio(s));
}
