#include <stdlib.h>
#include <string.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_wn_models.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "speech.h"

#define TAG "speech"
#define SPEECH_PARTITION "model"    /*模型存放分区名*/
#define AFE_INPUT_FORMAT "MM"       /*两麦、无参考通道(半双工)*/
#define SPEECH_WAKENET_NAME "wn10_nihaoxiaozhi"  /*要使用的唤醒词模型*/
#define SPEECH_RINGBUF_FRAMES 8     /*AFE 输入环形缓冲帧数(默认偏小,容易"缓冲满"被拒)*/
#define SPEECH_FRAME_QUEUE 4        /*输出帧队列深度(防止一次 feed 出多帧时被覆盖)*/
#define SPEECH_FETCH_MAX_PER_FEED 3 /*一次 feed 最多再额外取几帧(防死循环的安全上限)*/
#define SPEECH_FETCH_LOG_EVERY 128  /*每多少次 fetch 打一次状态*/
#define SPEECH_FETCH_WAIT_MS 30     /*取"额外帧"时最多等多久(应小于输入帧时长)*/

/*
 * 关于"一次 feed 要取几次输出":
 *   实测 get_feed_chunksize()=1024 点/通道, get_fetch_chunksize()=512 点。
 *   输入一帧 64ms,输出一帧 32ms,所以一次 feed 会产出约两帧输出。
 *   早期版本每次只 fetch 一帧,输出环逐渐积压 -> SE 任务写不进输出环而停住
 *   -> 输入环灌满 -> feed 被拒(实测约 50%)。现在一次 feed 后把输出取空。
 */

static srmodel_list_t *s_models = NULL;         /*模型分区模型列表*/
static const esp_afe_sr_iface_t *s_afe = NULL;  /*AFE框架接口*/
static esp_afe_sr_data_t *s_afe_data = NULL;    /*AFE框架实例*/

static int s_feed_chunk = 0;        /*feed 时每通道的点数*/
static int s_feed_channels = 0;     /*feed 的通道数(双通道立体声)*/
static int s_fetch_chunk = 0;       /*fetch 输出的点数*/
static int s_fetch_per_feed = 1;    /*一次 feed 应取几帧输出(= 输入帧长 / 输出帧长)*/

static int16_t *s_accum = NULL;     /*攒帧缓冲区*/
static size_t s_accum_frame = 0;    /*已攒帧数*/

static int16_t *s_out = NULL;       /*输出帧队列,SPEECH_FRAME_QUEUE 个槽,每槽 s_fetch_chunk 点*/
static uint16_t s_out_len[SPEECH_FRAME_QUEUE]; /*每槽实际点数*/
static uint8_t s_q_head = 0;        /*队列头(下一次读取位置)*/
static uint8_t s_q_cnt = 0;         /*队列中已有帧数*/

static uint32_t s_feed_err = 0;     /*feed 被拒(环形缓冲满)的次数*/
static uint32_t s_feed_try = 0;     /*feed 调用次数(含被拒)*/
static uint32_t s_fetch_cnt = 0;    /*fetch 成功次数*/
static uint32_t s_frame_drop = 0;   /*输出队列满而丢帧的次数*/
static int64_t s_t0_us = 0;         /*计时起点,用来比较音频时长与实际耗时*/

/*按名字精确挑唤醒词模型。
 * 不用 esp_srmodel_filter 直接取"第一个 wn":模型包里可能同时存在多个唤醒词,
 * 谁排在前面取决于目录名字母序,不可控。*/
static char *speech_pick_wakenet(srmodel_list_t *models)
{
    for(int i = 0; i < models->num; i++)
    {
        if(strcmp(models->model_name[i], SPEECH_WAKENET_NAME) == 0)
        {
            return models->model_name[i];
        }
    }

    ESP_LOGW(TAG, "模型包里没有 '%s',退回使用第一个唤醒词模型", SPEECH_WAKENET_NAME);
    return esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
}

