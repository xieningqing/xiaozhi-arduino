#pragma once

#include<Arduino.h>

class Button{
    public:
        Button(uint8_t pin,uint32_t debounceMs);

        void begin();
        bool update();
        bool isPressed() const;

    private:

    uint8_t pin_;
    uint32_t debounceMs_;

    bool rawPressed_ = false; // 最近一次直接读到的原始状态
    bool stablePressed_ = false; // 消抖确认后的稳定状态
    uint32_t rawChangeMs_ = 0;    // 原始状态最近一次变化的时间
};