/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "lcd1602_i2c.h"
#include "usart.h"     
#include "rc522.h"  
#include "card_db.h" 
#include <string.h>    
#include <stdio.h>   
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
// NFC mode: normal or add-card
typedef enum {
    NFC_MODE_NORMAL = 0,
    NFC_MODE_ADD_CARD = 1,
    NFC_MODE_DELETE_CARD = 2
} NfcMode_t;

static volatile NfcMode_t gNfcMode = NFC_MODE_NORMAL;

typedef struct {
    char line1[17];
    char line2[17];
} LcdMsg_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define PCF8574_ADDRESS  0b01001110   // = 0x4E
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

#define BT_UART   huart2   // HC-05 
#define DBG_UART  huart3   // TTL / PC Debug

QueueHandle_t xEventQueue;     // BT / NFC / Keypad → StateTask
QueueHandle_t xLcdQ;       
QueueHandle_t xBtRxQ;      

lcd1602_HandleTypeDef hlcd;
uint8_t gBtRxByte;         
static volatile uint8_t gIsUnlocked = 0;

/* USER CODE END PV */


/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void vBtTask(void *argument);     
void vStateTask(void *argument);   
void vLcdTask(void *argument);
void vKeypadTask(void *argument); 
void vNfcTask(void *argument);  

carddb_status_t Nfc_AddCard(const uint8_t uid[5]);
carddb_status_t Nfc_DeleteCard(const uint8_t uid[5]);
bool           Nfc_IsAuthorized(const uint8_t uid[5]);


/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
/* MCU Configuration--------------------------------------------------------*/
HAL_Init();
SystemClock_Config();

MX_GPIO_Init();
MX_DMA_Init();
MX_I2C1_Init();
MX_SPI1_Init();
MX_USART1_UART_Init();
MX_USART2_UART_Init();
MX_USART3_UART_Init();

/* USER CODE BEGIN 2 */
const char *bootMsg = "System boot\r\n";
HAL_UART_Transmit(&huart3, (uint8_t*)bootMsg, strlen(bootMsg), HAL_MAX_DELAY);

MFRC522_Init();

uint8_t ver = Read_MFRC522(VersionReg);
uint8_t txc = Read_MFRC522(TxControlReg);
char buf[64];
int len = sprintf(buf,
                  "RC522 Ver=0x%02X, TxControl=0x%02X\r\n",
                  ver, txc);
HAL_UART_Transmit(&huart3, (uint8_t*)buf, len, HAL_MAX_DELAY);

carddb_init();

void debug_print_carddb_codes(void)
{
    char dbg[64];

    sprintf(dbg, "CARDDB_OK=%d\r\n", (int)CARDDB_OK);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, strlen(dbg), HAL_MAX_DELAY);

    sprintf(dbg, "CARDDB_ERR_FULL=%d\r\n", (int)CARDDB_ERR_FULL);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, strlen(dbg), HAL_MAX_DELAY);

    sprintf(dbg, "CARDDB_ERR_FLASH=%d\r\n", (int)CARDDB_ERR_FLASH);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, strlen(dbg), HAL_MAX_DELAY);

    sprintf(dbg, "CARDDB_ERR_NOT_FOUND=%d\r\n", (int)CARDDB_ERR_NOT_FOUND);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, strlen(dbg), HAL_MAX_DELAY);
}
debug_print_carddb_codes();
card_entry_t snapshot[CARD_DB_MAX_CARDS];
int card_cnt = carddb_get_all(snapshot, CARD_DB_MAX_CARDS);

