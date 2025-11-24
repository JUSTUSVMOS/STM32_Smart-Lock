// card_db.h  — 白名單 Flash log 管理

#ifndef CARD_DB_H
#define CARD_DB_H

#include <stdint.h>

#define CARD_UID_SIZE        5      // 你目前是 5-byte UID，就先抓 5
#define CARD_DB_MAX_CARDS    32     // RAM 白名單最多幾張卡，自己調

// Flash 相關設定：用你現在專案的定義即可
// 你之前是這樣：
//   #define CARD_FLASH_SECTOR FLASH_SECTOR_11
//   #define CARD_FLASH_ADDR   0x080E0000U
// 我這裡沿用，但加一個 "sector 大小"，請依你的 chip 實際大小調整
#define CARD_FLASH_SECTOR       FLASH_SECTOR_11
#define CARD_FLASH_ADDR         0x080E0000U
// #define CARD_FLASH_SECTOR_SIZE  (128 * 1024U)  // F407 sector 11 是 128KB，確認一下
#define CARD_FLASH_SECTOR_SIZE   64   

// log record 相關常數
#define CARD_LOG_MAGIC       0xA5
#define CARD_LOG_OP_ADD      0x01
#define CARD_LOG_OP_DEL      0x02

// 回傳值
typedef enum {
    CARDDB_OK = 0,
    CARDDB_ERR_FULL,        // Flash log 沒空間了
    CARDDB_ERR_NOT_FOUND,   // 刪卡時找不到
    CARDDB_ERR_FLASH,       // Flash 寫入錯誤
} carddb_status_t;

// 一筆 RAM 裡的卡片條目
typedef struct {
    uint8_t uid[CARD_UID_SIZE];
    uint8_t in_use;         // 1 = 有效，0 = 空/已刪除
} card_entry_t;

// 初始化：開機時呼叫一次
void carddb_init(void);

// 加卡（白名單）: 成功回傳 CARDDB_OK，卡已存在也視為 OK
carddb_status_t carddb_add(const uint8_t uid[CARD_UID_SIZE]);

// 刪卡：找不到會回 CARDDB_ERR_NOT_FOUND
carddb_status_t carddb_remove(const uint8_t uid[CARD_UID_SIZE]);

// 查詢此 UID 是否在白名單中：1 = 在，0 = 不在
int carddb_check(const uint8_t uid[CARD_UID_SIZE]);

// （選用）取得目前白名單內容，方便你 debug / 顯示
int carddb_get_all(card_entry_t *out_array, int max_items);

#endif // CARD_DB_H
