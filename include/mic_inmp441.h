#pragma once

#include "board_config.h"
#include "driver/i2s_std.h"

/*==========引脚映射==========*/
/*
发送/接收通道的I2S引脚映射如下：
SCK -> GPIO6/GPIO10
WS  -> GPIO7/GPIO11
DOUT-> GPIO8/GPIO12
DIN -> GPIO9/GPIO13
*/

#define BUFFER_SIZE 2048 //定义缓冲区大小

//i2s初始化函数
void i2s_init_std_simplex(void);

//i2s读取任务函数
void i2s_read_task(void *arg);

//i2s写入任务函数
void i2s_write_task(void *arg);