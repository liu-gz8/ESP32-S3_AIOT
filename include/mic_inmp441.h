#pragma once

#include <stdint.h>

#include "board_config.h"
#include "driver/i2s_std.h"

#define MIC_SAMPLE_RATE_HZ  16000  //i2s频率
#define RX_FRAME_COUNT 256 //定义接收帧数
#define RX_BYTES_PER_FRAME (2 * sizeof(int32_t))  // 每帧字节数
#define RX_BUFFER_BYTES (RX_FRAME_COUNT * RX_BYTES_PER_FRAME)  //缓冲区大小

/*==========引脚映射==========*/
/*
发送/接收通道的I2S引脚映射如下：
SCK -> GPIO10
WS  -> GPIO11
DOUT-> GPIO12
DIN -> GPIO13
*/

//i2s初始化函数
esp_err_t mic_init(void);

//i2s读取函数
esp_err_t mic_read_frame(int32_t *r_buf, const size_t r_bytes);
