/**
 ******************************************************************************
 * @file    dfplayer.c
 * @brief   DFPlayer Mini driver + high-level alert state machine.
 ******************************************************************************
 */

#include "dfplayer.h"
#include <string.h>

/*  Low-level protocol
 *
 *  Command frame (10 bytes):
 *      0x7E 0xFF 0x06 CMD 0x00 PARAM_H PARAM_L CKSUM_H CKSUM_L 0xEF
 *
 *  Commands used:
 *      0x09  set playback device      (param: 2 = SD card)
 *      0x0C  reset module
 *      0x06  set volume                (param: 0..30)
 *      0x12  play /MP3/xxxx.mp3 by number
 *      0x16  stop
 */

#define CMD_RESET         0x0C
#define CMD_SET_DEVICE    0x09
#define CMD_SET_VOLUME    0x06
#define CMD_PLAY_MP3      0x12
#define CMD_STOP          0x16

#define PARAM_DEVICE_SD   0x0002

static UART_HandleTypeDef *df_huart = 0;

/* Compute the two checksum bytes per the DFPlayer spec. */
static void df_checksum(const uint8_t *frame, uint8_t *ck_hi, uint8_t *ck_lo)
{
    /* Sum bytes 1..6 (inclusive), take two's complement. */
    uint16_t sum = 0;
    for (int i = 1; i <= 6; i++) sum += frame[i];
    uint16_t chk = 0xFFFF - sum + 1;
    *ck_hi = (uint8_t)(chk >> 8);
    *ck_lo = (uint8_t)(chk & 0xFF);
}

/* Send a command frame. Blocks until UART transmit completes. */
static void df_send(uint8_t cmd, uint16_t param)
{
    if (df_huart == 0) return;

    uint8_t frame[10];
    frame[0] = 0x7E;
    frame[1] = 0xFF;
    frame[2] = 0x06;
    frame[3] = cmd;
    frame[4] = 0x00;                    /* 0x01 would request ACK; skip */
    frame[5] = (uint8_t)(param >> 8);
    frame[6] = (uint8_t)(param & 0xFF);
    df_checksum(frame, &frame[7], &frame[8]);
    frame[9] = 0xEF;

    HAL_UART_Transmit(df_huart, frame, sizeof(frame), 100);
}

void DF_Init(UART_HandleTypeDef *huart, uint8_t volume_0_30)
{
    df_huart = huart;

    df_send(CMD_RESET, 0x0000);
    HAL_Delay(1500);

    df_send(CMD_SET_DEVICE, PARAM_DEVICE_SD);
    HAL_Delay(200);

    DF_SetVolume(volume_0_30);
    HAL_Delay(100);
}

void DF_SetVolume(uint8_t volume_0_30)
{
    if (volume_0_30 > 30) volume_0_30 = 30;
    df_send(CMD_SET_VOLUME, (uint16_t)volume_0_30);
}

void DF_PlayMp3(uint16_t track_number)
{
    df_send(CMD_PLAY_MP3, track_number);
}

void DF_Stop(void)
{
    df_send(CMD_STOP, 0x0000);
}


/* 
 High-level Alert state machine
 */

#define REPLAY_COOLDOWN_MS   15000U   /* 15 s between replays of a sustained alert */

static uint32_t      al_last_play_ms = 0;
static DF_Channel_t  al_last_channel = DF_CH_NONE;  /* what was last announced */
static uint8_t       al_had_alert    = 0;           /* for all-clear edge detect */

void DF_Alerts_Init(UART_HandleTypeDef *huart, uint8_t volume_0_30)
{
    DF_Init(huart, volume_0_30);
    al_last_play_ms = 0;
    al_last_channel = DF_CH_NONE;
    al_had_alert    = 0;
}

void DF_Alerts_PlayStartup(void)
{
    DF_PlayMp3(DF_TRACK_STARTUP);
}

void DF_Alerts_PlayReady(void)
{
    DF_PlayMp3(DF_TRACK_READY);
}

/* Map (severity, channel) to the track that should be announced.
 * Returns 0 if nothing should be announced. */
static uint16_t pick_track(DF_Severity_t sev, DF_Channel_t ch)
{
    if (sev != DF_SEV_CRITICAL) return 0;

    switch (ch) {
        case DF_CH_CO:  return DF_TRACK_CO_CRIT;
        case DF_CH_LPG: return DF_TRACK_LPG_CRIT;
        case DF_CH_CO2: return DF_TRACK_CO2_CRIT;
        default:        return 0;
    }
}

void DF_Alerts_Update(DF_Severity_t worst_severity,
                      DF_Channel_t  worst_channel,
                      uint32_t      now_ms)
{
    /* Decide what SHOULD be announced right now. */
    uint16_t     desired_track   = pick_track(worst_severity, worst_channel);
    DF_Channel_t desired_channel = desired_track ? worst_channel : DF_CH_NONE;

    /* --- Case A: back to normal, but we were alerting before --- */
    if (desired_channel == DF_CH_NONE) {
        if (al_had_alert) {
            DF_PlayMp3(DF_TRACK_ALL_CLEAR);
            al_had_alert    = 0;
            al_last_channel = DF_CH_NONE;
            al_last_play_ms = now_ms;
        }
        return;
    }

    /* --- Case B: announcement needs to change --- */
    if (desired_channel != al_last_channel) {
        DF_PlayMp3(desired_track);
        al_last_channel = desired_channel;
        al_last_play_ms = now_ms;
        al_had_alert    = 1;
        return;
    }

    /* --- Case C: same alert persists; replay on cooldown --- */
    if ((now_ms - al_last_play_ms) >= REPLAY_COOLDOWN_MS) {
        DF_PlayMp3(desired_track);
        al_last_play_ms = now_ms;
        /* al_last_channel, al_had_alert unchanged */
    }
}
