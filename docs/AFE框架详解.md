# ESP-SR AFE 框架详解(函数 / 宏 / 结构体)

> 依据:工程内实际安装的 esp-sr 2.5.3,头文件路径
> `managed_components/espressif__esp-sr/include/esp32s3/`
> 整理时间:2026-09-16

## 1. 一句话定位

AFE(Audio Front-End)是**音频前端流水线框架**,不是模型。它把多路麦克风原始 PCM
变成一路干净的单声道 PCM,同时在里面跑可选的神经网络模型(唤醒词 / 降噪 / VAD),
并自己管理任务、内存、环形缓冲和帧对齐。

```
多通道 int16 PCM --feed()--> [ AEC -> SE -> NS -> VAD -> AGC -> WakeNet ] --fetch()--> 结果结构体
                                  (传统算法/可选模型)      (模型)
```

输出与输入的关系:

- 输入:多通道交织,采样率 16kHz,数据格式 int16,每帧点数由 `get_feed_chunksize()` 决定
- 输出:永远是**单通道**,每帧点数由 `get_fetch_chunksize()` 决定
- 唤醒词模型在 AFE **内部**被调用;命令词模型(MultiNet)不在 AFE 内,要用输出自己喂

## 2. 头文件分工

| 头文件 | 提供什么 |
|---|---|
| `esp_afe_config.h` | 所有枚举、宏、`afe_config_t`、配置类函数 |
| `esp_afe_sr_iface.h` | `afe_fetch_result_t`、`esp_afe_sr_iface_t`(函数指针表) |
| `esp_afe_sr_models.h` | `esp_afe_handle_from_config()` |
| `model_path.h` | `esp_srmodel_init/filter/deinit`、`srmodel_list_t` |
| `esp_aec.h` / `esp_aec_nlp.h` | `aec_mode_t`、`aec_nlp_level_t` |
| `esp_vad.h` | `vad_mode_t`、`vad_state_t` |
| `esp_wn_iface.h` / `esp_wn_models.h` | `wakenet_state_t`、`det_mode_t`、`ESP_WN_PREFIX` |

## 3. 宏定义

| 宏 | 值 | 用处 |
|---|---|---|
| `AFE_MAX_WAKEWORD_NUM` | 3 | 单个 AFE 实例最多挂几个唤醒词模型(用于双唤醒词等) |
| `AFE_VAD_ENERGY_THRESHOLD_DEFAULT` | -60.0f | VAD 默认的最低平均帧能量(dBFS) |
| `AFE_VAD_ENERGY_THRESHOLD_MIN` | -100.0f | 上面这个阈值的下限 |
| `ESP_WN_PREFIX` | "wn" | 用 `esp_srmodel_filter` 过滤出唤醒词模型 |
| `ESP_MN_PREFIX` / `ESP_MN_CHINESE` | "mn" / "cn" | 过滤命令词模型(mn7_cn 用得到) |

## 4. 枚举逐个讲

### 4.1 选择型(建实例前必须定)

| 枚举 | 取值 | 含义 |
|---|---|---|
| `afe_type_t` | `AFE_TYPE_SR` / `AFE_TYPE_VC` / `AFE_TYPE_VC_8K` / `AFE_TYPE_FD` | 场景:语音识别(不含非线性降噪) / 语音通话 / 8k 通话 / 全双工 |
| `afe_mode_t` | `AFE_MODE_LOW_COST` / `AFE_MODE_HIGH_PERF` | 算力/内存与效果的取舍;LOW_COST 更省 |
| `afe_memory_alloc_mode_t` | `MORE_INTERNAL` / `INTERNAL_PSRAM_BALANCE` / `MORE_PSRAM` | 内存往内部 RAM 还是 PSRAM 倾斜 |
| `aec_mode_t` | `AEC_MODE_SR_LOW_COST` / `SR_HIGH_PERF` / `VOIP_*` / `FD_*` | 回声消除模式,要和 `afe_type_t` 场景对应 |
| `aec_nlp_level_t` | `NORMAL` / `AGGR`(默认) / `VERYAGGR` | 非线性回声抑制强度,越激进残留回声越少、但可能伤语音 |
| `afe_ns_mode_t` | `AFE_NS_MODE_WEBRTC` / `AFE_NS_MODE_NET` | 降噪用传统算法还是 NSNet 模型 |
| `afe_agc_mode_t` | `AFE_AGC_MODE_WEBRTC` / `AFE_AGC_MODE_WAKENET` | 增益由 WebRTC AGC 算,还是借用 WakeNet 的增益 |
| `det_mode_t` | `DET_MODE_90` / `95` / `2CH_90` / `2CH_95` / `3CH_90` / `3CH_95` | 唤醒检测模式:90 更保守、95 更激进;后缀 2CH/3CH 是多帧判定窗口;**注释里 90 从 flash 加载、95 从 PSRAM 加载**,这是省 PSRAM 的旋钮 |
| `vad_mode_t` | `VAD_MODE_0`(Normal) ~ `VAD_MODE_4`(最激进) | WebRTC VAD 的灵敏度,数字越大越容易判成语音 |