/*把一帧输出放进队列(队列满则丢最旧的一帧)*/
static void speech_push_frame(const int16_t *src, size_t n)
{
    if(src == NULL || n == 0)
    {
        return;
    }
    if(n > (size_t)s_fetch_chunk)
    {
        n = (size_t)s_fetch_chunk;
    }

    if(s_q_cnt >= SPEECH_FRAME_QUEUE)
    {
        s_frame_drop++;
        s_q_head = (uint8_t)((s_q_head + 1) % SPEECH_FRAME_QUEUE);
        s_q_cnt--;
    }

    uint8_t slot = (uint8_t)((s_q_head + s_q_cnt) % SPEECH_FRAME_QUEUE);
    memcpy(s_out + (size_t)slot * (size_t)s_fetch_chunk, src, n * sizeof(int16_t));
    s_out_len[slot] = (uint16_t)n;
    s_q_cnt++;
}

/*清空攒帧缓冲与输出队列*/
static void speech_flush(void)
{
    s_accum_frame = 0;
    s_q_head = 0;
    s_q_cnt = 0;
    memset(s_out_len, 0, sizeof(s_out_len));
}

/*释放已申请的资源,供初始化失败时收尾(可重复调用)*/
static void speech_release(void)
{
    if(s_afe != NULL && s_afe_data != NULL)
    {
        s_afe->destroy(s_afe_data);
    }
    s_afe_data = NULL;
    s_afe = NULL;

    if(s_accum != NULL)
    {
        free(s_accum);
        s_accum = NULL;
    }
    if(s_out != NULL)
    {
        free(s_out);
        s_out = NULL;
    }
    speech_flush();

    if(s_models != NULL)
    {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
}

esp_err_t speech_init(void)
{
    if(s_afe_data != NULL)
    {
        ESP_LOGW(TAG, "已初始化,重复调用被忽略");
        return ESP_OK;
    }

    /*获取剩余PSRAM堆空间*/
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /*挂载模型分区*/
    s_models = esp_srmodel_init(SPEECH_PARTITION);
    if(s_models == NULL)
    {
        ESP_LOGE(TAG, "模型分区 '%s' 挂载失败(检查 partitions.csv 与烧录)", SPEECH_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "模型分区 '%s' 共有 %d 个模型", SPEECH_PARTITION, s_models->num);
    for(int i = 0; i < s_models->num; i++)
    {
        ESP_LOGI(TAG, "  [%d] %s", i, s_models->model_name[i]);
    }

    /*生成默认配置*/
    afe_config_t *cfg = afe_config_init(AFE_INPUT_FORMAT,
                                        s_models,
                                        AFE_TYPE_SR,
                                        AFE_MODE_LOW_COST);
    if(cfg == NULL)
    {
        ESP_LOGE(TAG, "afe_config_init 失败");
        speech_release();
        return ESP_FAIL;
    }

    /*按本项目需求覆盖默认值*/
    cfg->aec_init = false;        /*半双工,没有回采通道*/
    cfg->se_init = true;          /*双麦增强*/
    cfg->ns_init = true;
    cfg->vad_init = true;         /*vad_model_name 留空 -> 使用 WebRTC VAD*/
    cfg->wakenet_init = true;
    cfg->afe_perferred_core = 1;  /*AFE 内部任务绑核 1*/
    cfg->afe_perferred_priority = 5;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_ringbuf_size = SPEECH_RINGBUF_FRAMES;

    cfg->wakenet_model_name = speech_pick_wakenet(s_models);
    if(cfg->wakenet_model_name == NULL)
    {
        ESP_LOGE(TAG, "模型分区里找不到唤醒词模型");
        afe_config_free(cfg);
        speech_release();
        return ESP_FAIL;
    }

    /*创建AFE实例*/
    s_afe = esp_afe_handle_from_config(cfg);
    if(s_afe == NULL)
    {
        ESP_LOGE(TAG, "esp_afe_handle_from_config 失败");
        afe_config_free(cfg);
        speech_release();
        return ESP_FAIL;
    }

    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);   /*实例建好后配置即可释放*/
    cfg = NULL;

    if(s_afe_data == NULL)
    {
        ESP_LOGE(TAG, "AFE 实例创建失败(PSRAM 不足?)");
        speech_release();
        return ESP_FAIL;
    }

    /*查参数,分配缓冲*/
    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    s_feed_channels = s_afe->get_feed_channel_num(s_afe_data);
    s_fetch_chunk = s_afe->get_fetch_chunksize(s_afe_data);

    /*输入帧 1024 点(64ms),输出帧 512 点(32ms),一次 feed 对应两帧输出*/
    s_fetch_per_feed = (s_feed_chunk + s_fetch_chunk - 1) / s_fetch_chunk;
    if(s_fetch_per_feed < 1)
    {
        s_fetch_per_feed = 1;
    }
    if(s_fetch_per_feed > SPEECH_FETCH_MAX_PER_FEED + 1)
    {
        s_fetch_per_feed = SPEECH_FETCH_MAX_PER_FEED + 1;
    }

    if(s_feed_chunk <= 0 || s_feed_channels <= 0 || s_fetch_chunk <= 0)
    {
        ESP_LOGE(TAG, "AFE 参数异常: feed = %d 点 / %d 通道, fetch = %d 点",
                 s_feed_chunk, s_feed_channels, s_fetch_chunk);
        speech_release();
        return ESP_FAIL;
    }

    s_accum = (int16_t *)calloc((size_t)s_feed_chunk * (size_t)s_feed_channels, sizeof(int16_t));
    if(s_accum == NULL)
    {
        ESP_LOGE(TAG, "攒帧缓冲分配失败");
        speech_release();
        return ESP_ERR_NO_MEM;
    }

    s_out = (int16_t *)calloc((size_t)s_fetch_chunk * SPEECH_FRAME_QUEUE, sizeof(int16_t));
    if(s_out == NULL)
    {
        ESP_LOGE(TAG, "输出队列分配失败");
        speech_release();
        return ESP_ERR_NO_MEM;
    }

    speech_flush();
    s_feed_err = 0;
    s_feed_try = 0;
    s_fetch_cnt = 0;
    s_frame_drop = 0;
    s_t0_us = esp_timer_get_time();

    /*打印实际生效的流水线*/
    s_afe->print_pipeline(s_afe_data);

    ESP_LOGI(TAG, "feed = %d 点/通道 x %d 通道(%.0f ms), fetch = %d 点(%.0f ms), 采样率 = %d Hz",
             s_feed_chunk, s_feed_channels,
             s_feed_chunk * 1000.0f / s_afe->get_samp_rate(s_afe_data),
             s_fetch_chunk,
             s_fetch_chunk * 1000.0f / s_afe->get_samp_rate(s_afe_data),
             s_afe->get_samp_rate(s_afe_data));
    ESP_LOGI(TAG, "编译期 CPU 频率 = %d MHz, 输入环 = %d 帧, 输出队列 = %d 帧",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, SPEECH_RINGBUF_FRAMES, SPEECH_FRAME_QUEUE);
    ESP_LOGI(TAG, "每次 feed 取 %d 帧输出(输入 %d 点 / 输出 %d 点)",
             s_fetch_per_feed, s_feed_chunk, s_fetch_chunk);

    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM: %u KB -> %u KB(AFE 占用 %d KB)",
             (unsigned)(psram_before / 1024),
             (unsigned)(psram_after / 1024),
             (int)((psram_before - psram_after) / 1024));

    return ESP_OK;
}

