#!/usr/bin/env python3
"""生成设备提示音 PCM(16kHz / 16bit / 单声道 / s16le)。

全部用正弦合成,不依赖任何外部素材,**没有版权问题**。

用法:
    python make_beeps.py                 # 输出到 data/
    python make_beeps.py --outdir data

生成(每个状态的音型互相有区分度,均归一化到 -1dBFS):
    wake.pcm    唤醒应答:上行双音 1318 -> 1760Hz(明亮)
    ok.pcm      成功:上行双音 880 -> 1320Hz
    fail.pcm    失败:下行双音 660 -> 494Hz
    lowbat.pcm  低电量:三声下行 988 -> 784 -> 587Hz
    error.pcm   错误:两声低音 392Hz
"""

import argparse
import os
import sys

import numpy as np

SR = 16000


def tone(freq_hz: float, ms: float, amp: float = 0.6, fade_ms: float = 5.0) -> np.ndarray:
    n = int(SR * ms / 1000.0)
    t = np.arange(n) / float(SR)
    x = np.sin(2.0 * np.pi * freq_hz * t) * amp
    f = max(1, int(SR * fade_ms / 1000.0))
    if n > 2 * f:
        x[:f] *= np.linspace(0.0, 1.0, f, dtype=np.float32)
        x[-f:] *= np.linspace(1.0, 0.0, f, dtype=np.float32)
    return x.astype(np.float32)


def silence(ms: float) -> np.ndarray:
    return np.zeros(int(SR * ms / 1000.0), dtype=np.float32)


def save(path: str, segments, peak_db: float = -1.0) -> None:
    """合成并归一化到指定峰值(默认 -1dBFS),保证与语音提示音响度接近。"""
    y = np.concatenate(segments).astype(np.float64)
    peak = float(np.max(np.abs(y))) or 1.0
    y = y * ((10.0 ** (peak_db / 20.0) * 32767.0) / peak)
    y = np.clip(y, -32768.0, 32767.0).astype("<i2")
    y.tofile(path)
    peak = int(np.max(np.abs(y.astype(np.int32)))) if len(y) else 0
    print(f"{path}: {len(y)} samples / {len(y) / SR:.2f}s / {len(y) * 2} bytes / peak={peak}")


def main() -> int:
    ap = argparse.ArgumentParser(description="生成提示音 PCM")
    ap.add_argument("--outdir", default="data", help="输出目录(默认 data)")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    p = lambda name: os.path.join(args.outdir, name)  # noqa: E731

    # 唤醒应答:明亮的上行双音(1318Hz=E6 -> 1760Hz=A6)
    save(p("wake.pcm"), [
        silence(30),
        tone(1318, 70, 0.6), silence(30),
        tone(1760, 110, 0.6),
        silence(80),
    ])

    # 成功:上行双音(880Hz=A5 -> 1320Hz=E6)
    save(p("ok.pcm"), [
        silence(30),
        tone(880, 70, 0.6), silence(30),
        tone(1320, 110, 0.6),
        silence(80),
    ])

    # 失败:下行双音(660Hz=E5 -> 494Hz=B4)
    save(p("fail.pcm"), [
        silence(30),
        tone(660, 90, 0.6), silence(30),
        tone(494, 150, 0.6),
        silence(80),
    ])

    # 低电量:三声下行(988 -> 784 -> 587Hz)
    save(p("lowbat.pcm"), [
        silence(30),
        tone(988, 90, 0.6), silence(50),
        tone(784, 90, 0.6), silence(50),
        tone(587, 140, 0.6),
        silence(100),
    ])

    # 错误:两声低音(392Hz=G4)
    save(p("error.pcm"), [
        silence(30),
        tone(392, 120, 0.6), silence(50),
        tone(392, 180, 0.6),
        silence(100),
    ])

    return 0


if __name__ == "__main__":
    sys.exit(main())