### 4.2 状态型(运行时读)

| 枚举 | 取值 | 含义 |
|---|---|---|
| `wakenet_state_t` | `WAKENET_NO_DETECT`=0 / `WAKENET_DETECTED`=1 / `WAKENET_CHANNEL_VERIFIED`=-1 | 唤醒检测结果;-1 表示"输出通道已验证"的中间态 |
| `vad_state_t` | `VAD_SILENCE`=0 / `VAD_SPEECH`=1 | 语音活动结果 |
| `afe_vad_state_t` | 同上,已废弃 | 老代码里还能见到,新代码用 `vad_state_t` |
| `afe_sr_mode_t` | 已废弃 | 用 `afe_mode_t` 代替 |

### 4.3 其它

| 枚举 | 用处 |
|---|---|
| `afe_mn_peak_agc_mode_t` | 抓取音频的峰值目标:-9 / -6 / -3 dB,或 `AFE_MN_PEAK_NO_AGC` 不做 |
| `afe_debug_hook_type_t` | 调试抽头位置:`MASE_TASK_IN`(SE 任务输入) / `FETCH_TASK_IN`(fetch 任务输入) |

## 5. 结构体逐个讲

### 5.1 `afe_pcm_config_t`(输入通道描述)

| 成员 | 含义 |
|---|---|
| `total_ch_num` | 总通道数(麦克风 + 参考 + 占位) |
| `mic_num` / `mic_ids` | 麦克风通道数量与下标数组 |
| `ref_num` / `ref_ids` | 回采参考通道数量与下标数组 |
| `sample_rate` | 采样率 |

由 `afe_parse_input_format("MM", &pcm)` 从字符串解析出来,一般不用手填。

### 5.2 `afe_config_t`(核心配置,分组看)

**AEC 回声消除**

| 字段 | 说明 |
|---|---|
| `aec_init` | 是否启用。无回采通道(半双工)时设 `false` 可以省内存和 CPU |
| `aec_mode` / `aec_filter_length` / `aec_nlp_level` | 模式、滤波长度、非线性抑制强度 |

**SE 阵列处理**

| 字段 | 说明 |
|---|---|
| `se_init` | 双麦时开启,做波束形成/盲源分离。注意 `afe_config_check` 的规则:两通道输入时 SE 优先于 NS;若 SE 关闭则只用第一个麦克风通道 |

**NS 降噪**

| 字段 | 说明 |
|---|---|
| `ns_init` / `afe_ns_mode` | 是否启用、用 WebRTC 还是 NSNet |
| `ns_model_name` | 用 NSNet 时的模型名(填了才算调模型) |

**VAD**

| 字段 | 说明 |
|---|---|
| `vad_init` / `vad_mode` | 是否启用、WebRTC VAD 灵敏度 |
| `vad_model_name` | 填了就用 VADNet 模型,留空用 WebRTC |
| `vad_min_speech_ms` | 最短语音时长(>32ms,默认 128ms),避免把爆破音当语音 |
| `vad_min_noise_ms` | 最短静音时长(>64ms,默认 1000ms),决定多久算"说完了" |
| `vad_delay_ms` | 首个语音帧的延迟(默认 128ms);注释明确说**如果 `vad_cache` 补不回被截断的语音,就调大它** |
| `vad_mute_playback` | 播放时是否把回采静音(VAD 用) |
| `vad_enable_channel_trigger` | 是否用 VAD 来决定输出通道 |
| `vad_energy_threshold` | 报告语音所需的最低平均帧能量(dBFS);只在用 VAD 模型时生效,且创建后不能改 |

**WakeNet**

| 字段 | 说明 |
|---|---|
| `wakenet_init` | 是否启用唤醒 |
| `wakenet_model_name` / `wakenet_model_name_2` | 主/副唤醒词模型名,来自 `esp_srmodel_filter` |
| `wakenet_mode` | 检测模式,见 `det_mode_t`(直接影响内存与误唤醒率) |

