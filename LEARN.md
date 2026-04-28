# LEARN.md — How the Firmware Works?

A walkthrough of the Multi-Gas Safety System firmware. This is meant to be read top-to-bottom by anyone who wants to understand *why* the code is shaped the way it is, not just *what* it does. If you only want to build and flash, the [README](README.md) is enough.

The whole project is intentionally written without floating-point math, without dynamic allocation, and without blocking delays in the main loop. Each section below explains the design choice and the trade-off behind it.

---

## Table of Contents

1. [System architecture at a glance](#1-system-architecture-at-a-glance)
2. [The MQ sensor pipeline](#2-the-mq-sensor-pipeline)
3. [Severity tiers and debouncing](#3-severity-tiers-and-debouncing)
4. [The buzzer — patterns from modular arithmetic](#4-the-buzzer--patterns-from-modular-arithmetic)
5. [The DFPlayer driver and alert state machine](#5-the-dfplayer-driver-and-alert-state-machine)
6. [The OLED rendering layer](#6-the-oled-rendering-layer)
7. [The cooperative scheduler in `main.c`](#7-the-cooperative-scheduler-in-mainc)
8. [The ESP8266 dashboard pipeline](#8-the-esp8266-dashboard-pipeline)

---

## 1. System architecture at a glance

Two microcontrollers, each with one job:

- **STM32F103C8T6** owns everything safety-critical: ADC sampling, threshold logic, the buzzer, the OLED, and the DFPlayer voice output. If the network falls over, this MCU still alarms.
- **ESP8266 NodeMCU** is the network adapter: it receives a JSON line from the STM32 over UART, caches the latest reading, and serves it to phones over its own Wi-Fi access point.

This split matters. The dashboard is for convenience; the buzzer and the speaker are for safety. By keeping them on different chips, network problems can never silence the alarm.

```
                   ┌──────────────────┐
ADC ──── MQ-2  ──▶ │                  │ ── I²C ───▶ SSD1306 OLED
ADC ──── MQ-7  ──▶ │   STM32F103C8T6  │ ── PWM ───▶ Passive buzzer
ADC ──── MQ-135 ─▶ │   (Cortex-M3)    │ ── UART1 ─▶ DFPlayer + speaker
EXTI ─── Button ─▶ │                  │ ── UART3 ─▶ ESP8266 ─── Wi-Fi AP ─── phones
                   └──────────────────┘
```

The STM32 cycle is ~500 ms. Inside that cycle: read three sensors, classify, decide on the worst severity, drive every output, and ship a JSON line to the ESP. The buzzer pattern updates on a faster 10 ms tick to keep its on/off transitions crisp.

---

## 2. The MQ sensor pipeline

Every MQ module presents an analog voltage that is **inversely** related to the sensor resistance. As gas concentration rises, the sensor resistance drops, the divider voltage rises, and the ADC code climbs. The pipeline turns that single number into ppm.

```
ADC code (0–4095)
      │ code_to_vmv()
      ▼
V_pin in mV (0–3300)
      │ vpin_to_vsensor()  ← undo the 10k/20k divider
      ▼
V_sensor in mV (0–5000)
      │ vsensor_to_rs()    ← from V = Vcc · RL / (Rs+RL)
      ▼
Rs in ohms
      │ ÷ R0  (computed at calibration time)
      ▼
ratio_x100  (Rs/R0 × 100, integer)
      │ curve_lookup()
      ▼
ppm
```

### Why the 10 kΩ / 20 kΩ divider matters

The MQ modules need 5 V to power their internal heaters. Their analog output therefore swings 0–5 V. The STM32 ADC can only safely read 0–3.3 V; feeding 5 V to a port pin will damage the chip. The divider drops the swing by a factor of `1000/667 ≈ 1.5`, putting it inside the safe range. In software we reverse the scaling so we get the original sensor voltage back:

```c
#define MQ_VDIV_NUM   667U
#define MQ_VDIV_DEN  1000U

uint16_t v_sensor = ((uint32_t)v_pin_mv * MQ_VDIV_DEN) / MQ_VDIV_NUM;
```

`uint32_t` is essential mid-multiplication. `5000 × 1000 = 5,000,000` blows past `uint16_t` (max 65,535) easily.

### Why integer-only math?

The Cortex-M3 has no FPU. Every floating-point operation gets emulated in software, and a single `float` divide can be ~50 cycles. We have three sensors × ~16 averaged samples × 2 Hz = 96 conversions per second, plus all the unit conversions per cycle. Going integer-only keeps the whole pipeline well under 1 ms of CPU time. Hence the `_x100` ratio scaling: instead of `9.83`, we carry `983` and divide by 100 only when needed.

### The response curves

Each gas has its own (ratio, ppm) lookup table inside `mq_sensor.c`. The table is sorted ascending by ratio. At runtime we walk it once per reading and linearly interpolate between the two bracketing points:

```c
int32_t num  = (p1 - p0) * ((int32_t)ratio_x100 - r0);
int32_t ppm  = p0 + (num / (r1 - r0));
```

`int32_t` is used here because `(p1 - p0)` is *negative* — higher ratio means lower ppm. With unsigned types that subtraction would wrap.

The original datasheet curves are logarithmic, so a linear fit between sample points is mildly inaccurate inside each segment. For a safety alarm whose only real question is *"are we above the threshold?"*, that's fine. The threshold itself is in ppm and is well-separated from the noise floor. The tables also include a few extrapolated low-concentration points so clean air settles to ~0 ppm instead of clamping at the lowest tabulated value.

### Calibration: the most important function in the system

```c
void MQ_Calibrate(MQ_Sensor_t *s)
{
    uint16_t code     = adc_read_avg(s->hadc, s->adc_channel, MQ_CALIB_SAMPLES);
    uint16_t v_pin    = code_to_vmv(code);
    uint16_t v_sensor = vpin_to_vsensor(v_pin);
    uint32_t rs       = vsensor_to_rs(v_sensor);
    s->R0_ohm = (rs * 100U) / (uint32_t)s->clean_air_x100;
}
```

R₀ is the sensor resistance in *clean* air at the calibration moment. Every later reading is reported as a ratio relative to this baseline. If we calibrate in dirty air, every subsequent ppm number is silently wrong — biased low if the air was already gassy, biased high if it was unusually clean. That's why the OLED prints **"Needs fresh air"** and the firmware has a 4-cycle grace period afterwards before alerts can fire.

64 samples are averaged here (versus 16 at runtime) to push the noise floor down before locking in R₀. Get this number wrong once, and you live with bad data forever.

---

## 3. Severity tiers and debouncing

Three tiers, ordered numerically:

```c
typedef enum {
    SEV_NORMAL   = 0,
    SEV_WARNING  = 1,
    SEV_CRITICAL = 2
} Severity_t;
```

Ascending values let `service_sensor_cycle()` find the worst gas with `if (c->severity > worst) …`. A simple comparison only works because of that ordering; if `WARNING` were `2` and `CRITICAL` were `1`, the priority would silently flip.

The cutoffs are anchored to real-world figures rather than to whatever number "looks dangerous":

| Gas | Warn (ppm) | Crit (ppm) | Reasoning |
|---|---|---|---|
| LPG | 1000 | 2000 | ~10% / 20% of LEL — safety margin before any explosion risk |
| CO | 50 | 150 | OSHA 8-hour limit / onset of physiological symptoms |
| CO₂ | 1000 | 2000 | Stuffy / drowsy → cognitive impairment |

### Why two confirmation cycles?

The MQ outputs are noisy. A 50 mV electrical glitch can briefly push the computed ppm above the threshold, and once would be enough to trip the alarm if we acted on the first sample. Instead, every channel keeps a `confirm_count` that increments while severity is non-zero and resets to zero on a clean reading. The buzzer and voice are gated on `confirm_count >= ALERT_CONFIRM_CYCLES` (2). With a 500 ms cycle, that's a one-second confirmation window — fast enough to catch real events, slow enough to filter noise.

### Why a post-calibration grace period?

Right after `MQ_Calibrate()`, R₀ is fresh but the sensor outputs have not fully settled. The first one or two reads can swing wildly. `POST_CALIB_GRACE_CYCLES = 4` (≈2 s) blocks alerts until the sensor outputs settle, preventing a false "DANGER" overlay from greeting the user at boot.

---

## 4. The buzzer — patterns from modular arithmetic

The buzzer driver is one of the smallest files in the project and one of the most satisfying. There is no timer interrupt, no state machine, no flags. The whole pattern generator is this:

```c
uint32_t t = HAL_GetTick();
switch (b_mode) {
    case BUZZER_WARN: want_on = ((t % 1000U) < 250U); break;  // 1 Hz, 25% duty
    case BUZZER_CRIT: want_on = ((t %  200U) < 100U); break;  // 5 Hz, 50% duty
    case BUZZER_OFF:
    default:          want_on = 0;                   break;
}
```

`HAL_GetTick()` returns milliseconds since boot. `t % 1000` cycles 0–999 every second; the first 250 ms of each cycle are "on", the rest are "off" — that's a 1 Hz beep at 25% duty. `t % 200` cycles every 200 ms with 50% on; that's the 5 Hz urgent pattern. No state. No drift. The patterns come straight out of the system tick.

A passive buzzer is essentially a tiny speaker — apply a 2 kHz square wave and you hear a 2 kHz tone. We get that tone from TIM3 in PWM mode (PSC = 71, ARR = 499 → 2 kHz), then *gate* the tone by writing CCR = 0 (silent) or CCR = ARR/2 (50% duty, loudest). The CCR write only happens on transitions:

```c
if (want_on != b_last_on) {
    set_pulse(want_on ? b_tone_ccr : 0);
    b_last_on = want_on;
}
```

Without that guard we'd hammer the timer's compare register hundreds of times a second for no reason.

---

## 5. The DFPlayer driver and alert state machine

The DFPlayer Mini is a tiny MP3 chip with a UART interface. We send 10-byte command frames at 9600 baud and it plays files from a microSD card.

### Frame format

```
0x7E 0xFF 0x06 CMD 0x00 PARAM_HI PARAM_LO CK_HI CK_LO 0xEF
 │    │    │    │   │      │         │        │     │     │
start ver  len  cmd ack    param            checksum   end
```

The fixed start (`0x7E`) and end (`0xEF`) bytes let the receiver re-sync if a byte gets dropped. The checksum is the two's complement of the sum of bytes 1–6, so on the receiver `sum + checksum == 0` if the frame is intact.

### The 1500 ms post-reset delay

The single most important line in `dfplayer.c` is `HAL_Delay(1500)` after the reset command. The DFPlayer datasheet says the chip takes up to 1.5 s to boot and probe its SD card after reset. Send a command before that and it's silently dropped — and there's no error feedback, so debugging this without the datasheet is brutal. Better to block once at boot than to chase a ghost later.

### The high-level alert layer

`DF_Alerts_Update()` is called every sensor cycle. Most of the time it does nothing. It announces only on transitions and on cooldown-expiry, never on every cycle:

```
                  ┌─────────────────┐
                  │ desired_track = │
                  │ pick_track(sev, ch)
                  └────────┬────────┘
                           ▼
            ┌──────────────┴──────────────┐
            ▼                              ▼
   desired == NONE?                desired == NONE?
   (was alerting)                  → play current track
   → "all clear"                     → reset cooldown
                                      ▼
                            same as last announcement?
                              ▼                    ▼
                         (same channel)        (different)
                              ▼                    ▼
                     cooldown expired?       → play new track
                       ▼              ▼
                       → replay     → silent
```

Three pieces of state coordinate this: `al_last_channel` (what was last announced), `al_last_play_ms` (when), and `al_had_alert` (a flag for the falling-edge "all clear"). The 15-second `REPLAY_COOLDOWN_MS` exists because a sustained CO leak should remind the user, but not constantly. Re-announcing every 500 ms would be unbearable.

### Why no voice on warnings?

Audio fatigue. If borderline ppm triggered "Warning, warning" every minute, users would learn to tune the speaker out — and then ignore it during a real critical event. The buzzer covers warnings; voice is reserved for *act-now* moments.

---

## 6. The OLED rendering layer

The SSD1306 driver itself is third-party (Tilen Majerle / Alexander Lutsai port, GPLv3). What we wrote on top of it is `display_ui.c`, a small page-renderer that takes a snapshot of the system state and draws one of five pages:

```c
typedef enum {
    UI_PAGE_OVERVIEW = 0,   // all three sensors, single line each
    UI_PAGE_MQ2      = 1,   // MQ-2 detail with Rs / R0 diagnostics
    UI_PAGE_MQ7      = 2,
    UI_PAGE_MQ135    = 3,
    UI_PAGE_SYSTEM   = 4,   // uptime, R0 of all sensors
} UI_Page_t;
```

The view-model `UI_SensorView_t` is the contract between `main.c` and the renderers — `main.c` builds three of these per cycle, the UI never reaches into the global state.

### The two alert overrides

When `service_sensor_cycle()` confirms a real alert, the page selector is bypassed:

- **`UI_RenderFocusedAlert()` for `WARNING`.** Header `"ALERT  HH:MM"`, the offending gas in 11×18 font, with the other two sensors summarised on a single line at the bottom (`"CO 60* CO2 450"` — the asterisk marks elevated values).
- **`UI_RenderCriticalOverlay()` for `CRITICAL`.** Inverts the whole screen on alternate cycles (`blink_on` toggled in `main.c`), prints `DANGER` at 16×26 and `EVACUATE` at 11×18. Hard to ignore even from across the room.

Every text width was hand-checked against the 128 px screen for worst-case content. `"CO2 65535 ppm"` is 13 characters × 7 px = 91 px, fits comfortably. The comments in `display_ui.c` document those calculations in case fonts ever change.

---

## 7. The cooperative scheduler in `main.c`

The main loop is the canonical "track the last run timestamp per task" pattern:

```c
while (1) {
    uint32_t now = HAL_GetTick();
    if ((now - t_last_buzzer) >= BUZZER_PERIOD_MS) {
        t_last_buzzer = now;
        Buzzer_Update();
    }
    if ((now - t_last_sensor) >= SENSOR_PERIOD_MS) {
        t_last_sensor = now;
        service_sensor_cycle();
    }
    HAL_Delay(1);
}
```

No `HAL_Delay(500)` ever blocks the loop. The buzzer gets a pattern update every 10 ms (essential — its 5 Hz pattern flips every 100 ms) while the sensor cycle runs at 2 Hz. The trailing `HAL_Delay(1)` gives the CPU a one-millisecond breather so we don't burn 100% on a tight `HAL_GetTick()` poll.

### `service_sensor_cycle()` in one sentence

For each of the three channels, read ppm, classify, update the debounce counter, then track the worst severity across all three and dispatch to buzzer + DFPlayer + OLED + ESP. Everything important about the system happens in this one function.

### The button

`HAL_GPIO_EXTI_Callback()` runs in interrupt context. It does the absolute minimum:

```c
if ((now - g_last_btn_ms) <= BTN_DEBOUNCE_MS) return;  // software debounce
g_last_btn_ms = now;
if (g_alert_active) return;                            // no nav during alerts
g_page = (UI_Page_t)((g_page + 1) % UI_PAGE_COUNT);
```

Note `volatile` on the shared variables. Without it, the compiler could cache `g_page` in a register inside the main loop and never see the ISR's write. (See the C standard, §5.1.2.3 — `volatile` is the fence here.)

### The dashboard JSON

`send_dashboard_json()` formats ~150 bytes into a static buffer and ships it over USART3 with a blocking `HAL_UART_Transmit()`. At 9600 baud, 150 bytes take roughly 156 ms — well inside the 500 ms cycle. The `\n` terminator is the frame delimiter on the ESP side. There's no acknowledgement; if the ESP is unplugged the STM32 doesn't notice and doesn't care, which is the right behaviour for a safety device.

---

## 8. The ESP8266 dashboard pipeline

The ESP8266 sketch does three things:

1. **Brings up an open Wi-Fi access point** named `GasSafety` on `192.168.4.1`. No router needed, no internet needed.
2. **Drains its software-serial RX buffer byte-by-byte**, accumulating into a line buffer until it sees `\n`, then parses the JSON with ArduinoJson. Cap at 250 chars guards against runaway input.
3. **Serves two routes**:
   - `GET /` returns the dashboard HTML (stored in PROGMEM, served from flash).
   - `GET /api/data` returns the latest cached JSON plus an `age_ms = millis() - lastRxMs` field. The browser polls this every second and uses `age_ms` to detect a stale link.

The ESP uses **SoftwareSerial** because the NodeMCU's hardware UART is wired to the USB-serial bridge for debugging. Bit-banging UART at 9600 baud is well within reach, and avoiding the hardware UART means we keep the USB log alive while developing.

The data flow end-to-end:

```
 STM32 cycle
      │ 500 ms
      ▼
 send_dashboard_json()
      │ UART3 @ 9600
      ▼
 ESP8266 RX line buffer ── '\n' ──▶ ArduinoJson parse ──▶ cached state
                                                              │
                              every 1 s phone fetch ─────────┘
                              GET /api/data → JSON + age_ms
                                          │
                                          ▼
                              browser updates 3 cards + banner
```

---