if (card_cnt == 0)
{
    uint8_t defaultUid[5] = { 0x43, 0x0D, 0xD1, 0x13, 0x8C };
    carddb_status_t st = Nfc_AddCard(defaultUid);

    const char *msg;
    if (st == CARDDB_OK)
        msg = "CardDB empty, add default card\r\n";
    else
        msg = "CardDB empty, add default card FAILED\r\n";

    HAL_UART_Transmit(&DBG_UART, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);
}
else
{
    const char *msg = "Whitelist loaded from CardDB\r\n";
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);
}

  xEventQueue = xQueueCreate(8,  sizeof(uint8_t));   
  xLcdQ   = xQueueCreate(4,  sizeof(LcdMsg_t)); 
  xBtRxQ  = xQueueCreate(32, sizeof(uint8_t));   

  if (xEventQueue == NULL || xLcdQ == NULL || xBtRxQ == NULL)
  {
      const char *err = "Queue create failed!\r\n";
      HAL_UART_Transmit(&huart3, (uint8_t*)err, strlen(err), HAL_MAX_DELAY);
      Error_Handler();
  }

  HAL_UART_Receive_DMA(&huart2, &gBtRxByte, 1);

  xTaskCreate(vBtTask,     "BT",     256, NULL, tskIDLE_PRIORITY + 2, NULL);
  xTaskCreate(vKeypadTask, "KEYPAD", 256, NULL, tskIDLE_PRIORITY + 2, NULL);
  xTaskCreate(vLcdTask,    "LCD",    256, NULL, tskIDLE_PRIORITY + 1, NULL);
  xTaskCreate(vStateTask,  "STATE",  256, NULL, tskIDLE_PRIORITY + 3, NULL);
  xTaskCreate(vNfcTask,    "NFC",    256, NULL, tskIDLE_PRIORITY + 2, NULL);

  vTaskStartScheduler();

  /* USER CODE END 2 */

  while (1)
  {
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 50;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
#define PIN_LEN 4
const char unlockPin[PIN_LEN + 1] = "1234";
const char lockPin[PIN_LEN + 1]   = "0000";

carddb_status_t Nfc_AddCard(const uint8_t uid[5])
{
    return carddb_add(uid);
}

carddb_status_t Nfc_DeleteCard(const uint8_t uid[5])
{
    return carddb_remove(uid);
}

bool Nfc_IsAuthorized(const uint8_t uid[5])
{
    return (carddb_check(uid) != 0);
}

void RC522_TestLoop(void)
{
    uint8_t status;
    uint8_t atqa[2];
    uint8_t uid[5];
    char    msg[100];

    while (1)
    {
        status = MFRC522_Request(PICC_REQIDL, atqa);

        if (status == MI_OK)
        {
            status = MFRC522_Anticoll(uid);
            if (status == MI_OK)
            {
                int len = sprintf(msg,
                    "Card! ATQA=%02X %02X, UID=%02X %02X %02X %02X %02X\r\n",
                    atqa[0], atqa[1],
                    uid[0], uid[1], uid[2], uid[3], uid[4]);
                HAL_UART_Transmit(&huart3, (uint8_t*)msg, len, HAL_MAX_DELAY);

                HAL_Delay(500);
            }
            else
            {
                int len = sprintf(msg, "Anticoll failed, status=%d\r\n", status);
                HAL_UART_Transmit(&huart3, (uint8_t*)msg, len, HAL_MAX_DELAY);
            }
        }
        else
        {
            int len = sprintf(msg, "No card, status=%d\r\n", status);
            HAL_UART_Transmit(&huart3, (uint8_t*)msg, len, HAL_MAX_DELAY);
            HAL_Delay(500);
        }
    }
}

static char read_keypad(void)
{
    static const char keys[4][4] = {
        {'1','2','3','A'},
        {'4','5','6','B'},
        {'7','8','9','C'},
        {'*','0','#','D'}
    };

    for (int col = 0; col < 4; col++)
    {
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_11, (col == 0) ? GPIO_PIN_RESET : GPIO_PIN_SET);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_12, (col == 1) ? GPIO_PIN_RESET : GPIO_PIN_SET);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_13, (col == 2) ? GPIO_PIN_RESET : GPIO_PIN_SET);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_14, (col == 3) ? GPIO_PIN_RESET : GPIO_PIN_SET);

        vTaskDelay(pdMS_TO_TICKS(3));

        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_7) == GPIO_PIN_RESET)  return keys[0][col];
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_8) == GPIO_PIN_RESET)  return keys[1][col];
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_9) == GPIO_PIN_RESET)  return keys[2][col];
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_10) == GPIO_PIN_RESET) return keys[3][col];
    }

    return '\0'; 
}

