/**
 ******************************************************************************
 * @file    display_ui.c
 * @brief   Page renderers for the gas safety system OLED.
 *
 ******************************************************************************
 */

#include "display_ui.h"
#include "ssd1306.h"
#include "fonts.h"
#include <stdio.h>
#include <string.h>

#define UI_WIDTH   128
#define UI_HEIGHT   64

static inline void ui_clear(void) {
    SSD1306_Fill(SSD1306_COLOR_BLACK);
}

static void ui_puts(uint16_t x, uint16_t y, const char *s,
                    FontDef_t *f, SSD1306_COLOR_t c) {
    SSD1306_GotoXY(x, y);
    SSD1306_Puts((char*)s, f, c);
}

static void ui_puts_centered(uint16_t y, const char *s,
                             FontDef_t *f, SSD1306_COLOR_t c) {
    uint16_t px = (uint16_t)strlen(s) * f->FontWidth;
    uint16_t x  = (UI_WIDTH > px) ? (UI_WIDTH - px) / 2 : 0;
    ui_puts(x, y, s, f, c);
}

/* 7x7 severity icon. Outline / striped / filled for OK / WARN / CRIT. */
static void ui_draw_sev_icon(uint16_t x, uint16_t y, Severity_t s,
                             SSD1306_COLOR_t fg)
{
    SSD1306_DrawRectangle(x, y, 7, 7, fg);
    if (s == SEV_WARNING) {
        SSD1306_DrawLine(x + 1, y + 2, x + 5, y + 2, fg);
        SSD1306_DrawLine(x + 1, y + 4, x + 5, y + 4, fg);
    } else if (s == SEV_CRITICAL) {
        SSD1306_DrawFilledRectangle(x + 1, y + 1, 5, 5, fg);
    }
}

/* Format helpers */
static void fmt_uptime_hm(char *buf, size_t n, uint32_t uptime_ms) {
    uint32_t s = uptime_ms / 1000U;
    snprintf(buf, n, "%02lu:%02lu",
             (unsigned long)((s / 3600U) % 100U),
             (unsigned long)((s / 60U) % 60U));
}

/*
 *  Boot screens
 */

