"""程序合成一段原创背景音乐（无第三方素材、无版权问题），输出 44.1k 立体声 WAV。

用法： python music.py <时长秒> <输出.wav>
风格：92 BPM，Am7 - Fmaj7 - Cmaj7 - G6 循环；柔和 pad + 琶音 + 轻低音 + 极简打击。
"""
import sys
import wave
import numpy as np

SR = 44100
BPM = 92.0
BEAT = 60.0 / BPM
BAR = 4 * BEAT


def midi(n):
    return 440.0 * 2 ** ((n - 69) / 12.0)


def adsr(n, a, d, s, r, sustain_level=0.7):
    """采样点数的包络。"""
    t = np.arange(n) / SR
    env = np.ones(n) * sustain_level
    ai = int(a * SR)
    di = int(d * SR)
    ri = int(r * SR)
    if ai > 0:
        env[:ai] = np.linspace(0, 1, ai)
    if di > 0:
        end = min(n, ai + di)
        env[ai:end] = np.linspace(1, sustain_level, end - ai)
    if ri > 0 and n > ri:
        env[-ri:] *= np.linspace(1, 0, ri)
    return env


def tone(freq, dur, kind="pad", detune=0.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    if kind == "pad":
        env = adsr(n, 0.9, 0.6, 0.0, 0.9, 0.85)
        w = np.sin(2 * np.pi * freq * t) * 0.6
        w += np.sin(2 * np.pi * freq * (1 + 0.004) * t) * 0.25
        w += np.sin(2 * np.pi * freq * (1 - 0.004) * t) * 0.25
        w += np.sin(2 * np.pi * freq * 2 * t) * 0.06
    elif kind == "pluck":
        env = adsr(n, 0.004, 0.30, 0.0, 0.05, 0.0)
        w = np.sin(2 * np.pi * freq * t) * 0.7
        w += np.sin(2 * np.pi * freq * 3 * t) * 0.12
        w *= np.exp(-3.2 * t)
    elif kind == "bass":
        env = adsr(n, 0.01, 0.5, 0.0, 0.2, 0.4)
        w = np.sin(2 * np.pi * freq * t) * 0.9
        w += np.sin(2 * np.pi * freq * 2 * t) * 0.1
    else:  # bell
        env = adsr(n, 0.002, 0.9, 0.0, 0.1, 0.0)
        w = np.sin(2 * np.pi * freq * t) * 0.6 + np.sin(2 * np.pi * freq * 2.76 * t) * 0.15
        w *= np.exp(-2.0 * t)
    if detune:
        w += np.sin(2 * np.pi * freq * (1 + detune) * t) * 0.2
    return w * env


def add(buf, sig, at):
    i = int(at * SR)
    j = min(len(buf), i + len(sig))
    if i < len(buf):
        buf[i:j] += sig[: j - i]


def kick(dur=0.30):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = 110 * np.exp(-22 * t) + 42
    ph = 2 * np.pi * np.cumsum(f) / SR
    return np.sin(ph) * np.exp(-7.5 * t) * 0.9


def hat(dur=0.05):
    n = int(dur * SR)
    rng = np.random.default_rng(7)
    x = rng.standard_normal(n)
    x = np.diff(np.concatenate([[0.0], x]))  # 简易高通
    return x * np.exp(-45 * np.arange(n) / SR) * 0.16


def snare(dur=0.16):
    n = int(dur * SR)
    rng = np.random.default_rng(11)
    x = rng.standard_normal(n)
    x = np.diff(np.concatenate([[0.0], x]))
    return x * np.exp(-22 * np.arange(n) / SR) * 0.22


def main():
    dur = float(sys.argv[1])
    out = sys.argv[2]
    n = int(dur * SR)
    left = np.zeros(n)
    right = np.zeros(n)

    prog = [
        (57, [57, 60, 64, 67]),   # Am7
        (53, [53, 57, 60, 64]),   # Fmaj7
        (48, [60, 64, 67, 71]),   # Cmaj7
        (55, [55, 59, 62, 64]),   # G6
    ]
    bars = int(np.ceil(dur / BAR)) + 1

    for b in range(bars):
        t0 = b * BAR
        if t0 > dur:
            break
        root, chord = prog[b % 4]
        # pad
        for k, note in enumerate(chord):
            sig = tone(midi(note), BAR * 0.98, "pad")
            pan = 0.5 + 0.18 * (k - 1.5) / 1.5
            add(left, sig * (1 - pan) * 0.30, t0)
            add(right, sig * pan * 0.30, t0)
        # bass
        for bt in (0, 2):
            add(left, tone(midi(root - 12), BEAT * 1.6, "bass") * 0.30, t0 + bt * BEAT)
            add(right, tone(midi(root - 12), BEAT * 1.6, "bass") * 0.30, t0 + bt * BEAT)
        # 琶音：八分音符
        steps = [0, 1, 2, 3, 2, 1, 3, 2]
        for i, si in enumerate(steps):
            note = chord[si] + (12 if (i % 4 == 3) else 0)
            at = t0 + i * BEAT / 2
            sig = tone(midi(note), BEAT * 0.9, "pluck") * 0.20
            add(left, sig * 0.85, at + 0.012)
            add(right, sig * 0.55, at)
        # 打击
        for bt in (0, 2):
            k = kick() * 0.35
            add(left, k, t0 + bt * BEAT)
            add(right, k, t0 + bt * BEAT)
        for i in range(8):
            if i % 2 == 1:
                add(left, hat() * 0.7, t0 + i * BEAT / 2)
                add(right, hat(), t0 + i * BEAT / 2)
        add(left, snare() * 0.6, t0 + 2 * BEAT)
        add(right, snare() * 0.6, t0 + 2 * BEAT)
        # 每隔两小节一根铃声点缀
        if b % 4 == 3:
            add(left, tone(midi(chord[3] + 12), BEAT * 2, "bell") * 0.18, t0 + 1.5 * BEAT)
            add(right, tone(midi(chord[2] + 12), BEAT * 2, "bell") * 0.18, t0 + 2.5 * BEAT)

    # 简易立体声延迟（右侧）
    d = int(0.19 * SR)
    right[d:] += left[:-d] * 0.18
    left[d:] += right[:-d] * 0.10

    mix = np.stack([left, right])
    # 淡化头尾
    fin = int(2.2 * SR)
    fout = int(3.4 * SR)
    mix[:, :fin] *= np.linspace(0, 1, fin)
    mix[:, -fout:] *= np.linspace(1, 0, fout)
    # 柔和限幅 + 归一
    mix = np.tanh(mix * 1.15)
    peak = np.max(np.abs(mix))
    if peak > 0:
        mix = mix / peak * 0.89

    data = (mix.T * 32767).astype("<i2")
    with wave.open(out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(data.tobytes())
    print(f"music: {dur:.2f}s -> {out}")


if __name__ == "__main__":
    main()
