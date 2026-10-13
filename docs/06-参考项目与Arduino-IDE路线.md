# 参考项目与 Arduino IDE 路线

这篇文档说明 `winwill2012/xiaozhi-ai-arduino` 可借鉴什么、不能照搬什么，以及正式工程如何组织。

## 先定项目边界

最终交付物是：

- 使用 ESP32-S3-DevKitC-1 N16R8 和面包板
- 使用录音模块、功放模块、1.54 寸 LCD
- 使用 Arduino IDE 开发
- 连接小智官方服务器完成语音对话
- 代码由你实现，我提供方案、参考和审查

你不是要做的：

- 不是把参考项目直接改成小智服务器
- 不是使用参考项目里的豆包、Coze 密钥和自建服务链路
- 不是把 PlatformIO 工程原样搬进 Arduino IDE

## 两个项目的区别

| 对比项 | winwill2012/xiaozhi-ai-arduino | 本项目 |
|---|---|---|
| 开发环境 | PlatformIO | Arduino IDE |
| 工程入口 | `platformio.ini` + `src/` | `xiaozhi.ino` + sketch 内的 `src/` |
| 服务链路 | 豆包 ASR + Coze LLM + 豆包 TTS | 小智官方 OTA + WebSocket 服务 |
| 密钥 | 使用豆包和 Coze 的密钥 | 使用小智服务，不把第三方密钥写进代码 |
| 音频协议 | 项目自己的 ASR/TTS 流程 | 小智 Opus 音频帧和 WebSocket 协议 |
| 显示 | TFT_eSPI + LVGL | 先做 ST7789 最小显示，再逐步增加界面 |
| 适合用途 | 阅读模块拆分和部分驱动思路 | 作为正式固件工程 |

参考仓库没有声明开源许可证。它可以用来阅读和理解，但不适合直接把大量代码复制进你的项目。需要借鉴时，先理解思路，再用自己的工程结构重写。

## 可以借鉴的部分

阅读参考项目时，优先看这些内容：

| 文件或目录 | 可以学习什么 |
|---|---|
| `src/RecordingManager.*` | 录音任务、I2S 接收、环形缓冲区 |
| `src/AudioPlayer.*` | 播放队列、I2S 发送、音量缩放 |
| `src/GlobalState.*` | 用状态机表示待机、聆听、思考、说话 |
| `src/Settings.*` | 使用 Preferences/NVS 保存配置 |
| `src/LvglDisplay.*` | 屏幕初始化和显示层拆分 |
| `src/ui/` | LVGL 页面和组件组织方式 |

按已实现的模块有选择地阅读。例如还没接通音频时，不必先研究 LVGL 页面管理。

## 不要照搬的部分

### 1. 第三方服务密钥

参考项目的 `data/settings.json` 里包含服务配置和密钥。不要复制、提交或公开这些内容，也不要把豆包或 Coze 密钥写进你的小智项目。

### 2. 服务端协议

参考项目自己实现了 ASR、LLM、TTS 的连接流程，和小智服务器的 OTA、WebSocket、Opus 协议不是一回事。协议部分以 `78/xiaozhi-esp32` 和你的 [05-联网与协议.md](05-联网与协议.md) 为准。

### 3. 引脚定义

参考项目的麦克风、功放和屏幕引脚与本项目不一样，不要把两个项目的引脚表混着接。

本项目使用 [硬件引脚参考.md](硬件引脚参考.md) 中的定义。真正接线前，还要先确认你手里的模块型号和丝印。

### 4. PlatformIO 配置

`platformio.ini`、`lib/` 的依赖方式和 Arduino IDE 的库管理方式不同。可以参考依赖名称，但不要指望把整个工程复制到 Arduino IDE 后直接编译。

### 5. 旧版 I2S 驱动

参考项目使用 `driver/i2s.h` 的旧接口。本项目使用 ESP32 Arduino Core 3.x 的 `driver/i2s_std.h`，代码结构会不同。

## 先确认你的模块

“录音模块”和“功放模块”只是功能名称，不能直接决定接线。先看模块丝印或商品型号。

开发板已经确认是 N16R8：16MB Flash、8MB 八线 PSRAM。Arduino IDE 中对应选择 `Flash Size = 16MB`、`PSRAM = OPI PSRAM`。

