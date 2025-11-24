#include "card_db.h"           // carddb_status_t, card_entry_t, CARD_UID_SIZE...
#include "stm32f4xx_hal.h"     // HAL_FLASH_xxx, HAL_UART_xxx
#include "usart.h"             // UART handle (huart3)
#include <string.h>            // memcpy, memcmp
#include <stdio.h>             // snprintf

// ========================= Debug UART selection =========================
#define DBG_UART huart3

static void carddb_debug_flash_error(const char *tag)
{
    uint32_t err = HAL_FLASH_GetError();
    char buf[64];
    int len = snprintf(buf, sizeof(buf),
                       "%s: HAL_FLASH error=0x%08lX\r\n",
                       tag, (unsigned long)err);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)buf, len, HAL_MAX_DELAY);
}

// ========================= Flash log basic constants ======================

#ifndef CARD_FLASH_MAGIC
#define CARD_FLASH_MAGIC  0x54U   // 'T'
#endif

// --------- Flash log record layout ---------------------------------
// The structure size is a multiple of 4 bytes, which is convenient for Flash writes.
typedef struct {
    uint8_t  magic;                 // Fixed = CARD_FLASH_MAGIC (1 byte)
    uint8_t  op;                    // ADD / DEL (1 byte)
    uint8_t  uid[CARD_UID_SIZE];    // 5 bytes
    uint8_t  pad0;                  // 1 byte, keeps seq aligned on an even boundary
    uint16_t seq;                   // Monotonic sequence number, larger is newer
    uint16_t crc;                   // CRC16 over all previous fields
} card_log_t;

#define CARD_LOG_SIZE  (sizeof(card_log_t))

// Compile-time check: size must be a multiple of 4 bytes.
typedef char cardlog_size_check[(sizeof(card_log_t) % 4) == 0 ? 1 : -1];



// --------- RAM whitelist data ---------------------------------------

// --------- Flash blocks (for wear leveling) -------------------------
// We use 2 Flash sectors as two blocks and alternate which one is active.

typedef struct {
    uint32_t sector;      // FLASH_SECTOR_x
    uint32_t base_addr;   // Start address
    uint32_t size;        // Block size in bytes
    uint32_t erase_count; // In-RAM counter, just for wear-leveling demo
} card_block_t;

#define CARD_BLOCK_COUNT   2

// ★ Address and size must match your MCU; values below are typical for STM32F407.
static card_block_t g_blocks[CARD_BLOCK_COUNT] = {
    { FLASH_SECTOR_10, 0x080C0000U, 0x20000U, 0 },  // Block 0: 128KB
    { FLASH_SECTOR_11, 0x080E0000U, 0x20000U, 0 },  // Block 1: 128KB
};

static int      g_active_block = 0;  // Which block is currently active
static uint16_t g_last_seq     = 0;  // Largest sequence number seen in log
static uint32_t g_next_addr    = 0;  // Next log address inside the active block

// Whitelist state stored in RAM
static card_entry_t g_cards[CARD_DB_MAX_CARDS];

// Convenience macros
#define CUR_BLOCK      (g_blocks[g_active_block])
#define CUR_BASE_ADDR  (CUR_BLOCK.base_addr)
#define CUR_BLOCK_SIZE (CUR_BLOCK.size)


// --------- Small helpers: UID compare / copy ------------------------------

static int uid_equal(const uint8_t a[CARD_UID_SIZE],
                     const uint8_t b[CARD_UID_SIZE])
{
    return (memcmp(a, b, CARD_UID_SIZE) == 0);
}

static void uid_copy(uint8_t dst[CARD_UID_SIZE],
                     const uint8_t src[CARD_UID_SIZE])
{
    memcpy(dst, src, CARD_UID_SIZE);
}

// --------- Simple CRC16 (nice to mention in interviews) ---------------

static uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static uint16_t card_log_crc(const card_log_t *rec)
{
    // Compute CRC over all fields except the crc field itself.
    return crc16_ccitt((const uint8_t *)rec,
                       sizeof(card_log_t) - sizeof(rec->crc));
}

