/*音频片段环形缓冲区-单生产者消费者模型*/
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>


#include "freertos/FreeRTOS.h"
#include <freertos/queue.h>

#include "vad.h"


typedef struct 
{
    int16_t *pool;     /*片段存放区 pool_count * samples_per_seg, PSRAM*/
    size_t   samples_per_seg;   /* 单声道片段大小 = sample_rate * max_seconds(单声道) */
    uint8_t pool_count;  /*片段数量*/

    int16_t *pre_buf;    /*pre-roll 环形缓冲， 300ms*/
    size_t pre_samples;  /*每个样本存放大小*/
    size_t pre_w;      /*写入样本指针*/
    bool pre_filled;   /*写满标志*/

    QueueHandle_t free_q;   /*空闲片段索引*/
    QueueHandle_t ready_q;  /*已完成片段描述符*/
    uint32_t seq;
    uint32_t overrun_cnt;
    
}audio_segment_ctx_t;

typedef struct 
{
    uint8_t  pool_idx;      /* 池内第几个片段 */
    size_t   sample_count; /*样本计数*/
    uint32_t seq;
    int32_t  peak_l, peak_r;  /*左右峰值*/
    bool     truncated;
}audio_segment_desc_t;



/*初始化： 分配缓冲池（PSRAM）
 *sample_rate：片段采样率
 *max_seconds：单片段时长
 *pool_count：片段数量
 */
esp_err_t audio_segment_init(uint32_t sample_rate, uint32_t max_seconds, uint8_t pool_count);

/*传入stereo PCM和VAD事件
 *pcm：声音
 *frames:帧数块
 *ev：事件类型
*/
void audio_segment_feed(const int16_t *pcm,size_t frames, vad_event_t ev);

/*消费者：读取一个完成的片段（阻塞等待timeout）
 *out:读取位置
 *timeout：超时标记
 */
esp_err_t audio_segment_retrieve(audio_segment_desc_t *out, TickType_t timeout);

/*消费者：用完归还*/
void audio_segment_release(audio_segment_desc_t *seg);

/*统计：丢弃片段、当前池占用*/
void audio_segment_stats(uint32_t *overrun_cnt, uint32_t *pool_used);

