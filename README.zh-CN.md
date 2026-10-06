<div align="center">

![Muse Charm Banner](docs/banner.png)

# 🖤 Muse Charm

### 微雪 ESP32-S3-RLCD-4.2 的 Muse 语音小挂件

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.1-red.svg)](https://github.com/espressif/esp-idf)
[![Board](https://img.shields.io/badge/Board-ESP32--S3--RLCD--4.2-green.svg)](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm)
[![Display](https://img.shields.io/badge/Display-ST7305%20RLCD-black.svg)](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm)

[English](README.md)

通过反射式单色 LCD 和 Muse 对话。无背光、无炫光——像电子纸一样安静。

</div>

---

## ✨ 特性

- 🎙️ **按住说话** — 按住 KEY 说话，松开。官方会话转写语音，并通过 USB 返回文字回复。
- 🖥️ **ST7305 反射屏** — 300×400 单色，阳光下可读，超低功耗。
- 🐾 **动态 Muse 形象** — 复用官方像素渲染器，以 5 FPS 显示待机、聆听、思考和回复动画。
- 🌗 **深色 / 浅色模式** — 纯黑背景或纸白，为反射屏而设计。
- 🔊 **ES8311 + ES7210** — I²S 扬声器 DAC 和双麦克风阵列。
- 🔋 **低功耗** — 反射屏几乎不耗电，没有背光拖累电池。
- 🔐 **官方 Muse SDK** — 通过官方 gadget SDK 的 Noise 协议加密语音链路。

## 🛠️ 硬件

| 部件 | 规格 |
|------|------|
| **开发板** | 微雪 ESP32-S3-RLCD-4.2 |
| **主控** | ESP32-S3-WROOM-1-N16R8（16MB Flash，8MB Octal PSRAM）|
| **屏幕** | ST7305 4.2寸反射式 LCD，300×400 单色 |
| **音频 DAC** | ES8311（I²C `0x18`）|
| **麦克风** | ES7210 双麦（I²C `0x40`/`0x42`）|
| **按键** | BOOT（GPIO0），KEY（GPIO18）|
| **Flash 模式** | **DIO** ⚠️（这块板的 v0.2 芯片用 QIO 80MHz 无法启动）|

### 引脚

| 信号 | GPIO |
|------|------|
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

## 🚀 快速上手

### 准备

- [ESP-IDF v6.0.1](https://github.com/espressif/esp-idf/releases/tag/v6.0.1)
- [Muse SDK token](https://gadgets.muse.ai)（Account → SDK tokens，以 `mgst_` 开头）

### 编译

```bash
# 设置 ESP-IDF 环境
. $IDF_PATH/export.sh

# 首次编译：生成本机 OTA 签名密钥（不要提交或分享）
espsecure generate-signing-key --version 2 dev_signing_key.pem

# 填入你的 SDK token（千万别提交到 git！）
tools/board.sh waveshare-s3-rlcd42 menuconfig
# → Gadget SDK Token → 粘贴你的 mgst_ token

# 编译
tools/board.sh waveshare-s3-rlcd42 build
```

### 刷机

> ⚠️ **必须用 DIO flash 模式。** 这块板的芯片版本用 QIO 会卡在启动循环。

```bash
# 找串口
ls /dev/cu.usbmodem*   # macOS
ls /dev/ttyACM*        # Linux

# 把合并好的 factory 镜像刷到 0x0
python -m esptool --chip esp32s3 -p <串口> -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio 0x0 muse-voice-rlcd42-factory.bin
```

或自己合并 factory 镜像：

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

## 📱 配对

1. 上电——屏幕显示配对状态。
2. 打开 **Muse App** → 设置 → 设备 → 开发者模式 → 添加设备。
3. 从列表选 `MuseGadget-Disp-XXXXXX`。
4. 提示时**短按 BOOT 键**确认。
5. 选择 2.4GHz Wi-Fi（ESP32-S3 只支持 2.4G）。
6. 配网后设备自动重启。**按住 KEY** 说话；USB 聊天接口提供文字回复。

> 💡 **恢复出厂：** 按住 BOOT 5 秒清除 Wi-Fi 和配对信息。

## 💻 与电脑 Muse 交流

手机配网成功后，设备自动重启，释放蓝牙内存，再连接配对账号的 Muse VM。使用同一个账号和 VM，可以在电脑 Muse 中继续交流。

```bash
python3 tools/muse/chat.py --port /dev/cu.usbmodem1101 --status
python3 tools/muse/chat.py --port /dev/cu.usbmodem1101 "你好，请用中文回复"
```

USB 文字回复使用官方 SDK 的串口协议。上游会话目前用静音推进文字回复；语音播报需要另接 TTS。反射屏显示动态形象和状态；目前不显示完整聊天文字。不要把含个人 SDK 凭证的构建配置或固件上传到公开仓库。

## 🔊 音频诊断

在 115200 波特率的 USB 串口终端发送 `>audio.test` 并回车，可通过正常播放器、ES8311 和功放播放一秒 440/660 Hz 双音提示音，不依赖 TTS 服务。`@audio {"queued":false}` 表示音频硬件尚未就绪或正忙。KEY 录音完成后，日志会记录采样数量、峰值和 RMS 音量；扬声器 I²S 写入失败会明确报错。DAC 音量按 ES8311 的 0.5 dB 刻度转换，100% 限制在 0 dB。

## 🧠 工作原理

```
┌─────────┐   BLE    ┌──────────┐   Noise    ┌─────────┐
│  手机   │◄────────►│ ESP32-S3 │◄──────────►│ Muse VM │
│ (Muse   │  GATT    │ (小挂件) │  WebSocket │ (语音   │
│  App)   │          │          │  + TLS     │  AI)    │
└─────────┘          └──────────┘            └─────────┘
```

1. **配对** — 手机通过 BLE 连接，物理按键确认。
2. **配网** — Wi-Fi 密码通过加密 BLE 传输。
3. **语音链路** — 设备与 Muse VM 建立 Noise 加密 WebSocket。
4. **按住说话** — 按下 KEY → 录音 → 松开 → 转写 → Muse 返回文字回复。

## 📁 项目结构

```
├── main/
│   ├── rlcd42_status.c          # ST7305 反射屏驱动 + 状态界面
│   ├── voice_board_waveshare_s3_rlcd42.c  # ES8311/ES7210 音频板驱动
│   ├── app.c                    # 主应用逻辑
│   ├── ble_server.c             # BLE GATT 配对服务
│   └── Kconfig.projbuild        # 板级配置（含 SDK token）
├── devices/
│   └── sdkconfig.waveshare-s3-rlcd42  # 板级构建配置（DIO flash！）
├── tools/
│   └── board.sh                 # 板子选择脚本
└── components/                  # SDK 公共组件
```

## ⚠️ 已知硬件坑

- **QIO flash = 启动循环。** 这块板的 ESP32-S3 v0.2 芯片用 QIO 80MHz 无法启动（`rst:0x7 TG0WDT_SYS_RST`）。编译和刷机都要用 DIO。
- **ES7210 I²C 地址**因批次而异：`0x40` 或 `0x42`。驱动会两个都探测。

## 📄 许可证

Apache License 2.0 — 见 [LICENSE](LICENSE)。

基于 [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk)（Apache-2.0）。
官方 Jollybot 角色资源**未**包含（不在 Apache-2.0 授权范围内）。

---

<div align="center">
为反射屏而作 🖤
</div>