// --------- Flash helper functions -----------------------------------------

// Check whether a flash region is all 0xFF (i.e., never programmed).
static int flash_region_is_erased(uint32_t addr, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)addr;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] != 0xFF) {
            return 0; // Not empty
        }
    }
    return 1; // All 0xFF
}

// Read one log record from Flash (basically just treating Flash as const memory).
static void flash_read_log(uint32_t addr, card_log_t *out)
{
    memcpy(out, (const void *)addr, sizeof(card_log_t));
}

// Write one log record to Flash (word-by-word).
static carddb_status_t flash_write_log(uint32_t addr, const card_log_t *rec)
{
    HAL_StatusTypeDef hal_status;

    // Flash programming requires 32-bit aligned addresses.
    if ((addr % 4) != 0) {
        // ★ Extra: log an error when alignment is wrong.
        carddb_debug_flash_error("ALIGN_ERR");
        return CARDDB_ERR_FLASH;
    }

    HAL_FLASH_Unlock();

    const uint8_t *p = (const uint8_t *)rec;
    uint32_t current_addr = addr;

    // Program in units of 4 bytes.
    for (uint32_t i = 0; i < sizeof(card_log_t); i += 4) {
        uint32_t word = 0xFFFFFFFFU;
        // For the last partial word, remaining bytes stay as 0xFF.
        memcpy(&word, p + i, 4);

        hal_status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                       current_addr,
                                       word);
        if (hal_status != HAL_OK) {
            // ★ Extra: print HAL_FLASH_GetError() when programming fails.
            carddb_debug_flash_error("PROG_ERR");
            HAL_FLASH_Lock();
            return CARDDB_ERR_FLASH;
        }

        current_addr += 4;
    }

    HAL_FLASH_Lock();
    return CARDDB_OK;
}

// Erase the entire sector corresponding to the given block and update erase_count.
static carddb_status_t flash_erase_block(int block_idx)
{
    HAL_StatusTypeDef      hal_status;
    FLASH_EraseInitTypeDef erase_init;
    uint32_t               sector_error = 0;

    if (block_idx < 0 || block_idx >= CARD_BLOCK_COUNT) {
        return CARDDB_ERR_FLASH;
    }

    erase_init.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector       = g_blocks[block_idx].sector;
    erase_init.NbSectors    = 1;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_FLASH_Unlock();
    hal_status = HAL_FLASHEx_Erase(&erase_init, &sector_error);
    HAL_FLASH_Lock();

    if (hal_status != HAL_OK) {
        return CARDDB_ERR_FLASH;
    }

    g_blocks[block_idx].erase_count++;

    char dbg[64];
    int len = snprintf(dbg, sizeof(dbg),
                       "FLASH ERASE OK: block=%d erase_count=%lu\r\n",
                       block_idx,
                       (unsigned long)g_blocks[block_idx].erase_count);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);

    return CARDDB_OK;
}

// --------- Operations on the whitelist in RAM ---------------------------------

// Find UID index in RAM whitelist, return -1 if not found.
static int carddb_find_in_ram(const uint8_t uid[CARD_UID_SIZE])
{
    for (int i = 0; i < CARD_DB_MAX_CARDS; i++) {
        if (g_cards[i].in_use && uid_equal(g_cards[i].uid, uid)) {
            return i;
        }
    }
    return -1;
}

// Add UID to RAM whitelist; if it already exists, treat as success.
static void carddb_ram_add(const uint8_t uid[CARD_UID_SIZE])
{
    int idx = carddb_find_in_ram(uid);
    if (idx >= 0) {
        return; // Already exists
    }

    for (int i = 0; i < CARD_DB_MAX_CARDS; i++) {
        if (!g_cards[i].in_use) {
            uid_copy(g_cards[i].uid, uid);
            g_cards[i].in_use = 1;
            return;
        }
    }
}

