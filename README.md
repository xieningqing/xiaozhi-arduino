# 小智 Arduino 实现项目

在 ESP32-S3 面包板上实现一个连接小智官方服务、完成真实语音对话的固件。
支持**按键手动对讲**和**双击进入的全双工实时对话**两种模式。

## 功能

- 首次上电自动进入 WiFi 配网热点，手机填表即可完成联网
- OTA 拉取小智服务配置，优先使用 MQTT + UDP 通道
- 麦克风采集 16 kHz PCM，Opus 编码后按小智协议分帧上传
- 接收服务端 24 kHz Opus 音频，解码后经 I2S 播放
- 手动模式：单击开始录音，再单击结束并等待回复
- 全双工模式：双击进入，上下行同时进行，再双击退出
- ST7789 240x240 状态面板：连接状态、模式、状态文字、麦克风电平、滚动消息
- 断线、超时、会话失效自动恢复到待机

## 硬件

| 模块 | 型号 | 说明 |
|---|---|---|
| 开发板 | ESP32-S3-DevKitC-1 N16R8 | 16 MB Flash，8 MB OPI PSRAM |
| 屏幕 | 1.54 寸 ST7789，240x240，SPI | TFT_eSPI 驱动 |
| 麦克风 | INMP441 类 I2S 数字麦克风 | 16 kHz 单声道 |
| 功放 | MAX98357A 类 I2S 数字功放 + 喇叭 | 24 kHz 单声道 |

完整引脚表、采样率和接线注意事项见 [硬件引脚参考.md](docs/硬件引脚参考.md)。

## 按键操作

设备只有一个 BOOT 按键（GPIO0），用单击、双击、长按三种手势区分功能。

| 操作 | 行为 |
|---|---|
| 单击（待机） | 进入手动聆听，开始录音 |
| 单击（录音中） | 停止录音，等待服务端识别和回复 |
| 单击（播放中） | 打断当前播报，直接开始下一轮录音 |
| 双击（待机） | 进入全双工实时模式 |
| 双击（实时中） | 退出实时模式，回到待机 |
| 长按 3 秒 | 保存配网标记并重启，进入 WiFi 配网模式 |

双击判定窗口为 450 ms，配置项是 `config::kDoubleClickWindowMs`。
单击在待机和实时模式下会延迟一个双击窗口再执行，避免把双击拆成两次单击。

## 配网

不需要改代码，也不需要重新烧录。

1. 设备在以下任一情况自动进入配网：没有已保存的 WiFi、已保存的 WiFi 连不上。
2. 也可以随时长按 BOOT 3 秒强制进入配网。
3. 设备创建热点 `Xiaozhi-XXXX`，`XXXX` 是 MAC 后四位。
4. 手机连接该热点，通常会自动弹出配置页；没有弹出就打开 `http://192.168.4.1/`。
5. 填入 WiFi 名称和密码，保存后设备自动重启并连接。

WiFi 凭据保存在 NVS（命名空间 `wifi`），优先于 `Secrets.h` 里的编译期默认值。
`firmware/xiaozhi/src/core/Secrets.h` 只作为开发期兜底，正式使用可以不填。

## 语音模式

### 手动模式

半双工：录音时不上行播放，播放时不录音。
适合按键对讲，行为可预测，延迟低。

服务端控制消息使用 `{"type":"listen","state":"start","mode":"manual"}`。

### 全双工实时模式

使用服务端 `mode: "realtime"`，麦克风持续上行，服务端音频同时下行播放。

- 采集和 Opus 编码运行在独立的 `opus_upload` 任务，不阻塞网络主循环
- 上行时间戳取自下行播放时间戳队列，对齐服务端 AEC
- `hello` 中声明 `features.aec = true`，由服务端做回声消除

**现实限制**：设备端没有本地 AEC。喇叭音量过大或麦克风离喇叭太近时，
仍可能产生回声自激。优先通过物理布局（拉开麦克风与喇叭距离、降低音量）解决。

## 开发环境

| 项目 | 值 |
|---|---|
| 开发环境 | Arduino IDE 2.x（或 arduino-cli） |
| 核心 | esp32:esp32 3.3.12 |
| FQBN | `esp32:esp32:esp32s3` |
| 配置 | `FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB` |

### 使用 arduino-cli 编译

```powershell
arduino-cli compile `
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB" `
  --build-path ".arduino-build" `
  "firmware/xiaozhi"
```

### 使用 VS Code 任务

`.vscode/arduino.json` 已配置好：

- 编译前自动运行 `scripts/sync-libraries.ps1`，把仓库 `libraries/` 里的库
  通过 Junction 链接到 Arduino 库目录
- 编译后自动运行 `scripts/gen-intellisense.ps1`，生成 IntelliSense 编译数据库
- 构建输出到 `.arduino-build/`

## 依赖库

第三方库随仓库一起提交在 `libraries/`，不依赖库管理器在线安装：

| 库 | 版本 | 用途 |
|---|---|---|
| TFT_eSPI | 2.5.43 | ST7789 屏幕驱动 |
| ArduinoJson | 7.4.3 | OTA 与控制消息 JSON 解析 |
| ArduinoWebsockets | 0.5.4 | WebSocket 通道（保留，当前用 MQTT） |
| esp32_opus | 1.0.3 | Opus 编解码 |

## 工程结构

