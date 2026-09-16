/*麦克风应用层*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_segment.h"
#include "audio_app.h"
#include "audio_format.h"
#include "mic_inmp441.h"
#include "vad.h"
#include "audio_out.h"
#include "speech.h"

static bool was_playing = false;

/*采集 -> 解码 -> RMS -> VAD -> 片段,同时把原始 PCM 喂给 AFE 做唤醒*/
void audio_app_task(void *arg)
{
    static uint32_t block_id = 0;   /* 循环外定义 */

    int32_t *raw = (int32_t *)calloc(1, RX_BUFFER_BYTES);
    int16_t *pcm = (int16_t *)calloc(1, RX_PCM_BUFFER_BYTES);
    app_vad_event_t vad_event = 0;
    block_rms_t st = {};
    //int printf_cnt = 0;

    app_vad_t* vad = (app_vad_t *)malloc(sizeof(app_vad_t));
    assert(vad);
    app_vad_init(vad);

    while(1)
    {
        if(mic_read_frame(raw, RX_BUFFER_BYTES) != ESP_OK)
        {
            continue;
        }

        audio_convert_to_s16(raw, RX_FRAME_COUNT, pcm);
        st = audio_rms(pcm, RX_FRAME_COUNT);

        bool playing = audio_out_is_playing();
        if(playing)
        {
            was_playing = true;
            continue;
        }
        if(was_playing)  /*刚结束播放，复位vad*/
        {
            was_playing = false;
            vTaskDelay(pdMS_TO_TICKS(80));
            app_vad_init(vad);
            continue;
        }

        /*阶段一:AFE 只负责唤醒;VAD 与片段链路继续使用原始 pcm*/
        speech_event_t speech_ev = speech_feed(pcm, RX_FRAME_COUNT);

        vad_event = app_vad_process(vad, st.rms_l, st.rms_r);
        audio_segment_feed(pcm, RX_FRAME_COUNT, vad_event);

        if(speech_ev == SPEECH_EVT_WAKE)
        {
            printf("[WAKE] 唤醒命中\n");
        }

        block_id++;
        /*每 10 块(160ms)打一次:逐块 printf 会占掉大量时间,拖慢采集节奏。
         * 调 VAD 时想看得更密,把 10 改成 1 即可。*/
        if((block_id % 10) == 0)
        {
            printf("blk=%lu state=%d db=%.1f floor=%.1f event=%d frame=%ld\n",
                (unsigned long)block_id, vad->state, vad->voice_db,
                vad->noise_db, vad_event, vad->speech_frames);
        }
    }

    free(raw);
    free(pcm);
    free(vad);
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
            printf("seg=%lu samples=%u dur=%ums peakL=%d peakR=%d trunc=%d\n",
                   (unsigned long)d.seq, (unsigned)d.sample_count,
                   (unsigned)(d.sample_count * 1000 / 16000),
                   (int)d.peak_l, (int)d.peak_r, (int)d.truncated);

            audio_segment_release(&d);          /* ★ 必须归还,否则 3 段后池空 */

            audio_segment_stats(&overrun, &used);
            printf("stats: overrun=%lu pool_used=%lu\n",
                   (unsigned long)overrun, (unsigned long)used);
        }
    }
    vTaskDelete(NULL);
}