speech_event_t speech_feed(const int16_t *pcm_stereo, size_t frames)
{
    speech_event_t ev = SPEECH_EVT_NONE;

    if(s_afe == NULL || s_afe_data == NULL || s_accum == NULL || s_out == NULL)
    {
        return SPEECH_EVT_NONE;
    }
    if(s_feed_chunk <= 0 || s_feed_channels <= 0 || pcm_stereo == NULL || frames == 0)
    {
        return SPEECH_EVT_NONE;
    }

    size_t copied = 0;  /*本次调用已处理帧数*/
    while(copied < frames)
    {
        size_t need = (size_t)s_feed_chunk - s_accum_frame;
        size_t take = (frames - copied < need) ? (frames - copied) : need;

        memcpy(&s_accum[s_accum_frame * (size_t)s_feed_channels],
               &pcm_stereo[copied * (size_t)s_feed_channels],
               take * (size_t)s_feed_channels * sizeof(int16_t));

        s_accum_frame += take;
        copied += take;

        if(s_accum_frame < (size_t)s_feed_chunk)
        {
            continue;
        }

        s_accum_frame = 0;
        s_feed_try++;

        /*feed 被拒(输入环满)时也要继续 fetch:fetch 才是排空输出的动作*/
        if(s_afe->feed(s_afe_data, s_accum) <= 0)
        {
            s_feed_err++;
        }

        /*按固定比例取输出:第一帧阻塞等(保证拿到),后面的限时等。
         * 不要盲目多取——空环上重复 fetch 会让唤醒判定跑在旧数据上,导致连续误报。*/
        for(int i = 0; i < s_fetch_per_feed; i++)
        {
            afe_fetch_result_t *res = (i == 0)
                                      ? s_afe->fetch(s_afe_data)
                                      : s_afe->fetch_with_delay(s_afe_data,
                                                                pdMS_TO_TICKS(SPEECH_FETCH_WAIT_MS));
            if(res == NULL || res->data == NULL || res->data_size <= 0)
            {
                break;
            }

            speech_push_frame(res->data, (size_t)res->data_size / sizeof(int16_t));

            s_fetch_cnt++;
            if((s_fetch_cnt % SPEECH_FETCH_LOG_EVERY) == 0)
            {
                int64_t wall_ms = (esp_timer_get_time() - s_t0_us) / 1000;
                /*每毫秒 16 个采样点(16kHz)*/
                int64_t in_ms = (int64_t)(s_feed_try - s_feed_err)
                                * s_feed_chunk * s_feed_channels / 16;
                int64_t out_ms = (int64_t)s_fetch_cnt * s_fetch_chunk / 16;
                ESP_LOGI(TAG,
                         "AFE: 投入=%lld ms, 产出=%lld ms, 实际=%lld ms, feed %u 被拒 %u, "
                         "队列 %u/%d 丢帧 %u, 水位=%.2f",
                         in_ms,
                         out_ms,
                         wall_ms,
                         (unsigned)s_feed_try,
                         (unsigned)s_feed_err,
                         (unsigned)s_q_cnt,
                         SPEECH_FRAME_QUEUE,
                         (unsigned)s_frame_drop,
                         (double)res->ringbuff_free_pct);
            }

            if(res->wakeup_state == WAKENET_DETECTED)
            {
                ev = SPEECH_EVT_WAKE;
                ESP_LOGI(TAG, "唤醒命中: word = %d, model = %d, len = %d",
                         res->wake_word_index,
                         res->wakenet_model_index,
                         res->wake_word_length);
            }
        }
    }

    return ev;
}

