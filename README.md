<div align="center">

![Muse Charm Banner](docs/banner.png)

# 🖤 Muse Charm

### A Muse voice gadget for the Waveshare ESP32-S3-RLCD-4.2

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.1-red.svg)](https://github.com/espressif/esp-idf)
[![Board](https://img.shields.io/badge/Board-ESP32--S3--RLCD--4.2-green.svg)](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm)
[![Display](https://img.shields.io/badge/Display-ST7305%20RLCD-black.svg)](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm)

[中文文档](README.zh-CN.md)

Talk to Muse through a reflective monochrome LCD. No backlight, no glare — just e-paper-like calm.

</div>

---

## ✨ Features

- 🎙️ **Push-to-talk voice** — Hold KEY, speak, release. The official session transcribes and returns text replies over USB.
- 🖥️ **ST7305 reflective LCD** — 300×400 monochrome, sunlight-readable, ultra-low power.
- 🐾 **Animated Muse avatar** — Official pixel renderer at 5 FPS, with idle, listening, thinking and reply animations.
- 🌗 **Dark / Light modes** — Pure black background or paper white, designed for reflective displays.
- 🔊 **ES8311 + ES7210** — Speaker DAC and dual-microphone array via I²S.
- 🔋 **Low power** — Reflective display sips power; no backlight to drain the battery.
- 🔐 **Official Muse SDK** — Noise-protocol encrypted voice link via the official gadget SDK.

## 🛠️ Hardware

| Component | Spec |
|-----------|------|
| **Board** | Waveshare ESP32-S3-RLCD-4.2 |
| **SoC** | ESP32-S3-WROOM-1-N16R8 (16MB Flash, 8MB Octal PSRAM) |
| **Display** | ST7305 4.2" reflective LCD, 300×400 mono |
| **Audio DAC** | ES8311 (I²C `0x18`) |
| **Microphone** | ES7210 dual-mic (I²C `0x40`/`0x42`) |
| **Keys** | BOOT (GPIO0), KEY (GPIO18) |
| **Flash mode** | **DIO** ⚠️ (QIO 80MHz fails to boot on this board's v0.2 chip revision) |

### Pinout

| Signal | GPIO |
|--------|------|
| LCD DC | 5 |
| LCD SCLK | 11 |
| LCD MOSI | 12 |
| LCD CS | 40 |
| LCD RST | 41 |
| I²S MCLK | 16 |
| I²S BCLK | 9 |
| I²S LRCK | 45 |
| I²S DOUT | 8 |
| I²S DIN | 10 |
| PA_CTRL | 46 |
| KEY | 18 |
| BOOT | 0 |

## 🚀 Quick Start

### Prerequisites

- [ESP-IDF v6.0.1](https://github.com/espressif/esp-idf/releases/tag/v6.0.1)
- A [Muse SDK token](https://gadgets.muse.ai) (Account → SDK tokens, starts with `mgst_`)

### Build

```bash
# Set up ESP-IDF
. $IDF_PATH/export.sh

# First build: generate a local OTA signing key (never commit or share it)
espsecure generate-signing-key --version 2 dev_signing_key.pem

# Set your SDK token (NEVER commit this!)
tools/board.sh waveshare-s3-rlcd42 menuconfig
# → Gadget SDK Token → paste your mgst_ token

# Build
tools/board.sh waveshare-s3-rlcd42 build
```

### Flash

> ⚠️ **Must use DIO flash mode.** QIO bricks the boot on this board's chip revision.

```bash
# Find your serial port
ls /dev/cu.usbmodem*   # macOS
ls /dev/ttyACM*        # Linux

# Flash the merged factory image at 0x0
python -m esptool --chip esp32s3 -p <PORT> -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio 0x0 muse-voice-rlcd42-factory.bin
```

Or build the factory image yourself:

```bash
cd build-waveshare-s3-rlcd42
python -m esptool --chip esp32s3 merge-bin \
  --flash-mode dio --flash-size 16MB \
  -o muse-voice-rlcd42-factory.bin \
  0x0 bootloader/bootloader.bin \
  0x10000 partition_table/partition-table.bin \
  0x17000 ota_data_initial.bin \
  0x19000 phy_init_data.bin \
  0x20000 muse-gadget.bin
```

## 📱 Pairing

1. Power on — the screen shows the pairing status.
2. Open **Muse App** → Settings → Devices → Developer mode → Add Device.
3. Select `MuseGadget-Disp-XXXXXX` from the list.
4. When prompted, **short-press the BOOT button** to confirm.
5. Choose your 2.4GHz Wi-Fi network (ESP32-S3 is 2.4GHz only).
6. The gadget restarts after pairing. **Hold KEY** to talk; USB chat provides text replies.

> 💡 **Factory reset:** Hold BOOT for 5 seconds to wipe Wi-Fi and pairing data.

## 💻 Talk to your computer's Muse

After phone provisioning succeeds, the gadget restarts to release BLE memory and connects to the paired account's Muse VM. Use the same account and VM to continue the conversation in desktop Muse.

```bash
python3 tools/muse/chat.py --port /dev/cu.usbmodem1101 --status
python3 tools/muse/chat.py --port /dev/cu.usbmodem1101 "Hello, please reply briefly"
```

USB text replies use the official SDK console protocol. The upstream session currently paces text with silence; spoken replies require a separate TTS integration. The reflective display shows the animated avatar and status, rather than full chat text. Keep personal SDK credentials and built firmware out of public repositories.

## 🔊 Audio diagnostics

On the USB serial console at 115200 baud, send `>audio.test` followed by Enter to play one second of 440/660 Hz tones through the normal player, ES8311 and amplifier. This tests playback without a TTS service at a fixed 60% level and then restores the saved volume. `@audio {"queued":false}` means the voice hardware is not ready or is busy. After a KEY recording, logs report microphone sample count, peak and RMS amplitude. Speaker I²S failures are logged explicitly. The DAC volume uses the ES8311's 0.5 dB scale, with 100% capped at unity gain.

## 🧠 How It Works

```
┌─────────┐   BLE    ┌──────────┐   Noise    ┌─────────┐
│  Phone  │◄────────►│ ESP32-S3 │◄──────────►│ Muse VM │
│ (Muse   │  GATT    │ (gadget) │  WebSocket │ (voice  │
│  App)   │          │          │  + TLS     │  AI)    │
└─────────┘          └──────────┘            └─────────┘
```

1. **Pairing** — Phone connects via BLE, confirms with physical button press.
2. **Provisioning** — Wi-Fi credentials sent over encrypted BLE.
3. **Voice link** — Device opens a Noise-encrypted WebSocket to Muse's VM.
4. **Push-to-talk** — KEY down → record → KEY up → transcribe → Muse returns a text reply.

## 📁 Project Structure

```
├── main/
│   ├── rlcd42_status.c          # ST7305 reflective LCD driver + status UI
│   ├── voice_board_waveshare_s3_rlcd42.c  # ES8311/ES7210 audio board driver
│   ├── app.c                    # Main application logic
│   ├── ble_server.c             # BLE GATT pairing server
│   └── Kconfig.projbuild        # Board-specific config (incl. SDK token)
├── devices/
│   └── sdkconfig.waveshare-s3-rlcd42  # Board build configuration (DIO flash!)
├── tools/
│   └── board.sh                 # Board selection helper
└── components/                  # Shared SDK components
```

## ⚠️ Known Hardware Quirks

- **QIO flash = boot loop.** This board's ESP32-S3 v0.2 chip revision cannot boot with QIO 80MHz flash (`rst:0x7 TG0WDT_SYS_RST`). Always use DIO mode for both build and flash.
- **ES7210 I²C address** varies by batch: `0x40` or `0x42`. The driver probes both.

## 📄 License

Apache License 2.0 — see [LICENSE](LICENSE).

Based on the [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk) (Apache-2.0).
Original Jollybot character assets are **not** included (not covered by Apache-2.0).

---

<div align="center">
Made with 🖤 for reflective displays
</div>
