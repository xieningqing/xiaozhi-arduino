#include "Button.h"

Button::Button(uint8_t pin,uint32_t debounceMs):pin_(pin),debounceMs_(debounceMs){
}

void Button::begin(){
    pinMode(pin_,INPUT_PULLUP); // 按键接地，使用上拉输入

    rawPressed_ = digitalRead(pin_) == LOW; // 初始化原始状态
    stablePressed_ = rawPressed_; // 初始化稳定状态
    rawChangeMs_ = millis(); // 初始化原始状态变化时间
}

bool Button::update(){
    const bool raw = (digitalRead(pin_) == LOW); // 读取原始状态
    const uint32_t now = millis(); // 获取当前时间

    // 如果原始状态发生变化，更新变化时间
    if(raw != rawPressed_){
        rawPressed_ = raw;
        rawChangeMs_ = now;
        return false; // 状态变化，返回false
    }

    // 如果原始状态没有变化，检查是否超过消抖时间
    if(stablePressed_ != rawPressed_ &&(now - rawChangeMs_ >= debounceMs_)){
        stablePressed_ = rawPressed_; // 更新稳定状态
        return true; // 状态稳定变化，返回true
    }

    return false;
}

bool Button::isPressed() const{
    return stablePressed_;
}