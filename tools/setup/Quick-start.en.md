# Muse Charm v0.8.1 setup package

For Waveshare ESP32-S3-RLCD-4.2 only: 16MB Flash / 8MB PSRAM.
You need desktop Chrome/Edge, a data USB cable, a Muse account/App, your own
mgst_ SDK token, and 2.4GHz Wi-Fi. A Qwen Token Plan or MiniMax speech API key
is optional. No personal credentials are included.

## 1. Flash firmware — no compilation or ESP-IDF

Install Python 3, then:

```sh
python -m pip install esptool
python -m serial.tools.list_ports
```

Use `python3` instead of `python` on macOS/Linux if needed. Close serial
monitors and disconnect the setup page before flashing. Replace PORT with
COM3/COM4 on Windows, /dev/cu.usbmodem… on macOS, or /dev/ttyACM0 on Linux.
Hold BOOT, tap RESET, then release BOOT to enter download mode.

For a new board or an unknown partition layout, back up existing settings and
flash the factory image from the extracted package directory:

```sh
python -m esptool --chip esp32s3 --port PORT --no-stub write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 firmware/muse-charm-v0.8.1-rlcd42-factory.bin
```

For an existing Muse Charm with a matching partition layout, app-only update
preserves Wi-Fi, pairing and credentials:

```sh
python -m esptool --chip esp32s3 --port PORT --no-stub write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x20000 firmware/muse-charm-v0.8.1-rlcd42-app.bin
```

Do not run erase-flash or burn eFuses. The factory image replaces bootloader,
partition and OTA initialization regions. It does not contain or erase NVS;
old settings can remain when switching from other projects. Tap RESET after
flashing to boot normally.

## 2. Open the setup page

Visit https://eazylee.xyz/muse-charm/ in desktop Chrome/Edge. Use EN/中文 in
the header to switch languages. You can also open `setup/index.html` offline.
If local-file serial permissions are unavailable, run in the extracted folder:

```sh
python -m http.server 8765 --bind 127.0.0.1
```

Then open http://localhost:8765/setup/ . Stop the server with Ctrl+C when done.
Safari/Firefox are not supported. Credentials go directly to the board over
USB; the page has no external scripts or credential upload requests.

## 3. Enter your credentials

Connect the ESP32 USB Serial/JTAG port. Get your SDK token from
 gadgets.muse.ai → Account → SDK tokens. Select optional Qwen Token Plan,
MiniMax China or MiniMax Global and enter that provider’s speech API key.
MiniMax lets you choose model and Voice ID; defaults are speech-2.8-turbo and
male-qn-qingse. Select the region matching your account/key and confirm speech
API access and credits. MiniMax contract tests pass; paid synthesis has not
been verified without a real authorized key. Qwen speech is hardware-tested.

Save & restart. Tokens persist in NVS; runtime SDK tokens override build-time
ones. Blank fields preserve existing settings. The page only reports configured
status and never reads back saved keys. Format checks do not verify permissions.
After changing account tokens, restart and re-add the device in the Muse App.
NVS is unencrypted; do not publish device Flash/NVS dumps.

## 4. Pair and play

Add MuseGadget-Disp-XXXXXX in the Muse App, tap BOOT when prompted and choose
2.4GHz Wi-Fi. Hold KEY to talk and release to send. BOOT toggles theme;
double-tap rotates. Holding BOOT for 5 seconds resets pairing, while device-level
SDK/TTS credentials remain stored. Try “switch to light mode and wave” or
“set the timezone to Shanghai”.

Network music requires direct playable MP3 URLs. Battery percentage is a
voltage estimate. Q36 gamepad compatibility remains unverified.

Full guide: https://github.com/EazyLee30/muse-charm/blob/main/README.md