**AGC**

| 字段 | 说明 |
|---|---|
| `agc_init` / `agc_mode` | 是否启用、增益来源 |
| `agc_compression_gain_db` / `agc_target_level_dbfs` | 压缩增益(默认 9dB)、目标电平(默认 -3dBFS) |

**通用**

| 字段 | 说明 |
|---|---|
| `pcm_config` | 输入通道配置 |
| `afe_mode` / `afe_type` | 算力档、场景 |
| `afe_perferred_core` / `afe_perferred_priority` | AFE 内部 SE 任务绑在哪个核、什么优先级 |
| `afe_ringbuf_size` | 环形缓冲有多少帧,越大越抗抖动、越占内存 |
| `memory_alloc_mode` | 内存倾斜策略 |
| `afe_linear_gain` | 输出线性增益,范围 [0.1, 10.0],直接乘在输出幅度上 |
| `debug_init` | 是否开启调试 |
| `fixed_first_channel` | 首次唤醒后是否固定用某一路麦克风原始数据 |
| `fixed_output_channel` | 是否固定输出通道(否则由 WakeNet/VAD 动态选) |
| `output_playback_channel` | 是否把回采参考通道也输出到 fetch 结果里 |

### 5.3 `afe_fetch_result_t`(每帧输出,最常打交道)

| 成员 | 单位 | 含义与用法 |
|---|---|---|
| `data` | - | 目标通道音频(干净单声道),就是你之后要喂 MultiNet / VAD / 存片段的 |
| `data_size` | **字节** | 算点数要 `/= sizeof(int16_t)` |
| `vad_cache` / `vad_cache_size` | - / 字节 | VAD 缓存,**用来补回被截断的语音开头**——这就是 AFE 版的 pre-roll |
| `data_volume` | dBFS | VAD 输入的平均能量(AGC 之前算的)。开 VADNet 时窗口覆盖 `vad_min_speech_ms`,否则只有一帧 |
| `wakeup_state` | - | `wakenet_state_t`,判断唤醒就认这个 |
| `wake_word_index` | - | 命中的是第几个唤醒词(从 1 开始) |
| `wakenet_model_index` | - | 命中来自第几个 WakeNet 模型(挂多个模型时有用) |
| `vad_state` | - | `vad_state_t`,语音/静音 |
| `trigger_channel_id` | - | 当前选中的是哪一路麦克风通道 |
| `wake_word_length` | 采样点 | 唤醒词本身有多长,可以用来把唤醒词那段声音切出来 |
| `ret_value` | - | fetch 的返回状态,**头文件没有公开取值定义**,第一次跑先打印观察,别当断言用 |
| `raw_data` / `raw_data_channels` | - | 多通道原始输出(需要时才有意义) |
| `ringbuff_free_pct` | 0~1 | 环形缓冲剩余比例;注释称大于 0.5 表示"缓冲繁忙",可用它判断喂流是否跟不上 |
| `reserved` | - | 预留 |

### 5.4 `esp_afe_sr_iface_t`(函数指针表,即"框架 API")

分五组记:

- **生命周期**:`create_from_config`、`destroy`
- **数据流**:`feed`、`fetch`、`fetch_with_delay`、`reset_buffer`
- **查询**:`get_feed_chunksize`、`get_fetch_chunksize`、`get_channel_num`、`get_feed_channel_num`、`get_fetch_channel_num`、`get_samp_rate`
- **模块开关**:`disable_/enable_wakenet`、`disable_/enable_aec`、`disable_/enable_se`、`disable_/enable_vad`、`disable_/enable_ns`、`disable_/enable_agc`、`reset_vad`
- **唤醒词调节**:`set_wakenet_threshold`、`reset_wakenet_threshold`、`add_wakenet_model`、`print_pipeline`

### 5.5 `afe_task_into_t`

一个把 AFE 句柄和任务句柄打包的结构(`afe_data`、`afe_handle`、`feed_task`、`fetch_task`),
适合自己写双任务(feed/fetch 分离)时使用;本项目单任务就够,不必用。

## 6. 函数逐个讲

### 6.1 配置类(`esp_afe_config.h`)