// Remove UID from RAM whitelist.
static void carddb_ram_del(const uint8_t uid[CARD_UID_SIZE])
{
    int idx = carddb_find_in_ram(uid);
    if (idx >= 0) {
        g_cards[idx].in_use = 0;
    }
}

// --------- Replay Flash log at boot (supports multiple blocks) ----------------
static void carddb_replay_from_flash(void)
{
    memset(g_cards, 0, sizeof(g_cards));
    g_last_seq  = 0;
    g_next_addr = 0;
    g_active_block = 0;

    char dbg[128];

    HAL_UART_Transmit(&DBG_UART,
                      (uint8_t*)"carddb_replay_from_flash BEGIN\r\n",
                      strlen("carddb_replay_from_flash BEGIN\r\n"),
                      HAL_MAX_DELAY);

    // 1) First, find which block actually has data (not all 0xFF).
    int found_block = -1;
    for (int i = 0; i < CARD_BLOCK_COUNT; i++) {
        uint32_t addr = g_blocks[i].base_addr;

        if (!flash_region_is_erased(addr, CARD_LOG_SIZE)) {
            // If the first record of this block is not all 0xFF, treat this block as having valid data.
            found_block = i;
            break;
        }
    }

    if (found_block < 0) {
        // No logs found at all; start fresh using block 0.
        g_active_block = 0;
        g_next_addr    = g_blocks[0].base_addr;

        HAL_UART_Transmit(&DBG_UART,
                          (uint8_t*)"REPLAY: no valid block, start fresh on block 0\r\n",
                          strlen("REPLAY: no valid block, start fresh on block 0\r\n"),
                          HAL_MAX_DELAY);
        return;
    }

    g_active_block = found_block;

    uint32_t addr     = g_blocks[g_active_block].base_addr;
    uint32_t end_addr = addr + g_blocks[g_active_block].size;

    uint16_t max_seq = 0;

    while (addr + CARD_LOG_SIZE <= end_addr) {
        // If this record is all 0xFF, the rest is also empty.
        if (flash_region_is_erased(addr, CARD_LOG_SIZE)) {
            int len = snprintf(dbg, sizeof(dbg),
                               "REPLAY: block=%d addr=0x%08lX ERASED, stop\r\n",
                               g_active_block, (unsigned long)addr);
            HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);
            break;
        }

        card_log_t rec;
        flash_read_log(addr, &rec);

        int len = snprintf(dbg, sizeof(dbg),
                           "REPLAY: block=%d addr=0x%08lX magic=0x%02X op=%u "
                           "uid=%02X %02X %02X %02X %02X seq=%u crc=0x%04X\r\n",
                           g_active_block,
                           (unsigned long)addr,
                           rec.magic,
                           (unsigned)rec.op,
                           rec.uid[0], rec.uid[1], rec.uid[2], rec.uid[3], rec.uid[4],
                           (unsigned)rec.seq,
                           rec.crc);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);

        // magic check
        if (rec.magic != CARD_FLASH_MAGIC) {
            HAL_UART_Transmit(&DBG_UART,
                              (uint8_t*)"REPLAY: bad magic, stop\r\n",
                              strlen("REPLAY: bad magic, stop\r\n"),
                              HAL_MAX_DELAY);
            break;
        }

        // CRC check
        uint16_t crc = card_log_crc(&rec);
        if (crc != rec.crc) {
            len = snprintf(dbg, sizeof(dbg),
                           "REPLAY: bad CRC calc=0x%04X stored=0x%04X, stop\r\n",
                           crc, rec.crc);
            HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);
            break;
        }

        // Update max seq
        if (rec.seq > max_seq) {
            max_seq = rec.seq;
        }

        // Apply operation to RAM whitelist
        if (rec.op == CARD_LOG_OP_ADD) {
            carddb_ram_add(rec.uid);
        } else if (rec.op == CARD_LOG_OP_DEL) {
            carddb_ram_del(rec.uid);
        }

        addr += CARD_LOG_SIZE;
    }

    g_last_seq  = max_seq;
    g_next_addr = addr;

    int len = snprintf(dbg, sizeof(dbg),
                       "REPLAY DONE: active_block=%d last_seq=%u, next_addr=0x%08lX\r\n",
                       g_active_block,
                       (unsigned)g_last_seq,
                       (unsigned long)g_next_addr);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);
}

