/**
 ******************************************************************************
 * @file    thresholds.h
 * @brief   Severity tiers, per-gas cutoffs, and alert-debounce config.
 *
 *          Defense against sensor noise / transients: a sensor must read
 *          above its warning/critical cutoff for N consecutive cycles
 *          before the buzzer activates. See ALERT_CONFIRM_CYCLES.
 ******************************************************************************
 */

#ifndef THRESHOLDS_H
#define THRESHOLDS_H

#include <stdint.h>

typedef enum {
    SEV_NORMAL   = 0,
    SEV_WARNING  = 1,
    SEV_CRITICAL = 2
} Severity_t;

/* ---- MQ-2: LPG / combustible gas (ppm) ---- */
#define TH_MQ2_LPG_WARN      1000U   /* ~10% LEL */
#define TH_MQ2_LPG_CRIT      2000U   /* ~20% LEL */

/* ---- MQ-7: Carbon monoxide (ppm) ---- */
#define TH_MQ7_CO_WARN         50U   /* above curve floor, OSHA-adjacent */
#define TH_MQ7_CO_CRIT        150U   /* symptom onset after exposure    */

/* ---- MQ-135: CO2 (ppm) ---- */
#define TH_MQ135_CO2_WARN    1000U   /* stuffy / drowsy */
#define TH_MQ135_CO2_CRIT    2000U   /* cognitive loss  */

/* ---- Alert debounce ----
 * A reading must sit at this severity for N consecutive cycles before
 * the buzzer engages. Prevents single-sample glitches from alarming.
 * With 500 ms sensor cadence, 2 cycles = 1 second confirmation window. */
#define ALERT_CONFIRM_CYCLES  2U

/* ---- Post-calibration grace period ----
 * No alerts for this many cycles after calibration completes. Lets the
 * sensor outputs settle. */
#define POST_CALIB_GRACE_CYCLES  4U

static inline Severity_t classify_ppm(uint16_t ppm, uint16_t warn, uint16_t crit)
{
    if (ppm >= crit) return SEV_CRITICAL;
    if (ppm >= warn) return SEV_WARNING;
    return SEV_NORMAL;
}

/* Short 4-char status strings — fit cleanly in the 18-char OLED row. */
static inline const char* severity_label(Severity_t s) {
    switch (s) {
        case SEV_CRITICAL: return "CRIT";
        case SEV_WARNING:  return "WARN";
        default:           return " OK ";
    }
}

#endif /* THRESHOLDS_H */
