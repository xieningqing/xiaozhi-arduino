# M1：GPIO 基础

目标：用按键控制灯，并且全程不用 `delay()`。

## 先理解 GPIO

GPIO 就是芯片上可以读电压、也可以输出电压的引脚。

- 输出模式：你写 1，引脚输出 3.3V；写 0，输出 0V
- 输入模式：你读引脚，能知道外面是 3.3V（1）还是 0V（0）

按键的接法：一端接 GPIO，一端接 GND。按键松开时引脚悬空，需要内部上拉电阻把它拉到 3.3V；按下时被拉到 0V。

所以代码里输入模式要写 `INPUT_PULLUP`，并且**按下时读到的是 0，不是 1**。这是最常见的理解错误。

## 你的板子

| 功能 | GPIO |
|---|---|
| BOOT 按键 | 0 |
| 板载 RGB 灯 | 48 |

板载灯是一颗 WS2812 彩色灯，严格来说需要用专门库驱动。为了先练 GPIO，我们**先用它当普通灯试**（可能只亮一种颜色，没关系）。

## 第一个实验：串口看按键

```cpp
#define PIN_BOOT 0

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  Serial.println("press the BOOT button");
}

void loop() {
  // 因为用了 INPUT_PULLUP，按下时读到 LOW
  if (digitalRead(PIN_BOOT) == LOW) {
    Serial.println("pressed");
    delay(200);   // 这里先用 delay 方便观察，后面会改掉
  }
}
```

上传后按 BOOT 键，串口应该打印 `pressed`。

## 第二个实验：按键控制灯（错误示范）

先用最直观的写法，然后看它的问题：

```cpp
#define PIN_BOOT 0
#define PIN_LED  48

bool ledOn = false;

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
}

void loop() {
  if (digitalRead(PIN_BOOT) == LOW) {
    ledOn = !ledOn;          // 注意：这样一次按住会翻转很多次
    digitalWrite(PIN_LED, ledOn);
    delay(300);              // 用 delay 掩盖抖动，但会卡住程序
  }
}
```

这段能跑，但有两个问题：

1. `delay(300)` 期间程序完全停摆，什么都做不了
2. 靠 delay 掩盖按键抖动，是治标不治本

## 第三个实验：正确写法（非阻塞 + 边沿检测）

核心思路：`loop()` 每次只执行一小段，用 `millis()` 判断时间，而不是停下来等。

```cpp
#define PIN_BOOT 0
#define PIN_LED  48

bool ledOn = false;
bool lastRaw = false;      // 上一次读到的原始电平（按下为 true）
uint32_t lastChangeMs = 0; // 上次变化的时间

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  Serial.println("ready");
}

void loop() {
  bool raw = (digitalRead(PIN_BOOT) == LOW);
  uint32_t now = millis();

  // 只在"状态变化"且"距离上次变化超过 30ms"时才认为是有效按键
  if (raw != lastRaw && (now - lastChangeMs) > 30) {
    lastChangeMs = now;
    lastRaw = raw;

    if (raw) {              // 按下
      ledOn = !ledOn;
      digitalWrite(PIN_LED, ledOn);
      Serial.print("led = ");
      Serial.println(ledOn ? "on" : "off");
    }
  }
}
```

要点：

- `raw != lastRaw` 保证只在变化的那一瞬间触发，按住不会连发
- `30ms` 是软件消抖，真实按键抖动通常在 10ms 以内
- `now - lastChangeMs` 用无符号运算，即使 `millis()` 溢出也不会出错

## 验收标准

- [ ] 按一下 BOOT，灯状态翻转一次
- [ ] 按住不放，灯只翻转一次，不会疯狂闪烁
- [ ] 程序里 `loop()` 没有任何 `delay()`

## 为什么必须学会这个

音频采样每秒要处理成千上万个数据点。如果代码里到处是 `delay()`，麦克风的数据会直接丢失，声音就会断续或爆音。

从你第一个程序开始，就养成"永不阻塞"的习惯。

## 扩展验证

1. 改成按住灯亮、松开灯灭（提示：不需要边沿检测，直接 `digitalWrite(PIN_LED, raw)`）
2. 加一个计数器，串口打印总共按了多少次
3. 用两个时间戳做一个"每秒打印一次"的心跳输出，替代 `delay(1000)`
