#!/usr/bin/env python3
"""用微软 Edge-TTS 生成提示音 PCM(16kHz/16bit/单声道/s16le),自带语音优化。

处理链:
    Edge-TTS -> mp3 -> 16k 单声道 -> 去直流 -> 裁静音 -> 高通(200Hz)
    -> 2~4kHz 存在感提升 -> 淡入淡出 -> LUFS 归一化(-16)-> 峰值限制(-1dBFS)
    -> 头 100ms / 尾 150ms 静音

常用示例:
    # 单条(人声优化默认参数)
    python tts_to_pcm.py --name wake --text "在呢"
    # 换音色/语速
    python tts_to_pcm.py --name wake --text "在呢" --voice zh-CN-YunxiNeural --rate -10%
    # 整套预设
    python tts_to_pcm.py --preset all

依赖:edge-tts(联网合成)、PyAV(解码)、numpy(后处理)。
"""

import argparse
import asyncio
import os
import sys
import tempfile

import av
import edge_tts
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_dsp  # noqa: E402

DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"

PRESETS = {
    "wake":   "在呢",
    "ok":     "识别成功",
    "fail":   "没听清,请再说一遍",
    "lowbat": "电量不足,请充电",
    "error":  "设备异常",
}


def synth(text: str, voice: str, out_mp3: str, rate: str, volume: str) -> None:
    async def _run() -> None:
        communicate = edge_tts.Communicate(text, voice, rate=rate, volume=volume)
        await communicate.save(out_mp3)

    asyncio.run(_run())


def decode_mp3(src: str, out_rate: int) -> np.ndarray:
    """解码成 16k 单声道 float32 样本。"""
    chunks = []
    with av.open(src) as container:            # with 确保释放句柄(Windows 关键)
        stream = next((s for s in container.streams if s.type == "audio"), None)
        if stream is None:
            raise RuntimeError("没有音频流")
        resampler = av.AudioResampler(format="s16", layout="mono", rate=out_rate)
        for frame in container.decode(stream):
            for out_frame in resampler.resample(frame):
                chunks.append(bytes(out_frame.planes[0]))
        for out_frame in resampler.resample(None):
            chunks.append(bytes(out_frame.planes[0]))

    data = b"".join(chunks)
    if not data:
        raise RuntimeError("解码结果为空")
    return np.frombuffer(data, dtype="<i2").astype(np.float32)


def build_one(name: str, text: str, args) -> None:
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        mp3 = os.path.join(tmp, f"{name}.mp3")
        synth(text, args.voice, mp3, args.rate, args.volume)
        x = decode_mp3(mp3, args.sample_rate)

    y = audio_dsp.process_pcm(
        x, args.sample_rate,
        highpass_hz=args.highpass,
        presence_db=args.presence,
        presence_hz=args.presence_hz,
        target_lufs=args.lufs,
        peak_db=args.peak_db,
        trim_db=args.trim_db,
        fade_ms=args.fade_ms,
        pad_head_ms=args.pad_head_ms,
        pad_tail_ms=args.pad_tail_ms,
    )

    dst = os.path.join(args.outdir, f"{name}.pcm")
    samples = np.clip(y, -32768.0, 32767.0).astype("<i2")
    samples.tofile(dst)

    duration = len(samples) / float(args.sample_rate)
    peak = int(np.max(np.abs(samples.astype(np.int32)))) if len(samples) else 0
    lufs = audio_dsp.measure_lufs(samples.astype(np.float64), args.sample_rate)
    print(f"{dst}: {len(samples)} samples / {duration:.2f}s / {len(samples) * 2} bytes / "
          f"peak={peak} ({20 * np.log10(max(peak, 1) / 32767):.1f} dBFS) / ~{lufs:.1f} LUFS"
          f"  <- \"{text}\" [{args.voice}]")


def main() -> int:
    ap = argparse.ArgumentParser(description="Edge-TTS -> 16k/16bit/mono PCM(含语音优化)")
    ap.add_argument("--name", help="输出文件名(不含扩展名),如 wake")
    ap.add_argument("--text", help="合成文本")
    ap.add_argument("--preset", choices=list(PRESETS) + ["all"], help="使用预设文本")
    ap.add_argument("--voice", default=DEFAULT_VOICE, help=f"音色(默认 {DEFAULT_VOICE})")
    ap.add_argument("--rate", default="+0%", help="语速,如 +10%% / -10%%(建议 -10%%)")
    ap.add_argument("--volume", default="+0%", help="音量(默认 +0%%)")
    ap.add_argument("--outdir", default="data", help="输出目录(默认 data)")
    ap.add_argument("--sample-rate", type=int, default=16000)
    ap.add_argument("--peak-db", type=float, default=-1.0, help="峰值上限(默认 -1dBFS)")
    ap.add_argument("--lufs", type=float, default=-16.0, help="目标感知响度(默认 -16 LUFS)")
    ap.add_argument("--highpass", type=float, default=200.0, help="高通 Hz(默认 200,0=关闭)")
    ap.add_argument("--presence", type=float, default=4.0, help="2~4kHz 提升 dB(默认 +4)")
    ap.add_argument("--presence-hz", type=float, default=3000.0)
    ap.add_argument("--trim-db", type=float, default=-40.0, help="裁静音阈值(默认 -40dBFS)")
    ap.add_argument("--fade-ms", type=float, default=5.0)
    ap.add_argument("--pad-head-ms", type=float, default=100.0)
    ap.add_argument("--pad-tail-ms", type=float, default=150.0)
    args = ap.parse_args()

    if not args.preset and not (args.name and args.text):
        ap.error("需要 --preset 或 (--name + --text)")

    os.makedirs(args.outdir, exist_ok=True)

    if args.preset == "all":
        for name, text in PRESETS.items():
            build_one(name, text, args)
    elif args.preset:
        build_one(args.preset, PRESETS[args.preset], args)
    else:
        build_one(args.name, args.text, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
