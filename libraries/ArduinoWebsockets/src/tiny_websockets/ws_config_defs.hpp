#pragma once

#define _WS_CONFIG_NO_TRUE_RANDOMNESS
#define _WS_BUFFER_SIZE 512
// 读取握手响应的超时。原来是 1000ms，
// 直连服务器时响应可能更慢，超时会导致 readLine() 返回空串、
// 被误判为握手失败。放宽到 5000ms。
#define _CONNECTION_TIMEOUT 5000
