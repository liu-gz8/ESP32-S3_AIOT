#!/usr/bin/env python3
"""WAV -> 16kHz / 16bit / 单声道 原始 PCM(s16le),面向设备提示音。

自带语音优化处理链(见 audio_dsp.py):
    去直流 -> 裁首尾静音 -> 高通(去无用低频)-> 2~4kHz 存在感提升
    -> 淡入淡出 -> 感知响度(LUFS)归一化 -> 峰值限制 -> 首尾静音

常用示例:
    # 人声提示音(推荐参数)
    python wav_to_pcm.py in.wav data/wake.pcm --trim-db -40 --highpass 200 \
           --presence 4 --lufs -16 --rate 16000
    # 只有峰值归一化(默认)
    python wav_to_pcm.py in.wav out.pcm

依赖:numpy(不需要 ffmpeg/sox)。
"""

import argparse
import os
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_dsp  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description="WAV -> 16k/16bit/mono raw PCM(含语音优化)")
    ap.add_argument("src", help="输入 WAV(16bit PCM)")
    ap.add_argument("dst", help="输出 PCM(s16le)")
    ap.add_argument("--rate", type=int, default=16000, help="目标采样率(默认 16000)")
    ap.add_argument("--mono", choices=("mean", "left", "right"), default="mean",
                    help="多声道下混方式(默认 mean)")
    ap.add_argument("--peak-db", type=float, default=-1.0, help="峰值上限 dBFS(默认 -1)")
    ap.add_argument("--lufs", type=float, default=None,
                    help="按目标感知响度归一化,人声建议 -16;不填则只按峰值归一化")
    ap.add_argument("--highpass", type=float, default=0.0,
                    help="高通截止频率 Hz,人声建议 200(0=不启用)")
    ap.add_argument("--presence", type=float, default=0.0,
                    help="2~4kHz 存在感提升 dB,人声建议 +4(0=不启用)")
    ap.add_argument("--presence-hz", type=float, default=3000.0, help="存在感中心频率(默认 3000)")
    ap.add_argument("--trim-db", type=float, default=None,
                    help="裁掉首尾低于该电平的静音,例如 -40;默认不裁")
    ap.add_argument("--fade-ms", type=float, default=5.0, help="淡入淡出 ms(默认 5)")
    ap.add_argument("--pad-head-ms", type=float, default=100.0, help="开头静音 ms(默认 100)")
    ap.add_argument("--pad-tail-ms", type=float, default=150.0, help="结尾静音 ms(默认 150)")
    args = ap.parse_args()

    with wave.open(args.src, "rb") as w:
        nch, sw, sr, nfr = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(nfr)

    if sw != 2:
        print(f"错误:仅支持 16bit PCM WAV(当前 sampwidth={sw})", file=sys.stderr)
        return 2
    if nfr == 0:
        print("错误:输入为空", file=sys.stderr)
        return 2

    x = np.frombuffer(raw, dtype="<i2").astype(np.float32)
    if nch > 1:
        x = x.reshape(-1, nch)
        if args.mono == "left":
            x = x[:, 0]
        elif args.mono == "right":
            x = x[:, min(1, nch - 1)]
        else:
            x = x.mean(axis=1)

    print(f"输入: {sr} Hz / {nch} ch / {nfr} frames / {nfr / sr:.2f}s")

    x = audio_dsp.resample(x, sr, args.rate)
    y = audio_dsp.process_pcm(
        x, args.rate,
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

    samples = np.clip(y, -32768.0, 32767.0).astype("<i2")
    samples.tofile(args.dst)

    duration = len(samples) / float(args.rate)
    peak = int(np.max(np.abs(samples.astype(np.int32)))) if len(samples) else 0
    lufs = audio_dsp.measure_lufs(samples.astype(np.float64), args.rate)
    print(f"输出: {args.dst}")
    print(f"      {len(samples)} samples / {duration:.2f}s / {len(samples) * 2} bytes / "
          f"peak={peak} ({20 * np.log10(max(peak, 1) / 32767):.1f} dBFS) / ~{lufs:.1f} LUFS")
    if duration > 2.0:
        print("提示: 超过 2 秒,建议缩短(减少延迟与功耗)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
