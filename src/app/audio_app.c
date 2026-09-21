/*麦克风应用层*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_segment.h"
#include "audio_app.h"
#include "audio_format.h"
#include "audio_out.h"
#include "speech.h"

/*采集 -> 解码 -> AFE(feed->mase->fetch) -> 片段池*/
void audio_app_task(void *arg)
{
    /*1024帧采集缓冲区(64ms语音段)*/
    int32_t *raw = (int32_t *)calloc(RAW_I2S_FRAMES, RAW_I2S_BYTES_PER_FRAME);
    int16_t *pcm = (int16_t *)calloc(PCM_16_FRAMES, PCM_16_BYTES_PER_FRAME);

    while(1)
    {
        /*读取I2S原始数据 1024*64bit(溢出丢弃)*/
        if(mic_read_frame(raw, RAW_I2S_BUFFER_SIZES) != ESP_OK)
            continue;

        /*解码64bit（24bit有效 * 2）双声道*/
        audio_convert_to_s16(raw, RAW_I2S_FRAMES, pcm);

        if (audio_out_is_playing()) 
            continue;
            
        /*将PCM待处理队列中PCM喂给AFE框架*/
        speech_feed_pcm(pcm, PCM_16_FRAMES);
    }

    free(raw);
    free(pcm);
    vTaskDelete(NULL);
}




/*片段消费者:取出已完成的片段、打印统计、再归还给空闲队列。
 * 必须归还,否则 3 个槽用完后再也放不回池子,之后所有语音段都会被丢弃。*/
void seg_consumer_task(void *arg)
{
    audio_segment_desc_t d;
    uint32_t overrun = 0, used = 0;

    while (1) {
        if (audio_segment_retrieve(&d, portMAX_DELAY) == ESP_OK) {
            printf("seg=%lu samples=%u dur=%ums peak=%d trunc=%d\n",
                   (unsigned long)d.seq,
                    (unsigned)d.sample_count,
                   (unsigned)(d.sample_count * 1000 / 16000),
                   (int)d.peak,  
                   (int)d.truncated);

            audio_segment_release(&d);          /* ★ 必须归还,否则 3 段后池空 */

            audio_segment_stats(&overrun, &used);
            printf("stats: overrun=%lu pool_used=%lu\n",
                   (unsigned long)overrun, (unsigned long)used);
        }
    }
    vTaskDelete(NULL);
}