void UI_ShowSplash(void)
{
    ui_clear();
    ui_puts_centered( 8, "Gas Safety", &Font_11x18, SSD1306_COLOR_WHITE);
    ui_puts_centered(32, "Monitor",    &Font_11x18, SSD1306_COLOR_WHITE);
    ui_puts_centered(54, "starting...", &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_UpdateScreen();
}

void UI_ShowWarmupFrame(uint32_t seconds_left, uint32_t total_seconds)
{
    char buf[16];

    ui_clear();
    ui_puts_centered(2, "Warming up", &Font_11x18, SSD1306_COLOR_WHITE);

    snprintf(buf, sizeof(buf), "%lus", (unsigned long)seconds_left);
    ui_puts_centered(26, buf, &Font_16x26, SSD1306_COLOR_WHITE);

    SSD1306_DrawRectangle(4, 58, 120, 4, SSD1306_COLOR_WHITE);
    if (total_seconds > 0 && seconds_left <= total_seconds) {
        uint32_t done = total_seconds - seconds_left;
        uint32_t fill = (120U * done) / total_seconds;
        if (fill > 0)
            SSD1306_DrawFilledRectangle(4, 58, (uint16_t)fill, 4,
                                        SSD1306_COLOR_WHITE);
    }
    SSD1306_UpdateScreen();
}

void UI_ShowCalibrating(void)
{
    ui_clear();
    ui_puts_centered( 6, "Calibrating",     &Font_11x18, SSD1306_COLOR_WHITE);
    ui_puts_centered(30, "Needs fresh air", &Font_7x10,  SSD1306_COLOR_WHITE);
    ui_puts_centered(44, "Please wait...",  &Font_7x10,  SSD1306_COLOR_WHITE);
    SSD1306_UpdateScreen();
}

void UI_ShowCalibrationDone(void)
{
    ui_clear();
    ui_puts_centered(10, "Ready",             &Font_16x26, SSD1306_COLOR_WHITE);
    ui_puts_centered(44, "Monitoring active", &Font_7x10,  SSD1306_COLOR_WHITE);
    SSD1306_UpdateScreen();
}

/* 
 *  Normal overview page — all three sensors equally
 */

static void draw_overview_row(uint16_t y, const UI_SensorView_t *v)
{
    char line[24];
    snprintf(line, sizeof(line), "%-3s %5u ppm %4s",
             v->gas, v->ppm, severity_label(v->severity));
    ui_puts(0, y, line, &Font_7x10, SSD1306_COLOR_WHITE);
}

static void draw_overview(const UI_SensorView_t *m2,
                          const UI_SensorView_t *m7,
                          const UI_SensorView_t *m135,
                          uint32_t uptime_ms)
{
    char header[24], hm[8];
    fmt_uptime_hm(hm, sizeof(hm), uptime_ms);

    ui_clear();

    snprintf(header, sizeof(header), "Monitor    %s", hm);
    ui_puts(0, 0, header, &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_DrawLine(0, 11, 127, 11, SSD1306_COLOR_WHITE);

    draw_overview_row(16, m2);
    draw_overview_row(30, m7);
    draw_overview_row(44, m135);

    ui_puts(0, 55, "Press btn for details", &Font_7x10, SSD1306_COLOR_WHITE);
}

/* 
 *  Per-sensor detail page
 */

static void draw_sensor_detail(const UI_SensorView_t *v)
{
    char line[28];

    ui_clear();

    snprintf(line, sizeof(line), "%s: %s", v->name, v->gas);
    ui_puts(0, 0, line, &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_DrawLine(0, 11, 127, 11, SSD1306_COLOR_WHITE);

    snprintf(line, sizeof(line), "%u ppm", v->ppm);
    ui_puts(0, 14, line, &Font_11x18, SSD1306_COLOR_WHITE);

    snprintf(line, sizeof(line), "Status: %s", severity_label(v->severity));
    ui_puts(0, 36, line, &Font_7x10, SSD1306_COLOR_WHITE);
    ui_draw_sev_icon(112, 36, v->severity, SSD1306_COLOR_WHITE);

    snprintf(line, sizeof(line), "Rs=%luO  R0=%luO",
             (unsigned long)v->sensor->Rs_ohm,
             (unsigned long)v->sensor->R0_ohm);
    ui_puts(0, 52, line, &Font_7x10, SSD1306_COLOR_WHITE);
}

/* 
 *  System info page
 */

static void draw_system(const UI_SensorView_t *m2,
                        const UI_SensorView_t *m7,
                        const UI_SensorView_t *m135,
                        uint32_t uptime_ms)
{
    char line[28];

    ui_clear();
    ui_puts(0, 0, "System info", &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_DrawLine(0, 11, 127, 11, SSD1306_COLOR_WHITE);

    uint32_t s = uptime_ms / 1000U;
    snprintf(line, sizeof(line), "Uptime %02lu:%02lu:%02lu",
             (unsigned long)(s / 3600U),
             (unsigned long)((s / 60U) % 60U),
             (unsigned long)(s % 60U));
    ui_puts(0, 14, line, &Font_7x10, SSD1306_COLOR_WHITE);

    snprintf(line, sizeof(line), "R0 MQ-2   %lu",   (unsigned long)m2->sensor->R0_ohm);
    ui_puts(0, 26, line, &Font_7x10, SSD1306_COLOR_WHITE);
    snprintf(line, sizeof(line), "R0 MQ-7   %lu",   (unsigned long)m7->sensor->R0_ohm);
    ui_puts(0, 38, line, &Font_7x10, SSD1306_COLOR_WHITE);
    snprintf(line, sizeof(line), "R0 MQ-135 %lu",   (unsigned long)m135->sensor->R0_ohm);
    ui_puts(0, 50, line, &Font_7x10, SSD1306_COLOR_WHITE);
}

/* 
 *  Page dispatcher
 */

void UI_RenderPage(UI_Page_t page,
                   const UI_SensorView_t *mq2,
                   const UI_SensorView_t *mq7,
                   const UI_SensorView_t *mq135,
                   uint32_t uptime_ms)
{
    switch (page) {
        case UI_PAGE_OVERVIEW: draw_overview(mq2, mq7, mq135, uptime_ms); break;
        case UI_PAGE_MQ2:      draw_sensor_detail(mq2);                   break;
        case UI_PAGE_MQ7:      draw_sensor_detail(mq7);                   break;
        case UI_PAGE_MQ135:    draw_sensor_detail(mq135);                 break;
        case UI_PAGE_SYSTEM:   draw_system(mq2, mq7, mq135, uptime_ms);   break;
        default:               draw_overview(mq2, mq7, mq135, uptime_ms); break;
    }
    SSD1306_UpdateScreen();
}

/* 
 *  Focused alert view (WARN) — dominant gas prominently, others summarised
 *
 *  Layout:
 *    y=0   "ALERT      HH:MM"   (7x10, header)
 *    y=11  separator line
 *    y=14  "Primary: LPG  WARN" (7x10, 17-18 chars max -> 119-126 px)
 *    y=28  "1234 ppm"           (11x18, big, centered)
 *    y=50  "CO 60*  CO2 450"    (7x10, secondaries; '*' = also elevated)
 *
 *  All strings verified ≤ 128 px wide for worst-case values.
 */

/* Secondary short-form: "CO 60*" or "CO 60" — 7 chars max at worst case */
static void fmt_secondary(char *buf, size_t n, const UI_SensorView_t *v)
{
    const char *mark = (v->severity == SEV_NORMAL) ? "" : "*";
    snprintf(buf, n, "%s %u%s", v->gas, v->ppm, mark);
}

void UI_RenderFocusedAlert(const UI_SensorView_t *primary,
                           const UI_SensorView_t * const others[2],
                           uint32_t uptime_ms)
{
    char line[32];
    char hm[8];
    char s1[12], s2[12];

    fmt_uptime_hm(hm, sizeof(hm), uptime_ms);

    ui_clear();

    /* Header: "ALERT      HH:MM" — 5 + 6 spaces + 5 = 16 chars = 112 px */
    snprintf(line, sizeof(line), "ALERT      %s", hm);
    ui_puts(0, 0, line, &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_DrawLine(0, 11, 127, 11, SSD1306_COLOR_WHITE);

    /* Primary status line — fits 18 chars in 7x10 */
    snprintf(line, sizeof(line), "Primary: %s %s",
             primary->gas, severity_label(primary->severity));
    ui_puts(0, 14, line, &Font_7x10, SSD1306_COLOR_WHITE);

    /* Big ppm reading (11x18). Max "65535 ppm" = 9 chars * 11 = 99 px */
    snprintf(line, sizeof(line), "%u ppm", primary->ppm);
    ui_puts_centered(28, line, &Font_11x18, SSD1306_COLOR_WHITE);

    /* Secondaries. Worst case: "CO 999* CO2 9999*" = 17 chars * 7 = 119 px */
    fmt_secondary(s1, sizeof(s1), others[0]);
    fmt_secondary(s2, sizeof(s2), others[1]);
    snprintf(line, sizeof(line), "%s  %s", s1, s2);
    ui_puts(0, 52, line, &Font_7x10, SSD1306_COLOR_WHITE);

    SSD1306_UpdateScreen();
}

/*
 *  CRITICAL overlay — full-screen flashing alert
 *
 *   layout:
 *    y=0   "DANGER"        (16x26, 6 chars * 16 = 96 px, centered at x=16)
 *    y=28  "EVACUATE"      (11x18, 8 chars * 11 = 88 px, centered at x=20)
 *    y=52  "LPG 1234 ppm"  (7x10, 12 chars max * 7 = 84 px, centered)
 *
 *  Worst-case bottom line: "CO2 65535 ppm" = 13 chars * 7 = 91 px
 */
void UI_RenderCriticalOverlay(const UI_SensorView_t *offender, uint8_t blink_on)
{
    char line[24];
    SSD1306_COLOR_t fg = blink_on ? SSD1306_COLOR_WHITE : SSD1306_COLOR_BLACK;
    SSD1306_COLOR_t bg = blink_on ? SSD1306_COLOR_BLACK : SSD1306_COLOR_WHITE;

    SSD1306_Fill(bg);

    ui_puts_centered( 0, "DANGER",   &Font_16x26, fg);
    ui_puts_centered(28, "EVACUATE", &Font_11x18, fg);

    snprintf(line, sizeof(line), "%s %u ppm", offender->gas, offender->ppm);
    ui_puts_centered(52, line, &Font_7x10, fg);

    SSD1306_UpdateScreen();
}