```
firmware/xiaozhi/
├── xiaozhi.ino                 入口，设置 loopTask 栈为 48KB
└── src/
    ├── app/Application.*       状态机、按键手势、音频任务、会话管理
    ├── audio/
    │   ├── Microphone.*        I2S 麦克风，带超时读取
    │   ├── Speaker.*           I2S 功放输出
    │   ├── OpusAudioEncoder.*  16 kHz Opus 编码
    │   └── OpusAudioDecoder.*  24 kHz Opus 解码
    ├── core/
    │   ├── Config.h            引脚、超时、队列长度等常量
    │   ├── Secrets.h           开发期 WiFi 兜底（可留空）
    │   └── Version.h           固件名与版本
    ├── hal/Button.*            消抖按键
    ├── net/
    │   ├── Network.*           WiFi 配网、NTP、OTA 配置获取
    │   ├── MqttProtocol.*      MQTT 控制通道 + UDP 音频通道
    │   └── Protocol.*          WebSocket 通道（保留）
    └── ui/Display.*            240x240 状态面板
```

## 通信协议

OTA 返回配置后优先走 MQTT，其次才是 WebSocket。

| 项目 | 值 |
|---|---|
| 控制通道 | MQTT over TLS，端口 8883 |
| 音频通道 | UDP + AES-128-CTR |
| 上行音频 | Opus，16 kHz，单声道，60 ms（960 采样点） |
| 下行音频 | Opus，24 kHz，单声道，60 ms（1440 采样点） |
| UDP 包头 | `type 1B / flags 1B / payload_len 2B / ssrc 4B / timestamp 4B / sequence 4B` |
| 加密 | AES-CTR，nonce 就是 16 字节 UDP 包头 |
| 通道超时 | 120 秒无服务端数据判为失效 |
| MQTT 重连 | 仅在待机状态，间隔 60 秒 |

协议细节以 `78/xiaozhi-esp32` 为准，实现说明见
[05-联网与协议.md](docs/05-联网与协议.md)。

## 屏幕界面

240x240 分区布局，只重绘变化区域，避免音频播放时闪烁：

```
y   0.. 28   标题栏 + WiFi / 服务连接指示
y  32.. 50   模式行（IDLE / MANUAL / REALTIME）
y  56..120   大号状态文字（按状态着色）
y 128..152   麦克风电平条（绿 / 黄 / 红）
y 158..214   消息区（固定两行）
y 216..238   操作提示
```

消息区滚动规则：文本按像素宽度折行，先显示前两行；超过两行时每 1.8 秒
向上滚动一行，第三行进入底部、顶掉第一行；滚到最后两行停止，右下角显示
`当前行/总行数`。

当前未加载中文字库，非 ASCII 文本会加 `[zh]` 前缀标记，避免显示为乱码方块。

## 配置与调参

主要可调项都在 `firmware/xiaozhi/src/core/Config.h`：

| 常量 | 默认值 | 说明 |
|---|---|---|
| `kDoubleClickWindowMs` | 450 | 双击判定窗口 |
| `kWifiConfigLongPressMs` | 3000 | 长按进入配网的时间 |
| `kAudioChannelTimeoutMs` | 120000 | 通道无数据超时 |
| `kMqttReconnectIntervalMs` | 60000 | MQTT 重连间隔 |
| `kPlaybackQueueLength` | 8 | 下行播放队列长度 |
| `kUploadQueueLength` | 8 | 上行音频队列长度 |
| `kMicReadTimeoutMs` | 100 | 麦克风读取超时 |

消息滚动间隔在 `firmware/xiaozhi/src/ui/Display.cpp` 的
`kMessageScrollIntervalMs`，默认 1800 ms。

## 文档

| 文件 | 内容 |
|---|---|
| [00-学习路线图.md](docs/00-学习路线图.md) | 项目交付路线、集成里程碑和验收标准 |
| [01-环境搭建.md](docs/01-环境搭建.md) | Arduino IDE 配置、板子参数、烧录 |
| [02-GPIO入门.md](docs/02-GPIO入门.md) | GPIO、按键、非阻塞计时 |
| [03-代码组织.md](docs/03-代码组织.md) | 多文件工程、头文件、类封装 |
| [04-I2S音频入门.md](docs/04-I2S音频入门.md) | 麦克风采集、功放播放、音频回环 |
| [05-联网与协议.md](docs/05-联网与协议.md) | WiFi、OTA、MQTT/UDP 协议说明 |
| [06-参考项目与Arduino-IDE路线.md](docs/06-参考项目与Arduino-IDE路线.md) | 参考项目边界与工程组织 |
| [07-LCD-ST7789入门.md](docs/07-LCD-ST7789入门.md) | 屏幕接线、驱动和显示 |
| [08-整体工程与迭代规范.md](docs/08-整体工程与迭代规范.md) | 迭代、集成和回退规范 |
| [硬件引脚参考.md](docs/硬件引脚参考.md) | 引脚、采样率、接线表 |
| [避坑清单.md](docs/避坑清单.md) | 常见问题和排查方法 |

## 版本控制

- 仓库自带第三方库（`libraries/`），不依赖库管理器联网安装
- `.gitignore` 排除构建产物（`.arduino-build/`、`build/`、`logs/`）和编译中间文件
- `.gitattributes` 统一行尾：仓库内 LF，PowerShell 脚本 CRLF

## 已知限制

- 双核无本地 AEC，全双工依赖服务端回声消除，物理布局不当会自激
- 上行音频编码在独立任务，但网络发送仍在主循环，极端拥塞时会丢帧
- 显示层没有中文字库，中文识别文本不直接显示
- MCP 工具调用未实现，`hello` 中 `features.mcp` 声明为 `false`
- 仅支持 MQTT + UDP 主链路；WebSocket 通道代码保留但未启用
