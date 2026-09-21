#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "mic_inmp441.h"
#include "audio_segment.h"


static audio_segment_ctx_t s_ctx;

static bool     s_active;        /* 是否正在录一段 */
static bool     s_prev_speech;   /* 上一次的语音状态，用于边沿检测 */
static uint8_t  s_cur_idx;
static size_t   s_cur_count;
static uint8_t  s_cur_cmd_id;
static bool     s_has_cmd;
static size_t   s_min_samples;   /* 短于这个长度不算有效语音 */


/*结束输入:归还队列索引*/
static void finish_segment(bool truncated);

/*返回片段池当前片段索引（idx）的地址（int16_t *）*/
static int16_t *seg_base(uint8_t idx);

/*添加（n）个样本帧（src）到当前片段（自动截断）*/
static void append_samples(const int16_t *src, size_t n);

/*将当前预缓冲区内容拷贝到当前片段的缓冲区*/
static void append_pre_roll(void);


esp_err_t audio_segment_init(uint32_t sample_rate, uint32_t max_seconds, uint8_t pool_count)
{
    s_ctx.pre_filled = false;
    s_min_samples = (size_t)sample_rate * AUDIO_SEG_MIN_MS / 1000;
    /*数据非法溢出检查*/
    if (max_seconds > 30 || pool_count > 8) return ESP_ERR_INVALID_ARG;
    if(s_ctx.pool != NULL) return ESP_ERR_INVALID_STATE;
    if(sample_rate == 0 || max_seconds == 0 || pool_count == 0 ) 
        return ESP_ERR_INVALID_ARG;


    /*片段缓冲区创建（PSRAM）*/
    s_ctx.samples_per_seg = (size_t)sample_rate * max_seconds;
    s_ctx.pool_count = pool_count;
    s_ctx.pool = 
    (int16_t *)heap_caps_malloc
    (s_ctx.samples_per_seg * s_ctx.pool_count * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if(!s_ctx.pool)
        return ESP_ERR_NO_MEM;


    /*pre-roll缓冲区创建（300ms）（RAM）*/
    s_ctx.pre_samples = sample_rate * 300 / 1000;  //300ms
    s_ctx.pre_buf = heap_caps_malloc(s_ctx.pre_samples * sizeof(int16_t),MALLOC_CAP_INTERNAL);
    if(!s_ctx.pre_buf)
        {
            heap_caps_free(s_ctx.pool);
            s_ctx.pool = NULL;
            return ESP_ERR_NO_MEM;
        }
    else
        memset(s_ctx.pre_buf, 0, s_ctx.pre_samples * sizeof(int16_t));
    s_ctx.pre_w = 0;
    s_ctx.seq = 0;
    s_ctx.overrun_cnt = 0;

    /*队列创建*/
    s_ctx.free_q = xQueueCreate(pool_count, sizeof(uint8_t)); //空闲片段索引
    s_ctx.ready_q = xQueueCreate(pool_count, sizeof(audio_segment_desc_t)); /*已完成片段*/
    if(!s_ctx.free_q || !s_ctx.ready_q)
        {
            heap_caps_free(s_ctx.pool);
            s_ctx.pool = NULL;
            heap_caps_free(s_ctx.pre_buf);
            s_ctx.pre_buf = NULL;
            
            if (s_ctx.free_q) 
                {
                    vQueueDelete(s_ctx.free_q);
                    s_ctx.free_q = NULL; 
                }
            if (s_ctx.ready_q) 
                {
                    vQueueDelete(s_ctx.ready_q); 
                    s_ctx.ready_q = NULL; 
                }
            return ESP_ERR_NO_MEM;
        }
    
    
    for(uint8_t i = 0; i < pool_count; i++)
    {
        xQueueSend(s_ctx.free_q, &i, 0);
    }

    static const char *TAG = "AUDIO_SEG";
    ESP_LOGI(TAG, "seg init: seg=%u samples, pool=%u B, pre=%u B",
         (unsigned)s_ctx.samples_per_seg,
         (unsigned)(s_ctx.samples_per_seg * pool_count * sizeof(int16_t)),
         (unsigned)(s_ctx.pre_samples * sizeof(int16_t)));
    return ESP_OK;
}

void audio_segment_feed(const int16_t *pcm,size_t frames, bool is_speech, const int16_t *head, size_t head_frames)
{
    bool rising  = (is_speech && !s_prev_speech);
    bool falling = (!is_speech && s_prev_speech);
    s_prev_speech = is_speech;

    if (rising)
    {
        uint8_t idx;
        if (xQueueReceive(s_ctx.free_q, &idx, 0) != pdTRUE)
        {
            s_ctx.overrun_cnt++;
            s_active = false;
            return;
        }
        s_cur_idx = idx; s_cur_count = 0;
        s_cur_cmd_id = 0; s_has_cmd = false;
        s_active = true;

        if (head && head_frames > 0)
            append_samples(head, head_frames); 
        else
            append_pre_roll();          

        append_samples(pcm, frames);


        static bool s_head_logged = false;
        if (!s_head_logged) {
            s_head_logged = true;
            ESP_LOGI("AUDIO_SEG", "补头来源: %s", head_frames ? "AFE vad_cache" : "自研 pre-roll");
        }

    }
    else if (is_speech && s_active)
    {
        append_samples(pcm, frames);
    }
    else if (falling && s_active)
    {
        finish_segment(false);
        return;
    }

    /* 顶到时长上限就先收尾，不等 END */
    if (s_active && s_cur_count >= s_ctx.samples_per_seg)
        finish_segment(true);

}

static void finish_segment(bool truncated)
{
    s_active = false;

    /* 太短的不进池子，直接还回去（省得消费者拿到一堆碎片） */
    if (s_cur_count < s_min_samples)
    {
        uint8_t idx = s_cur_idx;
        xQueueSend(s_ctx.free_q, &idx, 0);
        s_cur_count = 0;
        return;
    }

    /* 峰值收尾扫一遍就行，不必每帧统计 */
    int32_t peak = 0;
    const int16_t *p = seg_base(s_cur_idx);
    for (size_t i = 0; i < s_cur_count; i++)
    {
        int32_t a = p[i] < 0 ? -p[i] : p[i];
        if (a > peak) peak = a;
    }

    audio_segment_desc_t d = {
        .pool_idx     = s_cur_idx,
        .sample_count = s_cur_count,
        .seq          = s_ctx.seq,
        .peak         = peak,
        .truncated    = truncated,
        .cmd_id       = s_cur_cmd_id,
        .has_cmd      = s_has_cmd,
    };
    s_ctx.seq++;

    if (xQueueSend(s_ctx.ready_q, &d, 0) != pdTRUE)
    {
        s_ctx.overrun_cnt++;
        uint8_t idx = s_cur_idx;
        xQueueSend(s_ctx.free_q, &idx, 0);   /* 推不进去就归还，别漏 */
    }
    s_cur_count = 0;
}

static int16_t *seg_base(uint8_t idx)
{
    return s_ctx.pool + (size_t)idx * s_ctx.samples_per_seg;
}

/*添加（n）个样本帧（src）到当前片段（自动截断）*/
static void append_samples(const int16_t *src, size_t n)
{
    size_t size = s_ctx.samples_per_seg - s_cur_count;
    if(n > size)
    {
        n = size;
    }
    memcpy(seg_base(s_cur_idx) + s_cur_count, src, n * sizeof(int16_t));
    s_cur_count += n;
}

void audio_segment_mark_cmd(uint8_t cmd_id)
{
    if (s_active)
    {
        s_cur_cmd_id = cmd_id;
        s_has_cmd    = true;
    }
}

static void append_pre_roll(void)
{
    size_t n = s_ctx.pre_samples;
    size_t old = s_ctx.pre_w;
    if(s_ctx.pre_filled)   //环形写满
    {
        /*写尾段*/
        append_samples(s_ctx.pre_buf + old, n - old);
        /*回头写首段*/
        append_samples(s_ctx.pre_buf, old);
    }
    else /*环形没写满*/
    {
        append_samples(s_ctx.pre_buf, s_ctx.pre_w);
    }
}

esp_err_t audio_segment_retrieve(audio_segment_desc_t *out, TickType_t timeout)
{
    if(!out || !s_ctx.ready_q)
        return ESP_ERR_INVALID_ARG;
    return (xQueueReceive(s_ctx.ready_q, out, timeout) == pdTRUE)
             ? ESP_OK : ESP_ERR_TIMEOUT;
}

void audio_segment_release(audio_segment_desc_t *seg)
{
    if(!seg || !s_ctx.free_q)
        return;
    xQueueSend(s_ctx.free_q, &seg->pool_idx, 0); /*消费者用完归还*/
}

void audio_segment_stats(uint32_t *overrun_cnt, uint32_t *pool_used)
{
    if(overrun_cnt)
        *overrun_cnt = s_ctx.overrun_cnt;
    if(pool_used)
        *pool_used = s_ctx.pool_count - uxQueueMessagesWaiting(s_ctx.free_q);
}
