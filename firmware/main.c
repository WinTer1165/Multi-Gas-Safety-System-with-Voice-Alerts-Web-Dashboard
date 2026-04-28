/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "ssd1306.h"
#include "fonts.h"
#include "mq_sensor.h"
#include "thresholds.h"
#include "display_ui.h"
#include "buzzer.h"
#include "dfplayer.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SENSOR_PERIOD_MS     500U
#define BUZZER_PERIOD_MS      10U
#define WARMUP_SECONDS        30U
#define BTN_DEBOUNCE_MS      200U
#define CHANNEL_COUNT          3U
#define DF_DEFAULT_VOLUME     25U 

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */
typedef struct {
    /* Static config */
    const char        *name;
    const char        *gas_label;
    uint32_t           adc_channel;
    uint16_t           clean_air_x100;
    const MQ_Curve_t  *curve;
    uint16_t           warn_th;
    uint16_t           crit_th;
    DF_Channel_t       df_id;        /* which DFPlayer channel-id to report */
 
    /* Runtime */
    MQ_Sensor_t        sensor;
    uint16_t           ppm;
    Severity_t         severity;
    uint8_t            confirm_count;
} GasChannel_t;
 
static GasChannel_t g_ch[CHANNEL_COUNT] = {
    { .name = "MQ-2",   .gas_label = "LPG",
      .adc_channel = ADC_CHANNEL_0, .clean_air_x100 = MQ2_CLEAN_AIR_X100,
      .curve = &MQ2_Curve_LPG,
      .warn_th = TH_MQ2_LPG_WARN,   .crit_th = TH_MQ2_LPG_CRIT,
      .df_id = DF_CH_LPG   },
 
    { .name = "MQ-7",   .gas_label = "CO",
      .adc_channel = ADC_CHANNEL_2, .clean_air_x100 = MQ7_CLEAN_AIR_X100,
      .curve = &MQ7_Curve_CO,
      .warn_th = TH_MQ7_CO_WARN,    .crit_th = TH_MQ7_CO_CRIT,
      .df_id = DF_CH_CO    },
 
    { .name = "MQ-135", .gas_label = "CO2",
      .adc_channel = ADC_CHANNEL_1, .clean_air_x100 = MQ135_CLEAN_AIR_X100,
      .curve = &MQ135_Curve_CO2,
      .warn_th = TH_MQ135_CO2_WARN, .crit_th = TH_MQ135_CO2_CRIT,
      .df_id = DF_CH_CO2   },
};
 
static volatile UI_Page_t  g_page        = UI_PAGE_OVERVIEW;
static volatile uint32_t   g_last_btn_ms = 0;
static volatile uint8_t    g_alert_active = 0;
static uint8_t             g_blink_phase  = 0;
static uint8_t             g_grace_cycles_left = POST_CALIB_GRACE_CYCLES;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART3_UART_Init(void);
/* USER CODE BEGIN PFP */
static void run_warmup(void);
static void run_calibration(void);
static void service_sensor_cycle(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void run_warmup(void)
{
    Buzzer_SetMode(BUZZER_OFF);
    for (uint32_t sec = WARMUP_SECONDS; sec > 0; sec--) {
        UI_ShowWarmupFrame(sec, WARMUP_SECONDS);
        HAL_GPIO_TogglePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin);
        for (int i = 0; i < 100; i++) { Buzzer_Update(); HAL_Delay(10); }
    }
}
 
static void run_calibration(void)
{
    Buzzer_SetMode(BUZZER_OFF);
 
    UI_ShowCalibrating();
    HAL_Delay(1500);
 
    for (unsigned i = 0; i < CHANNEL_COUNT; i++) {
        MQ_Calibrate(&g_ch[i].sensor);
    }
 
    UI_ShowCalibrationDone();
    HAL_Delay(1500);
 
    g_grace_cycles_left = POST_CALIB_GRACE_CYCLES;
}
 
