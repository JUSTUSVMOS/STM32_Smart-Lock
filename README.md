
# STM32 Smart Lock System (RFID + Keypad + Bluetooth + FreeRTOS)

This project implements a full-featured **smart lock system** on an **STM32F407** MCU.  
It integrates **RFID (MFRC522), Bluetooth (HC-05), Keypad input, LCD1602 I2C display, and internal Flash whitelist storage**, all running under **FreeRTOS**.

---

## 🔧 Features

### ✔ Multi-authentication
- **RFID** (MFRC522)
- **4-digit PIN via keypad**
- **4-digit PIN via Bluetooth (HC-05)**

### ✔ Flash-based Whitelist Database
- Internal Flash logging system  
- Wear-leveling + block rotation  
- Add/Delete UID  
- CRC16 for data integrity

### ✔ FreeRTOS Task Architecture
- `vBtTask` — Bluetooth PIN input (UART2 DMA RX)
- `vKeypadTask` — Scan 4x4 keypad and generate events
- `vNfcTask` — RFID scanning + whitelist check
- `vLcdTask` — LCD1602 I2C UI output
- `vStateTask` — Global lock/unlock state manager

### ✔ Hardware
- MFRC522 SPI mode
- LCD1602 via PCF8574 (I2C)
- HC-05 Bluetooth UART
- On-board LEDs for LOCK/UNLOCK indication

---

## 📊 System Architecture

*(Insert image link in GitHub:  
`![System Architecture](images/system_arch.png)`)*

---

## 📁 Project Structure

```
/Core
/Drivers
/FreeRTOS
/rc522
/card_db
/lcd1602_i2c
main.c
```

**Folder meaning:**
- `/Core` — CubeMX generated code  
- `/Drivers` — STM32 HAL libraries  
- `/FreeRTOS` — RTOS kernel & port layer  
- `/rc522` — MFRC522 RFID driver  
- `/card_db` — Flash-based whitelist and wear-leveling  
- `/lcd1602_i2c` — PCF8574 LCD driver  

---

## ⚙️ Build & Flash

1. Open with **STM32CubeIDE**
2. Build the project (`Ctrl + B`)
3. Connect ST-Link
4. Flash using **Run → Debug** or **Run → Run**

---

## 🧠 Task Overview (FreeRTOS)

### 🔵 **BT Task**
- Reads UART2 DMA RX (xBtRxQ)
- Builds PIN  
- Sends event `'1'` (unlock) or `'0'` (lock)

### 🔵 **Keypad Task**
- Scans 4×4 keypad  
- A = Add Card Mode  
- B = Delete Card Mode  
- C = Lock  
- Sends events or LCD messages

### 🔵 **RFIC Task (NFC Task)**
- Detects card  
- Reads UID  
- Checks whitelist via Flash DB  
- Performs Add/Delete in Flash

### 🔵 **State Task**
- Central event handler  
- Applies lock/unlock  
- Updates LEDs  
- Pushes messages to LCD queue

### 🔵 **LCD Task**
- Receives `LcdMsg_t` from xLcdQ  
- Updates UI lines

---

## 🗄 Flash Database (card_db)

- Append-only log in Flash  
- Record includes:
  - magic
  - op (ADD/DEL)
  - UID (5 bytes)
  - seq
  - CRC16
- Automatic GC when block is full  
- Keeps wear-leveling by rotating between **two Flash blocks**

---

## 📌 Example Output (Debug UART)

```
RC522 Ver=0xC4, TxControl=0x00
REPLAY DONE: active_block=1 last_seq=42
NFC: UID=43 0D D1 13 8C
NFC UNLOCK
```

