# CSIght — ESP32 Flash Instructions

## ⚠️ Read before flashing

- **Never** interrupt power during flashing
- **Never** modify bootloader partitions manually
- If something goes wrong the ESP32 can almost always be recovered — see Recovery below

---

## Step 1 — Find your chip

| Your board | Chip | Firmware folder |
|------------|------|-----------------|
| FZ WiFi Dev Board, FlipMods, FEBERIS, Marauder, any ESP32-WROOM-32 | ESP32 | `esp32/esp32/` |
| ESP32-S3 DevKit, XIAO-S3, TinyS3, FeatherS3 | ESP32-S3 | `esp32/esp32s3/` |
| ESP32-C3 Mini, XIAO-C3, LOLIN C3 | ESP32-C3 | `esp32/esp32c3/` |
| ESP32-C6 DevKit, XIAO-C6 | ESP32-C6 | `esp32/esp32c6/` |

---

## Step 2 — Install esptool

```bash
pip install esptool
```

---

## Step 3 — Flash

Replace `FOLDER` with your chip folder from the table above (e.g. `esp32/esp32/`).
Replace `PORT` with your serial port:
- **macOS**: `/dev/cu.usbserial-XXXX` (find with `ls /dev/cu.*`)
- **Linux**: `/dev/ttyUSB0` or `/dev/ttyACM0`
- **Windows**: `COM3` (check Device Manager)

```bash
cd FOLDER

esptool.py --chip auto --port PORT --baud 460800 \
  write_flash \
  0x1000  bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 csight_esp32.bin
```

Or using the included flash_args file:

```bash
esptool.py --chip auto --port PORT write_flash @flash_args
```

Press **EN/RST** on the board after flashing to boot.

---

## Step 4 — Wire to Flipper Zero

| ESP32 pin | Flipper GPIO | Notes |
|-----------|-------------|-------|
| TX (default 17) | Pin 14 | Configurable in app |
| RX (default 16) | Pin 13 | Configurable in app |
| GND | GND | Required |
| 3.3V or 5V | 3.3V or 5V | Power the board BEFORE launching app |

> ⚠️ **Power the ESP32 board BEFORE launching CSIght on the Flipper.** Launching the app without the board powered will cause the Flipper to crash. CSIght will warn you about this on first launch.

---

## Step 5 — Install the FAP

Copy the `.fap` matching your Flipper firmware from the `flipper/` folder:

```
SD Card/apps/GPIO/csight.fap
```

Launch from **Apps → GPIO → CSIght**.

---

## Recovery

If flashing fails or the board seems bricked:

```bash
# Erase flash completely
esptool.py --chip auto --port PORT erase_flash

# Then re-flash from scratch
```

This resets the board to factory state. The bootloader is almost never truly bricked.

---

## 3-in-1 board users (CC1101 + ESP32 + nRF24)

Your board uses shared GPIO. Default pins 16/17 should be free for UART but if you experience issues use the Custom pin option in CSIght to change them. Your SPI toggle switch position does not affect CSIght — it only needs UART.
