#include "src/app/Application.h"

// Opus 库（esp32_opus）通过 alloca() 在调用任务自己的栈上分配临时缓冲。
// 实测：24KB 栈在编码时只剩 204 字节，说明单次 opus_encode 的临时开销
// 约 20KB，必须留出充足余量，否则会写穿栈并表现为随机地址崩溃。
// 另外 loopTask 里还要跑 TLS 握手和 JSON 解析，这些都吃栈。
// 因此把 loopTask 栈提升到 48KB。
SET_LOOP_TASK_STACK_SIZE(48 * 1024);

Application app;
void setup() {
  // put your setup code here, to run once:
   app.begin();
}

void loop() {
  // put your main code here, to run repeatedly:
  app.loop();
}