// --------- Public API implementations -----------------------------------------
static void carddb_dump_flash(void);   

void carddb_init(void)
{
    carddb_replay_from_flash();

    int cnt = carddb_get_all(NULL, 0);  // Count only, don't fill array

    char dbg[96];
    int len = snprintf(dbg, sizeof(dbg),
                       "carddb_init: RAM cards=%d, active_block=%d last_seq=%u, next_addr=0x%08lX\r\n",
                       cnt,
                       g_active_block,
                       (unsigned)g_last_seq,
                       (unsigned long)g_next_addr);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);

    carddb_dump_flash();
}


// Check whether a UID is in the whitelist.
int carddb_check(const uint8_t uid[CARD_UID_SIZE])
{
    return (carddb_find_in_ram(uid) >= 0) ? 1 : 0;
}

// Get all whitelist entries (useful for debug / displaying).
int carddb_get_all(card_entry_t *out_array, int max_items)
{
    int count = 0;
    for (int i = 0; i < CARD_DB_MAX_CARDS; i++) {
        if (g_cards[i].in_use) {
            if (count < max_items) {
                out_array[count] = g_cards[i];
            }
            count++;
        }
    }
    return count; // Real whitelist count (may be > max_items)
}

// --------- Garbage collection (GC): move data and do simple wear leveling ----

// Select the next block to write (simple round-robin here; you could use erase_count for smarter wear leveling).
static int select_next_block_for_gc(void)
{
    // Simple version: just alternate between two blocks.
    return (g_active_block + 1) % CARD_BLOCK_COUNT;
}

static carddb_status_t carddb_gc(void)
{
    char dbg[128];

    HAL_UART_Transmit(&DBG_UART,
                      (uint8_t*)"GC: START\r\n",
                      strlen("GC: START\r\n"),
                      HAL_MAX_DELAY);

    // Count valid cards in RAM.
    int valid_count = 0;
    for (int i = 0; i < CARD_DB_MAX_CARDS; i++) {
        if (g_cards[i].in_use) {
            valid_count++;
        }
    }

    int len = snprintf(dbg, sizeof(dbg),
                       "GC: valid cards=%d\r\n", valid_count);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);

    // Estimate minimum space needed after GC (one ADD log per valid card).
    uint32_t needed = (uint32_t)valid_count * CARD_LOG_SIZE;

    // Select the next block as the new active block.
    int new_block = select_next_block_for_gc();
    int old_block = g_active_block;

    if (needed > g_blocks[new_block].size) {
        int len2 = snprintf(dbg, sizeof(dbg),
                            "GC: needed=%lu > block_size=%lu, FULL\r\n",
                            (unsigned long)needed,
                            (unsigned long)g_blocks[new_block].size);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len2, HAL_MAX_DELAY);
        return CARDDB_ERR_FULL;
    }

    // 2) First erase the new block.
    carddb_status_t est = flash_erase_block(new_block);
    if (est != CARDDB_OK) {
        int len2 = snprintf(dbg, sizeof(dbg),
                            "GC: flash_erase_block(new) FAIL, st=%d\r\n", (int)est);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len2, HAL_MAX_DELAY);
        return est;
    }

    // 3) For every in_use card in RAM, write an ADD log into the new block.
    uint32_t addr     = g_blocks[new_block].base_addr;
    uint16_t new_seq  = 0;

    for (int i = 0; i < CARD_DB_MAX_CARDS; i++) {
        if (!g_cards[i].in_use)
            continue;

        card_log_t rec;
        memset(&rec, 0xFF, sizeof(rec));

        rec.magic = CARD_FLASH_MAGIC;
        rec.op    = CARD_LOG_OP_ADD;
        uid_copy(rec.uid, g_cards[i].uid);
        rec.seq   = ++new_seq;           // After GC, sequence numbers are re-numbered starting from 1.
        rec.crc   = card_log_crc(&rec);

        int len2 = snprintf(dbg, sizeof(dbg),
                            "GC WRITE: block=%d addr=0x%08lX seq=%u "
                            "uid=%02X %02X %02X %02X %02X crc=0x%04X\r\n",
                            new_block,
                            (unsigned long)addr,
                            (unsigned)rec.seq,
                            rec.uid[0], rec.uid[1], rec.uid[2], rec.uid[3], rec.uid[4],
                            rec.crc);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len2, HAL_MAX_DELAY);

        carddb_status_t st = flash_write_log(addr, &rec);
        if (st != CARDDB_OK) {
            int len3 = snprintf(dbg, sizeof(dbg),
                                "GC WRITE FAIL at addr=0x%08lX st=%d\r\n",
                                (unsigned long)addr, (int)st);
            HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len3, HAL_MAX_DELAY);
            return st;
        }

        addr += CARD_LOG_SIZE;
    }

    // 4) After the new block is fully written, erase the old block to free space.
    est = flash_erase_block(old_block);
    if (est != CARDDB_OK) {
        int len2 = snprintf(dbg, sizeof(dbg),
                            "GC: flash_erase_block(old) FAIL, st=%d\r\n", (int)est);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len2, HAL_MAX_DELAY);
        // At this point, data already lives in new_block, so even if erasing old_block fails, data is still safe.
        // We still return an error, but we keep the state switched to new_block.
    }

    // 5) Update global state: new active block, valid seq, and g_next_addr.
    g_active_block = new_block;
    g_last_seq     = new_seq;
    g_next_addr    = addr;

    int len4 = snprintf(dbg, sizeof(dbg),
                        "GC: DONE, active_block=%d last_seq=%u, next_addr=0x%08lX\r\n",
                        g_active_block,
                        (unsigned)g_last_seq,
                        (unsigned long)g_next_addr);
    HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len4, HAL_MAX_DELAY);

    return CARDDB_OK;
}

