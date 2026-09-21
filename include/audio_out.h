#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

#define TX_FRAME_COUNT 256 /*对齐16ms*/
#define TONE_SAMPLE_RATE   16000
#define TONE_AMPLITUDE     8000      /* 约 24% 满幅,避免破音;可调 */
#define TONE_FADE_MS       5         /* 淡入淡出,防爆音 */
#define TWO_PI             6.2831853f

/*配置AMP_EN，准备播放*/
esp_err_t audio_out_init(void);

/* 提示音测试用 */
esp_err_t audio_out_tone(uint32_t freq_hz, uint32_t duration_ms);

/*16k单声道 16bit*/
esp_err_t audio_out_play(const int16_t *pcm, size_t samples);

/*播放storage里面的pcm*/
esp_err_t audio_out_play_file(const char *path);

/*开/关功放*/
void audio_out_set_enable(bool on);

/*半双工调用*/
bool audio_out_is_playing(void);

/*播放任务创建*/
void audio_out_task(void *arg);