/* Build a UI_SensorView_t from a channel — small helper. */
static UI_SensorView_t make_view(GasChannel_t *c) {
    UI_SensorView_t v = {
        .sensor   = &c->sensor,
        .name     = c->name,
        .gas      = c->gas_label,
        .ppm      = c->ppm,
        .severity = c->severity,
    };
    return v;
}

/* Shared severity strings — keep in sync with JS in ESP8266 dashboard */
static const char * const sev_str[] = { "OK", "WARN", "CRIT" };

/* Push one JSON line over USART3 to the ESP8266. Called once per cycle. */
static void send_dashboard_json(Severity_t worst_sev,
                                unsigned   worst_idx,
                                uint8_t    alert_active)
{
    static char buf[240];

    const char *active_gas = alert_active ? g_ch[worst_idx].name : "";

    int len = snprintf(buf, sizeof(buf),
        "{"
          "\"mq2\":{\"ppm\":%u,\"sev\":\"%s\"},"
          "\"mq7\":{\"ppm\":%u,\"sev\":\"%s\"},"
          "\"mq135\":{\"ppm\":%u,\"sev\":\"%s\"},"
          "\"alert\":{\"active\":%s,\"gas\":\"%s\",\"level\":\"%s\"},"
          "\"up\":%lu"
        "}\n",
        g_ch[0].ppm, sev_str[g_ch[0].severity],
        g_ch[1].ppm, sev_str[g_ch[1].severity],
        g_ch[2].ppm, sev_str[g_ch[2].severity],
        alert_active ? "true" : "false",
        active_gas,
        sev_str[worst_sev],
        (unsigned long)HAL_GetTick());

    if (len > 0) {
        HAL_UART_Transmit(&huart3, (uint8_t *)buf, (uint16_t)len, 200);
    }
}

static void service_sensor_cycle(void)
{
    Severity_t    worst_severity = SEV_NORMAL;
    unsigned      worst_idx      = 0;
 
    /* 1. Sample + classify + debounce */
    for (unsigned i = 0; i < CHANNEL_COUNT; i++) {
        GasChannel_t *c = &g_ch[i];
        c->ppm      = MQ_ReadPPM(&c->sensor, c->curve);
        c->severity = classify_ppm(c->ppm, c->warn_th, c->crit_th);
 
        if (c->severity == SEV_NORMAL) {
            c->confirm_count = 0;
        } else if (c->confirm_count < 255) {
            c->confirm_count++;
        }
 
        if (c->severity > worst_severity) {
            worst_severity = c->severity;
            worst_idx      = i;
        }
    }
 
    /* 2. Is the worst channel "confirmed" and out of grace? */
    uint8_t worst_confirmed =
        (worst_severity != SEV_NORMAL) &&
        (g_ch[worst_idx].confirm_count >= ALERT_CONFIRM_CYCLES) &&
        (g_grace_cycles_left == 0);
 
    /* 3. Buzzer policy */
    Buzzer_Mode_t buzz = BUZZER_OFF;
    if (g_grace_cycles_left > 0) {
        g_grace_cycles_left--;
    } else if (worst_confirmed) {
        buzz = (worst_severity == SEV_CRITICAL) ? BUZZER_CRIT : BUZZER_WARN;
    }
    Buzzer_SetMode(buzz);
 
    /* 3b. Voice alerts (DFPlayer).
     * Only CRITICAL severities produce voice — warnings stay buzzer-only
     * per your audio-script mapping. The alerts layer handles:
     *   - announcing new criticals
     *   - replaying sustained criticals every 15 s
     *   - playing 0011.mp3 (all-clear) on return to normal
     * It does nothing during grace period or for unconfirmed alerts. */
    DF_Severity_t df_sev = DF_SEV_NORMAL;
    DF_Channel_t  df_ch  = DF_CH_NONE;
    if (worst_confirmed && worst_severity == SEV_CRITICAL) {
        df_sev = DF_SEV_CRITICAL;
        df_ch  = g_ch[worst_idx].df_id;
    }
    DF_Alerts_Update(df_sev, df_ch, HAL_GetTick());
 
   /* 4. Render */
    uint32_t now_ms = HAL_GetTick();

   if (worst_confirmed) {
        /* Unified alert view — same layout for WARN and CRIT.
         * Flashes every cycle (500 ms) by inverting the whole display,
         * so a glance at the OLED says "something is wrong" even if
         * you can't read the text yet. */
        g_alert_active = 1;
        g_blink_phase ^= 1;

        UI_SensorView_t v_primary = make_view(&g_ch[worst_idx]);

        UI_SensorView_t v_a, v_b;
        const UI_SensorView_t *others[2];
        unsigned k = 0;
        for (unsigned i = 0; i < CHANNEL_COUNT; i++) {
            if (i == worst_idx) continue;
            if (k == 0) { v_a = make_view(&g_ch[i]); others[0] = &v_a; }
            else        { v_b = make_view(&g_ch[i]); others[1] = &v_b; }
            k++;
        }
        UI_RenderFocusedAlert(&v_primary, others, now_ms);
        SSD1306_InvertDisplay(g_blink_phase);
    }
    else {
        /* All calm — regular page navigation */
        g_alert_active = 0;

        /* Ensure display is back to normal polarity when alert clears */
        if (g_blink_phase) {
            g_blink_phase = 0;
            SSD1306_InvertDisplay(0);
        }

        UI_SensorView_t v[CHANNEL_COUNT];
        for (unsigned i = 0; i < CHANNEL_COUNT; i++) {
            v[i] = make_view(&g_ch[i]);
        }
        UI_RenderPage(g_page, &v[0], &v[1], &v[2], now_ms);
    }
 
    HAL_GPIO_TogglePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin);
		/* 5. Push latest state to ESP8266 dashboard */
    send_dashboard_json(worst_severity, worst_idx,
                        (uint8_t)(worst_confirmed ? 1 : 0));
}
 
