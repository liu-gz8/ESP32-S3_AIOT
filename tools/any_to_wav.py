#!/usr/bin/env python3
"""任意音频格式 -> 16kHz/16bit/单声道 WAV(通过 PyAV/FFmpeg 解码)。

用法:
    python any_to_wav.py input.webm out.wav [--rate 16000]

支持的输入取决于 FFmpeg(常见:webm/opus、mp3、m4a/aac、wav、flac、ogg 等)。
依赖:PyAV( pip install av )

转换完成后再用 wav_to_pcm.py 生成设备用的 .pcm,或直接用 wav_to_pcm.py 处理 WAV。
"""

import argparse
import sys
import wave

import av
import numpy as np


def main() -> int:
    ap = argparse.ArgumentParser(description="任意音频 -> 16k/16bit/mono WAV")
    ap.add_argument("src", help="输入音频(webm/opus/mp3/m4a/flac/ogg/wav...)")
    ap.add_argument("dst", help="输出 WAV")
    ap.add_argument("--rate", type=int, default=16000, help="目标采样率(默认 16000)")
    args = ap.parse_args()

    try:
        container = av.open(args.src)
    except Exception as exc:                     # noqa: BLE001
        print(f"打开失败: {exc}", file=sys.stderr)
        return 2

    stream = next((s for s in container.streams if s.type == "audio"), None)
    if stream is None:
        print("输入中没有音频流", file=sys.stderr)
        return 2

    resampler = av.AudioResampler(format="s16", layout="mono", rate=args.rate)
    chunks = []

    for frame in container.decode(stream):
        for out_frame in resampler.resample(frame):
            chunks.append(bytes(out_frame.planes[0]))
    for out_frame in resampler.resample(None):   # flush
        chunks.append(bytes(out_frame.planes[0]))

    data = b"".join(chunks)
    samples = np.frombuffer(data, dtype="<i2")
    if samples.size == 0:
        print("解码结果为空", file=sys.stderr)
        return 2

    with wave.open(args.dst, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(args.rate)
        w.writeframes(samples.tobytes())

    duration = samples.size / float(args.rate)
    peak = int(np.max(np.abs(samples.astype(np.int32))))
    print(f"输入: {args.src}")
    print(f"输出: {args.dst}  {samples.size} samples / {duration:.2f}s @ {args.rate}Hz / peak={peak}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