void vKeypadTask(void *argument)
{
    char pinBuf[PIN_LEN + 1];
    uint8_t idx = 0;
    char key;
    char lastKey = '\0';      
    LcdMsg_t msg;

    snprintf(msg.line1, sizeof(msg.line1), "LOCKED");
    snprintf(msg.line2, sizeof(msg.line2), "PIN: ----");
    xQueueSend(xLcdQ, &msg, 0);

    for (;;)
    {
        char curKey = read_keypad();

        if (curKey == '\0')
        {
            lastKey = '\0';
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (curKey == lastKey)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        lastKey = curKey;
        key = curKey;

        if (key >= '0' && key <= '9')
        {
            if (idx < PIN_LEN)
            {
                pinBuf[idx++] = key;
            }
        }
        else if (key == '*')
        {
            idx = 0;
        }
        else if (key == 'A')
        {
            if (!gIsUnlocked)
            {
                snprintf(msg.line1, sizeof(msg.line1), "LOCKED");
                snprintf(msg.line2, sizeof(msg.line2), "UNLOCK FIRST");
                xQueueSend(xLcdQ, &msg, 0);

                const char *btmsg = "DENY ADD: LOCKED\r\n";
                HAL_UART_Transmit(&BT_UART, (uint8_t *)btmsg, strlen(btmsg), HAL_MAX_DELAY);

                vTaskDelay(pdMS_TO_TICKS(800));
                snprintf(msg.line1, sizeof(msg.line1), "LOCKED");
                snprintf(msg.line2, sizeof(msg.line2), "PIN: ----");
                xQueueSend(xLcdQ, &msg, 0);
                continue;
            }

            gNfcMode = NFC_MODE_ADD_CARD;

            snprintf(msg.line1, sizeof(msg.line1), "ADD CARD MODE");
            snprintf(msg.line2, sizeof(msg.line2), "TAP CARD...");
            xQueueSend(xLcdQ, &msg, 0);

            const char *btmsg = "ENTER ADD CARD MODE\r\n";
            HAL_UART_Transmit(&BT_UART, (uint8_t *)btmsg, strlen(btmsg), HAL_MAX_DELAY);

            idx = 0;
            continue;
        }

        else if (key == 'B')
        {
            if (!gIsUnlocked)
            {
                snprintf(msg.line1, sizeof(msg.line1), "LOCKED");
                snprintf(msg.line2, sizeof(msg.line2), "UNLOCK FIRST");
                xQueueSend(xLcdQ, &msg, 0);

                const char *btmsg = "DENY DELETE: LOCKED\r\n";
                HAL_UART_Transmit(&BT_UART, (uint8_t *)btmsg, strlen(btmsg), HAL_MAX_DELAY);

                vTaskDelay(pdMS_TO_TICKS(800));
                snprintf(msg.line1, sizeof(msg.line1), "LOCKED");
                snprintf(msg.line2, sizeof(msg.line2), "PIN: ----");
                xQueueSend(xLcdQ, &msg, 0);
                continue;
            }

            gNfcMode = NFC_MODE_DELETE_CARD;

            snprintf(msg.line1, sizeof(msg.line1), "DEL CARD MODE");
            snprintf(msg.line2, sizeof(msg.line2), "TAP CARD...");
            xQueueSend(xLcdQ, &msg, 0);

            const char *btmsg = "ENTER DELETE CARD MODE\r\n";
            HAL_UART_Transmit(&BT_UART, (uint8_t*)btmsg, strlen(btmsg), HAL_MAX_DELAY);

            idx = 0;
            continue;
        }

        else if (key == 'C')
        {
            if (gIsUnlocked)   
            {
                gNfcMode = NFC_MODE_NORMAL;

                uint8_t evt = '0';
                xQueueSend(xEventQueue, &evt, portMAX_DELAY);

                const char *btmsg = "KEYPAD C -> LOCK\r\n";
                HAL_UART_Transmit(&BT_UART, (uint8_t*)btmsg, strlen(btmsg), HAL_MAX_DELAY);

                idx = 0;
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            else
            {
                snprintf(msg.line1, sizeof(msg.line1), "ALREADY LOCK");
                snprintf(msg.line2, sizeof(msg.line2), "PIN: ----");
                xQueueSend(xLcdQ, &msg, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            continue;
        }
        else if (key == '#')
        {
            pinBuf[idx] = '\0';

            uint8_t evt;
            if (idx == PIN_LEN && strncmp(pinBuf, unlockPin, PIN_LEN) == 0)
            {
                evt = '1'; // UNLOCK
                xQueueSend(xEventQueue, &evt, portMAX_DELAY);

                const char *btmsg = "KEYPAD UNLOCK\r\n";
                HAL_UART_Transmit(&BT_UART, (uint8_t *)btmsg, strlen(btmsg), HAL_MAX_DELAY);

                snprintf(msg.line1, sizeof(msg.line1), "UNLOCK");
                snprintf(msg.line2, sizeof(msg.line2), "PIN OK");
                xQueueSend(xLcdQ, &msg, 0);

                idx = 0;
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            else if (idx == PIN_LEN && strncmp(pinBuf, lockPin, PIN_LEN) == 0)
            {
                evt = '0'; // LOCK
                xQueueSend(xEventQueue, &evt, portMAX_DELAY);

                const char *btmsg = "KEYPAD LOCK\r\n";
                HAL_UART_Transmit(&BT_UART, (uint8_t *)btmsg, strlen(btmsg), HAL_MAX_DELAY);

                snprintf(msg.line1, sizeof(msg.line1), "LOCK");
                snprintf(msg.line2, sizeof(msg.line2), "PIN OK");
                xQueueSend(xLcdQ, &msg, 0);

                idx = 0;
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            else
            {
                const char *btmsg = "KEYPAD WRONG PIN\r\n";
                HAL_UART_Transmit(&BT_UART, (uint8_t *)btmsg, strlen(btmsg), HAL_MAX_DELAY);

                snprintf(msg.line1, sizeof(msg.line1), "WRONG PIN");
                snprintf(msg.line2, sizeof(msg.line2), "TRY AGAIN");
                xQueueSend(xLcdQ, &msg, 0);

                idx = 0;
                vTaskDelay(pdMS_TO_TICKS(500));
                snprintf(msg.line1, sizeof(msg.line1), "LOCKED");
                snprintf(msg.line2, sizeof(msg.line2), "PIN: ----");
                xQueueSend(xLcdQ, &msg, 0);
            }

            continue;
        }

        memset(msg.line1, ' ', sizeof(msg.line1));
        memset(msg.line2, ' ', sizeof(msg.line2));
        snprintf(msg.line1, sizeof(msg.line1), "LOCKED");
        snprintf(msg.line2, sizeof(msg.line2), "PIN: ");

        for (int i = 0; i < idx && i < PIN_LEN; i++)
        {
            msg.line2[5 + i] = '*';
        }
        msg.line2[5 + PIN_LEN] = '\0';

        xQueueSend(xLcdQ, &msg, 0);
    }
}

void vNfcTask(void *argument)
{
    uint8_t status;
    uint8_t tagType[2] = {0};
    uint8_t uid[5]     = {0};
    LcdMsg_t msg;
    char dbg[80];

    HAL_UART_Transmit(&DBG_UART,
                      (uint8_t *)"NFC TASK START\r\n",
                      strlen("NFC TASK START\r\n"),
                      HAL_MAX_DELAY);

    for (;;)
    {
        // heart beat debug
        HAL_UART_Transmit(&DBG_UART,
                          (uint8_t *)"NFC: loop...\r\n",
                          strlen("NFC: loop...\r\n"),
                          HAL_MAX_DELAY);

        // 1) scan
        status = MFRC522_Request(PICC_REQIDL, tagType);

        // print status 
        int len = sprintf(dbg, "NFC: Request status=%d\r\n", status);
        HAL_UART_Transmit(&DBG_UART, (uint8_t *)dbg, len, HAL_MAX_DELAY);

        if (status == MI_OK)
        {
            len = sprintf(dbg,
                          "NFC: Card detected, ATQA=%02X %02X\r\n",
                          tagType[0], tagType[1]);
            HAL_UART_Transmit(&DBG_UART, (uint8_t *)dbg, len, HAL_MAX_DELAY);

            status = MFRC522_Anticoll(uid);

            len = sprintf(dbg, "NFC: Anticoll status=%d\r\n", status);
            HAL_UART_Transmit(&DBG_UART, (uint8_t *)dbg, len, HAL_MAX_DELAY);

            if (status == MI_OK)
            {
                len = sprintf(dbg,
                              "NFC: UID=%02X %02X %02X %02X %02X\r\n",
                              uid[0], uid[1], uid[2], uid[3], uid[4]);
                HAL_UART_Transmit(&DBG_UART, (uint8_t *)dbg, len, HAL_MAX_DELAY);

                snprintf(msg.line1, sizeof(msg.line1), "CARD DETECTED");
                snprintf(msg.line2, sizeof(msg.line2),
                         "%02X%02X%02X%02X",
                         uid[0], uid[1], uid[2], uid[3]);
                xQueueSend(xLcdQ, &msg, 0);

                if (gNfcMode == NFC_MODE_ADD_CARD)
                {
                    HAL_UART_Transmit(&DBG_UART,
                                    (uint8_t *)"NFC: ADD_CARD mode\r\n",
                                    strlen("NFC: ADD_CARD mode\r\n"),
                                    HAL_MAX_DELAY);

                    carddb_status_t st = Nfc_AddCard(uid);
                    

                    if (st == CARDDB_OK)
                    {
                        const char *btmsg = "ADD CARD OK\r\n";
                        HAL_UART_Transmit(&BT_UART,
                                        (uint8_t *)btmsg,
                                        strlen(btmsg),
                                        HAL_MAX_DELAY);

                        snprintf(msg.line1, sizeof(msg.line1), "CARD SAVED");
                        snprintf(msg.line2, sizeof(msg.line2), "UID ADDED");
                        xQueueSend(xLcdQ, &msg, 0);
                    }
                    else if (st == CARDDB_ERR_FULL)
                    {
                        const char *btmsg = "ADD FAIL: FLASH FULL\r\n";
                        HAL_UART_Transmit(&BT_UART,
                                        (uint8_t *)btmsg,
                                        strlen(btmsg),
                                        HAL_MAX_DELAY);

                        snprintf(msg.line1, sizeof(msg.line1), "ADD FAIL");
                        snprintf(msg.line2, sizeof(msg.line2), "FLASH FULL");
                        xQueueSend(xLcdQ, &msg, 0);
                    }
                    else
                    {
                        char dbg2[64];
                        int len2 = sprintf(dbg2, "ADD ERR, st=%d\r\n", (int)st);
                        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg2, len2, HAL_MAX_DELAY);
                        const char *btmsg = "ADD FAIL: FLASH ERR\r\n";
                        HAL_UART_Transmit(&BT_UART,
                                        (uint8_t *)btmsg,
                                        strlen(btmsg),
                                        HAL_MAX_DELAY);

                        snprintf(msg.line1, sizeof(msg.line1), "ADD FAIL");
                        snprintf(msg.line2, sizeof(msg.line2), "FLASH ERR");
                        xQueueSend(xLcdQ, &msg, 0);
                    }

                    gNfcMode = NFC_MODE_NORMAL;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }

                else if (gNfcMode == NFC_MODE_DELETE_CARD)
                {
                    HAL_UART_Transmit(&DBG_UART,
                                    (uint8_t *)"NFC: DELETE_CARD mode\r\n",
                                    strlen("NFC: DELETE_CARD mode\r\n"),
                                    HAL_MAX_DELAY);

                carddb_status_t st = Nfc_DeleteCard(uid);

                if (st == CARDDB_OK)
                {
                    const char *btmsg = "DELETE OK\r\n";
                    HAL_UART_Transmit(&BT_UART,
                                    (uint8_t *)btmsg,
                                    strlen(btmsg),
                                    HAL_MAX_DELAY);

                    snprintf(msg.line1, sizeof(msg.line1), "CARD DELETED");
                    snprintf(msg.line2, sizeof(msg.line2), "SUCCESS");
                    xQueueSend(xLcdQ, &msg, 0);
                }
                else if (st == CARDDB_ERR_NOT_FOUND)
                {
                    const char *btmsg = "DELETE FAIL\r\n";
                    HAL_UART_Transmit(&BT_UART,
                                    (uint8_t *)btmsg,
                                    strlen(btmsg),
                                    HAL_MAX_DELAY);

                    snprintf(msg.line1, sizeof(msg.line1), "DELETE FAIL");
                    snprintf(msg.line2, sizeof(msg.line2), "NOT FOUND");
                    xQueueSend(xLcdQ, &msg, 0);
                }
                else
                {
                    char dbg2[64];
                    int len2 = sprintf(dbg2, "DEL ERR, st=%d\r\n", (int)st);
                    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg2, len2, HAL_MAX_DELAY);
                    const char *btmsg = "DELETE FAIL: FLASH ERR\r\n";
                    HAL_UART_Transmit(&BT_UART,
                                    (uint8_t *)btmsg,
                                    strlen(btmsg),
                                    HAL_MAX_DELAY);

                    snprintf(msg.line1, sizeof(msg.line1), "DELETE FAIL");
                    snprintf(msg.line2, sizeof(msg.line2), "FLASH ERR");
                    xQueueSend(xLcdQ, &msg, 0);
                }

                    gNfcMode = NFC_MODE_NORMAL;
                    vTaskDelay(pdMS_TO_TICKS(800));
                }

                else
                {
                    HAL_UART_Transmit(&DBG_UART,
                                      (uint8_t *)"NFC: NORMAL mode\r\n",
                                      strlen("NFC: NORMAL mode\r\n"),
                                      HAL_MAX_DELAY);

                    if (Nfc_IsAuthorized(uid))
                    {
                        uint8_t evt = '1';
                        xQueueSend(xEventQueue, &evt, 0);

                        const char *btmsg = "NFC AUTH UNLOCK\r\n";
                        HAL_UART_Transmit(&BT_UART,
                                          (uint8_t *)btmsg,
                                          strlen(btmsg),
                                          HAL_MAX_DELAY);

                        snprintf(msg.line1, sizeof(msg.line1), "NFC UNLOCK");
                        snprintf(msg.line2, sizeof(msg.line2), "AUTHORIZED");
                        xQueueSend(xLcdQ, &msg, 0);

                        HAL_GPIO_TogglePin(GPIOD, LD6_Pin);
                        vTaskDelay(pdMS_TO_TICKS(150));
                        HAL_GPIO_TogglePin(GPIOD, LD6_Pin);

                        vTaskDelay(pdMS_TO_TICKS(800));
                    }
                    else
                    {
                        const char *btmsg = "NFC UNKNOWN CARD\r\n";
                        HAL_UART_Transmit(&BT_UART,
                                          (uint8_t *)btmsg,
                                          strlen(btmsg),
                                          HAL_MAX_DELAY);

                        snprintf(msg.line1, sizeof(msg.line1), "CARD DENIED");
                        snprintf(msg.line2, sizeof(msg.line2), "NOT AUTH");
                        xQueueSend(xLcdQ, &msg, 0);

                        vTaskDelay(pdMS_TO_TICKS(800));
                    }
                }
            }
            else
            {
                int len2 = sprintf(dbg,
                                   "NFC: Anticoll failed, status=%d\r\n",
                                   status);
                HAL_UART_Transmit(&DBG_UART, (uint8_t *)dbg, len2, HAL_MAX_DELAY);

                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart2)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        HAL_GPIO_TogglePin(GPIOD, LD4_Pin);

        xQueueSendFromISR(xBtRxQ, &gBtRxByte, &xHigherPriorityTaskWoken);

        HAL_UART_Receive_DMA(&huart2, &gBtRxByte, 1);

        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}



void I2C_ScanBus(void)
{
    char buf[32];
    HAL_UART_Transmit(&huart2,
                      (uint8_t *)"I2C SCAN START\r\n",
                      strlen("I2C SCAN START\r\n"),
                      HAL_MAX_DELAY);

    for (uint8_t addr = 1; addr < 127; addr++)
    {
        if (HAL_I2C_IsDeviceReady(&hi2c1, addr << 1, 1, 10) == HAL_OK)
        {
            int len = snprintf(buf, sizeof(buf),
                               "Found I2C device at 0x%02X\r\n", addr);
            HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, HAL_MAX_DELAY);
        }
    }

    HAL_UART_Transmit(&huart2,
                      (uint8_t *)"I2C SCAN DONE\r\n",
                      strlen("I2C SCAN DONE\r\n"),
                      HAL_MAX_DELAY);
}

void vBtTask(void *argument)
{
    char    pinBuf[PIN_LEN + 1];
    uint8_t ch;
    int     idx;

    const char *hello  = "HC-05 ready\r\n";
    const char *prompt = "Enter 4-digit code:\r\n";

    HAL_UART_Transmit(&BT_UART, (uint8_t *)hello,  strlen(hello),  HAL_MAX_DELAY);
    HAL_UART_Transmit(&BT_UART, (uint8_t *)prompt, strlen(prompt), HAL_MAX_DELAY);

    for (;;)
    {
        idx = 0;

        while (1)
        {
            if (xQueueReceive(xBtRxQ, &ch, portMAX_DELAY) != pdPASS)
                continue;

            if (ch == '\r' || ch == '\n')
            {
                break;
            }

            if (ch >= '0' && ch <= '9')
            {
                if (idx < PIN_LEN)
                    pinBuf[idx++] = (char)ch;
            }
        }

        if (idx == 0)
        {
            continue;
        }

        pinBuf[idx] = '\0';

        if (idx == PIN_LEN && strncmp(pinBuf, unlockPin, PIN_LEN) == 0)
        {
            uint8_t evt = '1';
            xQueueSend(xEventQueue, &evt, portMAX_DELAY);

        }
        else if (idx == PIN_LEN && strncmp(pinBuf, lockPin, PIN_LEN) == 0)
        {
            uint8_t evt = '0';
            xQueueSend(xEventQueue, &evt, portMAX_DELAY);

            HAL_UART_Transmit(&BT_UART,
                              (uint8_t *)prompt,
                              strlen(prompt),
                              HAL_MAX_DELAY);
        }
        else
        {
            HAL_UART_Transmit(&BT_UART,
                              (uint8_t *)"WRONG\r\n",
                              strlen("WRONG\r\n"),
                              HAL_MAX_DELAY);

            HAL_UART_Transmit(&BT_UART,
                              (uint8_t *)prompt,
                              strlen(prompt),
                              HAL_MAX_DELAY);
        }
    }
}


void vLcdTask(void *argument)
{
    lcd1602_Init(&hlcd, &hi2c1, PCF8574_ADDRESS);
    lcd1602_LED(&hlcd, ENABLE);
    lcd1602_Clear(&hlcd);

    LcdMsg_t msg;

    for (;;)
    {
        if (xQueueReceive(xLcdQ, &msg, portMAX_DELAY) == pdPASS)
        {
            lcd1602_Clear(&hlcd);
            lcd1602_SetCursor(&hlcd, 0, 0);
            lcd1602_Print(&hlcd, (uint8_t*)msg.line1);
            lcd1602_SetCursor(&hlcd, 0, 1);
            lcd1602_Print(&hlcd, (uint8_t*)msg.line2);
        }
    }
}

void vStateTask(void *argument)
{
    uint8_t cmd;

    HAL_UART_Transmit(&DBG_UART,
                      (uint8_t *)"STATE TASK STARTED\r\n",
                      strlen("STATE TASK STARTED\r\n"),
                      HAL_MAX_DELAY);

    LcdMsg_t bootMsg;
    snprintf(bootMsg.line1, sizeof(bootMsg.line1), "LOCKED");
    snprintf(bootMsg.line2, sizeof(bootMsg.line2), "WAITING PIN");
    xQueueSend(xLcdQ, &bootMsg, 0);

    gIsUnlocked = false; 

    for (;;)
    {
        if (xQueueReceive(xEventQueue, &cmd, portMAX_DELAY) == pdPASS)
        {
            LcdMsg_t msg;

            if (cmd == '1')  
            {
                gIsUnlocked = true;   

                HAL_GPIO_WritePin(GPIOD, LD4_Pin | LD5_Pin, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(GPIOD, LD4_Pin, GPIO_PIN_SET);

                HAL_UART_Transmit(&DBG_UART,
                                  (uint8_t *)"STATE: UNLOCK\r\n",
                                  strlen("STATE: UNLOCK\r\n"),
                                  HAL_MAX_DELAY);

                HAL_UART_Transmit(&BT_UART,
                                  (uint8_t *)"UNLOCK\r\n",
                                  strlen("UNLOCK\r\n"),
                                  HAL_MAX_DELAY);

                snprintf(msg.line1, sizeof(msg.line1), "UNLOCK");
                snprintf(msg.line2, sizeof(msg.line2), "        ");
                xQueueSend(xLcdQ, &msg, 0);
            }
            else if (cmd == '0')  // LOCK
            {
                gIsUnlocked = false;  

                HAL_GPIO_WritePin(GPIOD, LD4_Pin | LD5_Pin, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(GPIOD, LD5_Pin, GPIO_PIN_SET);

                HAL_UART_Transmit(&DBG_UART,
                                  (uint8_t *)"STATE: LOCK\r\n",
                                  strlen("STATE: LOCK\r\n"),
                                  HAL_MAX_DELAY);

                HAL_UART_Transmit(&BT_UART,
                                  (uint8_t *)"LOCK\r\n",
                                  strlen("LOCK\r\n"),
                                  HAL_MAX_DELAY);

                snprintf(msg.line1, sizeof(msg.line1), "LOCK");
                snprintf(msg.line2, sizeof(msg.line2), "       ");
                xQueueSend(xLcdQ, &msg, 0);
            }
        }
    }
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM7 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM7) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

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

#ifdef  USE_FULL_ASSERT
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
