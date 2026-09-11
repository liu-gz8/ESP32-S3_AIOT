#pragma once
#include "audio_format.h"
#include "mic_inmp441.h"

#define RX_PCM_BYTES_PER_FRAME (2 * sizeof(int16_t))   //解耦帧大小
#define RX_PCM_BUFFER_BYTES (RX_FRAME_COUNT * RX_PCM_BYTES_PER_FRAME)  //解耦缓冲区


//打印解码后音频任务
void audio_app_task(void *arg);
void seg_consumer_task(void *arg);