static carddb_status_t carddb_append_log(uint8_t op,
                                         const uint8_t uid[CARD_UID_SIZE])
{
    uint32_t end_addr = CUR_BASE_ADDR + CUR_BLOCK_SIZE;

    // ==== If there is not enough space, run GC first (which may switch to another block). ====
    if (g_next_addr == 0) {
        // Safety: if g_next_addr is not initialized yet, set it to the start of the current active block.
        g_next_addr = CUR_BASE_ADDR;
    }

    if (g_next_addr + CARD_LOG_SIZE > end_addr) {
        char dbg[64];
        int len = snprintf(dbg, sizeof(dbg),
                           "APPEND_LOG: no space in block=%d, try GC\r\n",
                           g_active_block);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);

        carddb_status_t gcst = carddb_gc();
        if (gcst != CARDDB_OK) {
            int len2 = snprintf(dbg, sizeof(dbg),
                                "APPEND_LOG: GC FAIL, st=%d\r\n", (int)gcst);
            HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len2, HAL_MAX_DELAY);
            return gcst;
        }

        // After GC, active_block and g_next_addr are updated; check free space again.
        end_addr = CUR_BASE_ADDR + CUR_BLOCK_SIZE;
        if (g_next_addr + CARD_LOG_SIZE > end_addr) {
            HAL_UART_Transmit(&DBG_UART,
                              (uint8_t*)"APPEND_LOG: still FULL after GC\r\n",
                              strlen("APPEND_LOG: still FULL after GC\r\n"),
                              HAL_MAX_DELAY);
            return CARDDB_ERR_FULL;
        }
    }
    // =========================================================================================

    card_log_t rec;
    memset(&rec, 0xFF, sizeof(rec)); // Keep unused bytes as 0xFF.

    rec.magic = CARD_FLASH_MAGIC;
    rec.op    = op;
    uid_copy(rec.uid, uid);
    rec.seq   = ++g_last_seq;
    rec.crc   = card_log_crc(&rec);

    // Debug print before writing the record.
    {
        char dbg[128];
        int len = snprintf(dbg, sizeof(dbg),
                           "APPEND_LOG: block=%d addr=0x%08lX op=%u seq=%u "
                           "uid=%02X %02X %02X %02X %02X crc=0x%04X\r\n",
                           g_active_block,
                           (unsigned long)g_next_addr,
                           (unsigned)rec.op,
                           (unsigned)rec.seq,
                           rec.uid[0], rec.uid[1], rec.uid[2], rec.uid[3], rec.uid[4],
                           rec.crc);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);
    }

    carddb_status_t st = flash_write_log(g_next_addr, &rec);
    if (st == CARDDB_OK) {
        g_next_addr += CARD_LOG_SIZE;

        char dbg2[64];
        int len2 = snprintf(dbg2, sizeof(dbg2),
                            "APPEND_LOG OK, next_addr=0x%08lX\r\n",
                            (unsigned long)g_next_addr);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg2, len2, HAL_MAX_DELAY);
    } else {
        char dbg2[64];
        int len2 = snprintf(dbg2, sizeof(dbg2),
                            "APPEND_LOG FAIL, st=%d\r\n", (int)st);
        HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg2, len2, HAL_MAX_DELAY);
    }

    return st;
}