| 模块 | 本文默认假设 | 必须确认 | 如果不符合 |
|---|---|---|---|
| 录音模块 | INMP441 类 I2S 数字麦克风 | 是否有 BCLK、WS、DIN；是否明确写 INMP441/ICS-43434 等 | 模拟麦克风不能按本文 I2S 代码接线，需要重新设计输入方案 |
| 功放模块 | MAX98357A 类 I2S 数字功放 | 是否有 BCLK、LRC、DIN；是否是 MAX98357A | PAM8403 等模拟输入功放需要额外的音频输出方案 |
| LCD | ST7789 1.54 寸 SPI 屏 | 控制器、分辨率、引脚名、是否带触摸 | 240x240 和 240x320 的初始化参数不同，触摸屏还要额外接线 |

确认型号后再更新 [硬件引脚参考.md](硬件引脚参考.md)。型号不确定时先不要通电接线。

## Arduino IDE 工程怎么组织

正式工程只有一个，放在 `firmware/xiaozhi/`。Arduino IDE 2.x 会递归编译 sketch 根目录下 `src/` 中的源码，因此正式工程使用 `src/` 分层：

```text
firmware/xiaozhi/
├── xiaozhi.ino
├── data/
└── src/
    ├── app/
    │   ├── Application.h
    │   └── Application.cpp
    ├── audio/
    │   ├── Microphone.h
    │   ├── Microphone.cpp
    │   ├── Speaker.h
    │   ├── Speaker.cpp
    │   └── AudioPipeline.h
    ├── core/
    │   ├── Config.h
    │   └── Version.h
    ├── hal/
    │   ├── Button.h
    │   ├── Button.cpp
    │   ├── RgbLed.h
    │   └── RgbLed.cpp
    ├── net/
    │   ├── Network.h
    │   ├── Network.cpp
    │   ├── Protocol.h
    │   └── Protocol.cpp
    └── ui/
        ├── Display.h
        └── Display.cpp
```

Arduino IDE 的多文件规则：

- 主文件叫 `xiaozhi.ino`
- sketch 根目录叫 `xiaozhi`
- `src/` 下的 `.cpp` 会递归参与编译
- `.ino` 中引用模块时使用 `src/...` 路径
- 模块之间使用相对路径包含头文件

每个里程碑结束时，工程都要保持可编译、可上传。临时实验只用于验证硬件或库，验证后把结果集成回正式工程。详细规范见 [08-整体工程与迭代规范.md](08-整体工程与迭代规范.md)。

## 推荐的推进顺序

| 里程碑 | 交付内容 | 暂不进入 |
|---|---|---|
| 0 | Arduino IDE 能上传、串口能打印 | 音频、网络、屏幕 |
| 1 | GPIO、按键、非阻塞计时 | 类和复杂抽象 |
| 2 | 多文件工程、类封装 | 音频协议 |
| 2.5 | LCD 点亮、显示固定文本 | LVGL 复杂界面 |
| 3 | 麦克风读数和幅度打印 | 功放 |
| 4 | 功放播放正弦波 | 麦克风回环 |
| 5 | 麦克风到功放的回环 | 服务器 |
| 6 | WiFi 和 OTA 配置 | Opus |
| 7 | WebSocket 握手 | 完整对话 |
| 8 | 播放服务端音频 | 上行录音 |
| 9 | Opus 编码、录音和完整对话 | 大而全的重构 |

屏幕属于 M1，因为它会成为后续音频和网络联调的状态窗口。

## 参考项目查看顺序

1. M1 后看 `Application.*` 和 `GlobalState.*`，理解模块如何组装。
2. M2 后看 `RecordingManager.*`，对照 I2S 接收代码。
3. M2 后看 `AudioPlayer.*`，对照队列和播放任务。
4. M1 后看 `LvglDisplay.*` 和 `src/ui/`，理解显示层拆分。
5. M3 阶段只看 `78/xiaozhi-esp32` 的 `main/protocols/` 和 `main/ota.cc`。

## 你现在最需要建立的三个习惯

1. 每次只引入一个新变量：新模块、新库、新协议不要同时上。
2. 每个模块都能独立复现：先证明硬件正常，再接入上一层。
3. 代码先跑通，再解释，再修改。
4. 正式工程持续增长，不复制成多个并行项目。

有了 C/C++ 和算法基础，你不需要从语法重新学起。真正要补的是硬件时序、外设驱动和实时系统的思维方式。

## 相关链接

- [参考项目：winwill2012/xiaozhi-ai-arduino](https://github.com/winwill2012/xiaozhi-ai-arduino)
- [小智官方固件：78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
- [小智官网](https://xiaozhi.me)