| 函数 | 作用与注意点 |
|---|---|
| `afe_config_init(input_format, models, type, mode)` | **入口**。按输入格式和目标芯片填一份默认配置(尽量把算法都开上),之后自己微调 |
| `afe_config_check(cfg)` | 检查冲突并**修改**配置。例:两通道输入时 SE 优先于 NS;SE 关了只留第一个麦克风通道 |
| `afe_config_alloc()` / `afe_config_free()` | 手动分配/释放配置结构。`afe_config_init` 的返回值也要用 `free` 释放 |
| `afe_config_copy(dst, src)` | 拷贝配置,做多实例或保存快照用 |
| `afe_config_print(cfg)` | 打印配置,排查"我设的到底生效没有" |
| `afe_parse_input_format(fmt, pcm)` | 把 `"MM"`/`"MMNR"` 解析成通道配置 |
| `afe_parse_input(data, frame, mic, ref, pcm)` | 把交织的多通道数据拆成"麦克风连续 + 回采连续" |
| `afe_parse_data(data, frame, ch, out)` | 交织 → 连续(每通道一段) |
| `afe_format_data(data, frame, ch, out)` | 连续 → 交织 |
| `afe_adjust_gain(data, frame, factor)` | 就地调整增益 |
| `afe_concat_data(in, in_frame, ch, out, out_frame)` | 把两块数据拼接成一块 |

### 6.2 实例与运行(`esp_afe_sr_iface.h`)

| 函数 | 签名要点 | 说明 |
|---|---|---|
| `esp_afe_handle_from_config(cfg)` | 返回 `const esp_afe_sr_iface_t*` | 从配置拿"类",一次就够 |
| `create_from_config(cfg)` | 返回 `esp_afe_sr_data_t*` | 创建实例,内部会建 SE 任务、分配内存 |
| `feed(afe, in)` | `in` 长度 = `get_feed_chunksize() x 通道数` | 喂一帧,**通道交织**;返回输入长度 |
| `fetch(afe)` | 阻塞,默认超时 2000ms | 取一帧处理结果(单通道) |
| `fetch_with_delay(afe, ticks)` | 自定义超时 | 需要更短/更长等待时用 |
| `reset_buffer(afe)` | 返回 1 成功 / -1 失败 | 清空环形缓冲,播放完提示音之类的场景用 |
| `get_feed_chunksize(afe)` | 每通道点数 | 决定你要攒多少点才喂一次 |
| `get_fetch_chunksize(afe)` | 每通道点数 | 决定输出缓冲开多大 |
| `get_channel_num` / `get_feed_channel_num` / `get_fetch_channel_num` | - | 总/输入/输出通道数 |
| `get_samp_rate(afe)` | Hz | 采样率,用于校验 |
| `disable_wakenet` / `enable_wakenet` | 返回 -1 失败 / 0 关 / 1 开 | 半双工必备 |
| `disable_aec`/`enable_aec`、`disable_se`/`enable_se`、`disable_vad`/`enable_vad`、`disable_ns`/`enable_ns`、`disable_agc`/`enable_agc` | 同上 | 运行期开关各模块,不重建实例 |
| `reset_vad(afe)` | - | 清 VAD 状态,场景切换时用 |
| `set_wakenet_threshold(afe, index, thr)` | index 只能 1 或 2;thr 在 [0.4, 0.9999] | 运行期调唤醒灵敏度:调高=更难唤醒但误触发更少 |
| `reset_wakenet_threshold(afe, index)` | - | 恢复模型自带阈值 |
| `add_wakenet_model(afe, name)` | 返回加入后的模型数量 | 运行期再挂一个唤醒词(总数不超过 `AFE_MAX_WAKEWORD_NUM`) |
| `print_pipeline(afe)` | - | 打印真正生效的流水线,形如 `[input] -> |AEC(...)| -> |WakeNet(wn9_hilexin)| -> [output]` |
| `destroy(afe)` | - | 释放实例 |

### 6.3 模型管理(`model_path.h`)

| 函数 | 说明 |
|---|---|
| `esp_srmodel_init(partition_label)` | 挂载模型分区,返回模型列表;分区名要和 `partitions.csv` 一致(本项目是 `"model"`) |
| `esp_srmodel_filter(models, keyword1, keyword2)` | 按前缀/语言筛模型:`("wn", NULL)` 找唤醒词,`("mn","cn")` 找中文命令词 |
| `esp_srmodel_exists(models, name)` | 判断某个模型在不在 |
| `esp_srmodel_get_wake_words(models, name)` | 取某个唤醒词模型里包含的唤醒词名字 |
| `esp_srmodel_deinit(models)` | 释放列表 |

## 7. 生命周期(本项目的实际节奏)