bool speech_take_frame(const int16_t **out, size_t *frames)
{
    if(out != NULL)
    {
        *out = NULL;
    }
    if(frames != NULL)
    {
        *frames = 0;
    }

    if(s_out == NULL || s_q_cnt == 0)
    {
        return false;
    }

    uint8_t slot = s_q_head;
    if(out != NULL)
    {
        *out = s_out + (size_t)slot * (size_t)s_fetch_chunk;
    }
    if(frames != NULL)
    {
        *frames = s_out_len[slot];
    }

    s_q_head = (uint8_t)((s_q_head + 1) % SPEECH_FRAME_QUEUE);
    s_q_cnt--;
    return true;
}

void speech_set_wakenet(bool on)
{
    if(s_afe == NULL || s_afe_data == NULL)
    {
        return;
    }

    if(on)
    {
        /*播放期间攒的音频和残留的输出帧都要丢掉*/
        speech_flush();

        s_afe->reset_buffer(s_afe_data);
        if(s_afe->enable_wakenet(s_afe_data) < 0)
        {
            ESP_LOGW(TAG, "enable_wakenet 失败");
        }
        ESP_LOGI(TAG, "唤醒已开启");
    }
    else
    {
        if(s_afe->disable_wakenet(s_afe_data) < 0)
        {
            ESP_LOGW(TAG, "disable_wakenet 失败");
        }
        ESP_LOGI(TAG, "唤醒已关闭(半双工)");
    }
}

void speech_dump(void)
{
    if(s_afe == NULL || s_afe_data == NULL)
    {
        ESP_LOGW(TAG, "speech 未初始化,无可打印信息");
        return;
    }

    ESP_LOGI(TAG, "分区 '%s',模型数量 = %d",
             SPEECH_PARTITION, (s_models != NULL) ? s_models->num : 0);

    ESP_LOGI(TAG, "feed = %d 点/通道 x %d 通道, fetch = %d 点, 采样率 = %d Hz",
             s_feed_chunk,
             s_feed_channels,
             s_fetch_chunk,
             s_afe->get_samp_rate(s_afe_data));

    ESP_LOGI(TAG, "PSRAM 剩余 %u KB,内部RAM 剩余 %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    /*流水线由 AFE 内部打印,形如 [input] -> |AEC(..)| -> |WakeNet(..)| -> [output]*/
    s_afe->print_pipeline(s_afe_data);
}