/* Button: advance page, but only when there's no active alert. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != BTN_NEXT_Pin) return;
 
    uint32_t now = HAL_GetTick();
    if ((now - g_last_btn_ms) <= BTN_DEBOUNCE_MS) return;
    g_last_btn_ms = now;
 
    /* Ignore button presses while an alert is being shown, so the user
     * can't accidentally navigate away from a real warning. */
    if (g_alert_active) return;
 
    g_page = (UI_Page_t)((g_page + 1) % UI_PAGE_COUNT);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  MX_TIM3_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
	Buzzer_Init(&htim3, TIM_CHANNEL_3);
 
    /* Bring up DFPlayer. Init blocks ~1.8 s (module bootup + config). */
    DF_Alerts_Init(&huart1, DF_DEFAULT_VOLUME);
		HAL_Delay(500);
		 
    if (SSD1306_Init() == 0) {
        while (1) {
            HAL_GPIO_TogglePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin);
            HAL_Delay(100);
        }
    }
 
    UI_ShowSplash();
    DF_Alerts_PlayStartup();           /* 0001.mp3 — "System activated..." */
    HAL_Delay(1500);
 
    for (unsigned i = 0; i < CHANNEL_COUNT; i++) {
        MQ_Init(&g_ch[i].sensor, &hadc1,
                g_ch[i].adc_channel, g_ch[i].clean_air_x100);
    }
 
    run_warmup();
    run_calibration();
 
    DF_Alerts_PlayReady();             /* 0002.mp3 — "System ready..." */
 
    uint32_t t_last_sensor = HAL_GetTick();
    uint32_t t_last_buzzer = HAL_GetTick();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
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
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 400000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 71;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 499;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_TIM_DISABLE_OCxPRELOAD(&htim3, TIM_CHANNEL_3);
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 9600;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_STATUS_Pin */
  GPIO_InitStruct.Pin = LED_STATUS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_STATUS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : BTN_NEXT_Pin */
  GPIO_InitStruct.Pin = BTN_NEXT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BTN_NEXT_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