```
启动
  esp_srmodel_init("model")
  afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_LOW_COST)
     └─ 微调字段(aec/se/ns/vad/wakenet/core/prio/memory)
  esp_afe_handle_from_config(cfg) -> iface
  iface->create_from_config(cfg)  -> afe
  iface->print_pipeline(afe)        打印确认
  afe_config_free(cfg)

循环(每 16ms 采集一块 256 点)
  攒帧到 get_feed_chunksize()(预计 512)
  iface->feed(afe, acc)            喂一帧
  res = iface->fetch(afe)          取一帧
     res->wakeup_state == WAKENET_DETECTED ? 触发唤醒
     res->data 交给 VAD / 片段池 / MultiNet

播放提示音
  iface->disable_wakenet(afe) -> 播放 -> iface->reset_buffer + enable_wakenet

退出(本项目不退出)
  iface->destroy(afe); esp_srmodel_deinit(models)
```

## 8. 本项目的推荐配置(含理由)

| 字段 | 取值 | 理由 |
|---|---|---|
| `input_format` | `"MM"` | 两麦、无回采通道(半双工) |
| `afe_type` / `afe_mode` | `AFE_TYPE_SR` / `AFE_MODE_LOW_COST` | 识别场景 + 省算力 |
| `aec_init` | `false` | 没有参考通道,开了也没用,反而占内存 |
| `se_init` | `true` | 双麦的价值在这里 |
| `ns_init` | `true` | 先用 WebRTC,后续可换 NSNet 对比 |
| `vad_init` | `true` | 先用 WebRTC,自研 VAD 继续管片段切分 |
| `wakenet_init` | `true` | 唤醒必备 |
| `wakenet_model_name` | `esp_srmodel_filter(models, ESP_WN_PREFIX, NULL)` | 拿到 wn10_nihaoxiaozhi |
| `wakenet_mode` | 先默认,后续用 `set_wakenet_threshold` 调 | 误唤醒/漏唤醒的调节旋钮 |
| `afe_perferred_core` / `priority` | 1 / 5 | 音频任务独立在一个核上 |
| `memory_alloc_mode` | `AFE_MEMORY_ALLOC_MORE_PSRAM` | 内部 SRAM 更宝贵(WiFi/栈都要用) |
| `afe_ringbuf_size` | 保持默认 | 先不动,出现丢帧再调大 |

## 9. 常见坑

1. `feed` 的长度是"**每通道点数 x 通道数**",不是字节数也不是单通道点数。
2. `fetch` 返回的 `data_size` 是**字节**,算点数要除以 2。
3. AFE 按固定帧长工作(预计 512 点/32ms),采集块 256 点时必须攒帧,直接喂会错帧。
4. `fetch` 默认阻塞 2000ms;喂流停了它会干等,别在没有数据时反复调它。
5. 唤醒词是"在流里逐帧判断"的,中断喂流会打断检测——半双工要用 `disable/enable_wakenet` + `reset_buffer`。
6. `wakenet_mode` 的 90/95 与 2CH/3CH 会同时影响内存与误唤醒率,换档后要重新测误唤醒。
7. `afe_config_check` 会**改**你的配置(比如两通道下 SE 优先于 NS、SE 关了只用第一路麦克风),所以 `print_pipeline()` 的打印才是真相。
8. `vad_cache` 是 AFE 自带的"补回被截断语音"机制,配合 `vad_delay_ms` 使用;你自己的 pre-roll 别和它重复做同一件事。
9. `data_volume` 是 AGC **之前**的能量,和你自己算的 RMS/dBFS 口径不完全一样,做对比时要注意。
10. `ret_value` 没有公开的取值枚举,第一次接入先打印几帧观察。

## 10. 速查:我该用哪个

| 我想做的事 | 用哪个 |
|---|---|
| 挂载模型分区 | `esp_srmodel_init` |
| 找唤醒词模型 | `esp_srmodel_filter(models, "wn", NULL)` |
| 建实例 | `afe_config_init` → `esp_afe_handle_from_config` → `create_from_config` |
| 确认配置生效 | `afe_config_print` + `print_pipeline` |
| 喂数据 | `feed`(长度 = chunksize x 通道数) |
| 取结果 | `fetch` |
| 知道要攒多少点 | `get_feed_chunksize` / `get_fetch_chunksize` |
| 播放提示音时暂停唤醒 | `disable_wakenet` / `enable_wakenet` / `reset_buffer` |
| 唤醒太灵敏/太迟钝 | `set_wakenet_threshold` |
| 加第二个唤醒词 | `add_wakenet_model`(最多 3 个) |
| 运行时关掉某个算法 | `disable_aec/se/ns/vad/agc` |
| 释放 | `destroy` + `esp_srmodel_deinit` |