static void carddb_dump_flash(void)
{
    char dbg[128];
    uint32_t addr = CARD_FLASH_ADDR;
    const uint32_t end_addr = CARD_FLASH_ADDR + 4 * CARD_LOG_SIZE; // Dump first 4 records for debug

    HAL_UART_Transmit(&DBG_UART,
                      (uint8_t*)"FLASH DUMP BEGIN\r\n",
                      strlen("FLASH DUMP BEGIN\r\n"),
                      HAL_MAX_DELAY);

    while (addr + CARD_LOG_SIZE <= end_addr)
    {
        card_log_t rec;
        flash_read_log(addr, &rec);

        // If the entire record is 0xFF, treat it as empty.
        if (flash_region_is_erased(addr, CARD_LOG_SIZE)) {
            int len = snprintf(dbg, sizeof(dbg),
                               "0x%08lX: ERASED\r\n",
                               (unsigned long)addr);
            HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);
        } else {
            uint16_t crc_calc = card_log_crc(&rec);
            int len = snprintf(dbg, sizeof(dbg),
                               "0x%08lX: magic=0x%02X op=%u "
                               "uid=%02X %02X %02X %02X %02X "
                               "seq=%u crc=0x%04X calc=0x%04X\r\n",
                               (unsigned long)addr,
                               rec.magic,
                               (unsigned)rec.op,
                               rec.uid[0], rec.uid[1], rec.uid[2], rec.uid[3], rec.uid[4],
                               (unsigned)rec.seq,
                               rec.crc,
                               crc_calc);
            HAL_UART_Transmit(&DBG_UART, (uint8_t*)dbg, len, HAL_MAX_DELAY);
        }

        addr += CARD_LOG_SIZE;
    }

    HAL_UART_Transmit(&DBG_UART,
                      (uint8_t*)"FLASH DUMP END\r\n",
                      strlen("FLASH DUMP END\r\n"),
                      HAL_MAX_DELAY);
}

carddb_status_t carddb_add(const uint8_t uid[CARD_UID_SIZE])
{
    // First update RAM whitelist.
    carddb_ram_add(uid);

    // Then append an ADD log to Flash.
    return carddb_append_log(CARD_LOG_OP_ADD, uid);
}

carddb_status_t carddb_remove(const uint8_t uid[CARD_UID_SIZE])
{
    int idx = carddb_find_in_ram(uid);
    if (idx < 0) {
        return CARDDB_ERR_NOT_FOUND;
    }

    // Update RAM state first.
    g_cards[idx].in_use = 0;

    // Then append a DEL log.
    return carddb_append_log(CARD_LOG_OP_DEL, uid);
}
