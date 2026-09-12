#!/usr/bin/env python3
"""提示音 DSP 处理(仅依赖 numpy)。

提供:
    highpass()            高通滤波(去掉小喇叭放不出来的低频)
    presence_boost()      2~4kHz 存在感提升(提升语音可懂度)
    measure_lufs()        近似 ITU-R BS.1770 / EBU R128 的感知响度
    normalize_lufs()      按目标 LUFS 归一化,并限制峰值
    process_pcm()         完整处理链:去直流 -> 裁静音 -> 滤波 -> 淡入淡出 -> 响度归一化 -> 峰值限制 -> 首尾静音

说明:滤波与加权都在频域完成(零相位),对短提示音足够精确且实现简单。
LUFS 为工程近似值(K 加权 + 简化门限),用于让多条提示音"听起来一样响",
不等同于实验室级 BS.1770 测量。
"""

import numpy as np


def _rfft_freqs(n: int, sr: int) -> np.ndarray:
    return np.fft.rfftfreq(n, d=1.0 / sr)


def _apply_magnitude(x: np.ndarray, sr: int, mag_fn) -> np.ndarray:
    """在频域按 |H(f)| 滤波(零相位)。"""
    n = len(x)
    if n == 0:
        return x
    f = np.maximum(_rfft_freqs(n, sr), 1e-6)
    spec = np.fft.rfft(x)
    return np.fft.irfft(spec * mag_fn(f), n=n)


def highpass(x: np.ndarray, sr: int, fc: float, order: int = 4) -> np.ndarray:
    """Butterworth 高通(频域幅度响应)。"""
    fc = float(fc)
    if fc <= 0:
        return x

    def mag(f):
        return 1.0 / np.sqrt(1.0 + (fc / f) ** (2 * order))

    return _apply_magnitude(x, sr, mag)


def lowpass(x: np.ndarray, sr: int, fc: float, order: int = 4) -> np.ndarray:
    fc = float(fc)
    if fc <= 0:
        return x

    def mag(f):
        return 1.0 / np.sqrt(1.0 + (f / fc) ** (2 * order))

    return _apply_magnitude(x, sr, mag)


def presence_boost(x: np.ndarray, sr: int, gain_db: float,
                   center_hz: float = 3000.0, width_oct: float = 1.2) -> np.ndarray:
    """以 center_hz 为中心的钟形提升(对数频率轴上的高斯)。"""
    if gain_db == 0:
        return x
    g = 10.0 ** (gain_db / 20.0)

    def mag(f):
        oct_dist = np.log2(f / center_hz) / width_oct
        return 1.0 + (g - 1.0) * np.exp(-0.5 * oct_dist ** 2)

    return _apply_magnitude(x, sr, mag)


def resample(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    """线性重采样;降采样前先低通抗混叠。"""
    if sr_in == sr_out or len(x) == 0:
        return x
    y = x
    if sr_out < sr_in:
        y = lowpass(y, sr_in, 0.45 * sr_out, order=6)
    t_in = np.arange(len(y)) / float(sr_in)
    n_out = int(round(len(y) * sr_out / float(sr_in)))
    t_out = np.arange(n_out) / float(sr_out)
    return np.interp(t_out, t_in, y)


def _k_weighting_mag(f: np.ndarray) -> np.ndarray:
    """K 加权近似:38Hz 高通 + 约 2kHz 以上 +4dB 高频搁架。"""
    hp = 1.0 / np.sqrt(1.0 + (38.0 / np.maximum(f, 1e-6)) ** 4)
    shelf = 1.0 + (10.0 ** (4.0 / 20.0) - 1.0) * (1.0 / (1.0 + (1500.0 / np.maximum(f, 1e-6)) ** 2))
    return hp * shelf


def measure_lufs(x: np.ndarray, sr: int) -> float:
    """近似 LUFS(相对值):K 加权后的均方,按 400ms 分块 + -10dB 相对门限。"""
    if len(x) == 0:
        return -70.0

    weighted = _apply_magnitude(x.astype(np.float64), sr, _k_weighting_mag)
    block = max(1, int(sr * 0.4))
    step = max(1, block // 4)
    powers = []
    for start in range(0, max(1, len(weighted) - block + 1), step):
        seg = weighted[start:start + block]
        if len(seg) == 0:
            continue
        powers.append(float(np.mean(seg ** 2)))
    if not powers:
        powers = [float(np.mean(weighted ** 2))]

    powers = np.array(powers)
    mean_power = float(np.mean(powers))
    if mean_power <= 0:
        return -70.0
    gate = mean_power * (10.0 ** (-10.0 / 10.0))     # 相对门限 -10 dB
    kept = powers[powers >= gate]
    if kept.size == 0:
        kept = powers
    power = float(np.mean(kept))
    if power <= 0:
        return -70.0
    return -0.691 + 10.0 * np.log10(power / (32767.0 ** 2))


def normalize_lufs(x: np.ndarray, sr: int, target_lufs: float,
                   peak_ceiling_db: float = -1.0) -> np.ndarray:
    """按目标 LUFS 归一化;若超过峰值上限则整体回退(硬限制,不削顶)。"""
    current = measure_lufs(x, sr)
    gain = 10.0 ** ((target_lufs - current) / 20.0)
    y = x * gain

    ceiling = (10.0 ** (peak_ceiling_db / 20.0)) * 32767.0
    peak = float(np.max(np.abs(y))) if len(y) else 0.0
    if peak > ceiling and peak > 0:
        y *= ceiling / peak
    return y


def trim_silence(x: np.ndarray, sr: int, threshold_db: float) -> np.ndarray:
    if threshold_db is None or len(x) == 0:
        return x
    thr = (10.0 ** (threshold_db / 20.0)) * 32767.0
    idx = np.where(np.abs(x) > thr)[0]
    if idx.size == 0:
        return x
    return x[idx[0]: idx[-1] + 1]


def process_pcm(x: np.ndarray, sr: int,
                highpass_hz: float = 0.0,
                presence_db: float = 0.0,
                presence_hz: float = 3000.0,
                target_lufs: float = None,
                peak_db: float = -1.0,
                trim_db: float = None,
                fade_ms: float = 5.0,
                pad_head_ms: float = 100.0,
                pad_tail_ms: float = 150.0) -> np.ndarray:
    """完整处理链,输入/输出均为 float32 的 int16 幅度样本。"""
    y = x.astype(np.float64)
    if len(y) == 0:
        return y.astype(np.float32)

    y = y - float(np.mean(y))                      # 去直流
    y = trim_silence(y, sr, trim_db)               # 裁首尾静音

    if highpass_hz:
        y = highpass(y, sr, highpass_hz)
    if presence_db:
        y = presence_boost(y, sr, presence_db, presence_hz)

    fade = int(sr * fade_ms / 1000.0)
    if fade > 1 and len(y) > 2 * fade:
        y[:fade] *= np.linspace(0.0, 1.0, fade)
        y[-fade:] *= np.linspace(1.0, 0.0, fade)

    if target_lufs is not None:
        y = normalize_lufs(y, sr, target_lufs, peak_db)
    else:
        peak = float(np.max(np.abs(y))) or 1.0
        y = y * ((10.0 ** (peak_db / 20.0) * 32767.0) / peak)

    head = np.zeros(int(sr * pad_head_ms / 1000.0))
    tail = np.zeros(int(sr * pad_tail_ms / 1000.0))
    y = np.concatenate([head, y, tail])
    return np.clip(y, -32768.0, 32767.0).astype(np.float32)
