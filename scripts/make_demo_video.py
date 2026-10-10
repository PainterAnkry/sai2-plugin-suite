# -*- coding: utf-8 -*-
"""把 demo 帧序列（BMP）合成功能演示视频。

产物：
  sai2_功能演示.avi   —— MJPEG 编码 AVI（Windows 自带播放器可直接放）
  sai2_功能演示_预览.gif —— 小尺寸 GIF 预览

用法: python make_demo_video.py <frames_dir> <out_avi> <out_gif> [fps]
帧率默认 6.7（对应 Demo.cpp 的 150ms/帧）。
"""
import glob
import io
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------- 字幕时间线
# (起始帧, 结束帧, 文案)；帧号按 Demo.cpp 时间线估算
SCENES = [
    (0,   16,  "① 主面板 —— 自动抓取 SAI2 当前图层"),
    (16,  48,  "② 模糊滤镜 —— 参数滑杆实时预览"),
    (48,  60,  "③ 应用 —— 结果写回剪贴板 / 主面板"),
    (60,  118, "④ 液化 —— 向前变形 / 膨胀 / 旋转，画笔涂抹"),
    (118, 128, "⑤ 应用液化结果"),
    (128, 149, "⑥ 滤镜库 —— 水彩 / 素描 一键切换"),
    (149, 184, "⑦ 调整 —— 反相 / 去色 / 亮度对比度 / 色相"),
    (184, 189, "⑧ 回到主面板"),
    (189, 199, "⑨ 深色主题 —— 一键切换，全窗口跟随"),
    (199, 210, "⑩ 功能窗口同步深色"),
    (210, 10 ** 9, "SAI2 插件系统 · 完整链路：抓取 → 处理 → 应用 → 回贴"),
]


def load_font(size):
    for p in (r"C:\Windows\Fonts\msyhbd.ttc", r"C:\Windows\Fonts\msyh.ttc",
              r"C:\Windows\Fonts\simhei.ttf"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def make_title(w, h):
    im = Image.new("RGB", (w, h), (24, 26, 32))
    d = ImageDraw.Draw(im)
    f1, f2 = load_font(64), load_font(30)
    t1 = "SAI2 插件系统"
    t2 = "功能演示 · 抓取图层 → 滤镜 / 液化 / 调整 → 应用回贴"
    tw = d.textlength(t1, font=f1)
    d.text(((w - tw) / 2, h * 0.40 - 40), t1, font=f1, fill=(235, 238, 245))
    tw = d.textlength(t2, font=f2)
    d.text(((w - tw) / 2, h * 0.40 + 50), t2, font=f2, fill=(140, 150, 170))
    # 顶部强调条
    d.rectangle([0, 0, w, 10], fill=(10, 132, 255))
    return im


def caption_frame(im, text, fcap):
    d = ImageDraw.Draw(im, "RGBA")
    w, h = im.size
    tw = d.textlength(text, font=fcap)
    pad, bh = 18, 52
    x0, y0 = (w - tw) / 2 - pad, h - bh - 14
    d.rounded_rectangle([x0, y0, x0 + tw + pad * 2, y0 + bh], radius=14,
                        fill=(20, 22, 28, 215))
    d.text((x0 + pad, y0 + (bh - 30) / 2 - 2), text, font=fcap,
           fill=(240, 242, 248))
    return im


def jpeg_bytes(im, q=88):
    buf = io.BytesIO()
    im.convert("RGB").save(buf, "JPEG", quality=q)
    return buf.getvalue()


def mux_mjpeg_avi(jpegs, w, h, fps, path):
    """手写 MJPEG AVI（RIFF），Windows 播放器直接可放。"""
    # fps 精确到分数（例如 6.67 -> scale=3 rate=20）
    from fractions import Fraction
    fr = Fraction(fps).limit_denominator(1000)
    dwScale, dwRate = fr.denominator, fr.numerator

    n = len(jpegs)
    sizes = [len(j) for j in jpegs]
    max_len = max(sizes)

    def chunk(fourcc, data):
        pad = b"\x00" if len(data) % 2 else b""
        return fourcc + struct.pack("<I", len(data)) + data + pad

    # AVIMAINHEADER（56 字节 = 14 个 DWORD）
    avih = struct.pack(
        "<14I",
        int(1e6 / fps),      # dwMicroSecPerFrame
        max_len * 2,         # dwMaxBytesPerSec
        0,                   # dwPaddingGranularity
        0x10,                # dwFlags: AVIF_HASINDEX
        n,                   # dwTotalFrames
        0,                   # dwInitialFrames
        1,                   # dwStreams
        max_len,             # dwSuggestedBufferSize
        w, h,                # dwWidth/dwHeight
        0, 0, 0, 0)          # dwReserved[4]
    # AVISTREAMHEADER（56 字节）：type,handler,flags,prio,lang,init,scale,
    #   rate,start,length,bufsize,quality,samplesize,rcFrame(8)
    strh = struct.pack("<4s4sIHH8I", b"vids", b"MJPG", 0, 0, 0,
                       0, dwScale, dwRate, 0, n, max_len, 10000, 0)
    strh += struct.pack("<4H", 0, 0, 0, 0)  # rcFrame
    strf = struct.pack(
        "<IiiHH4sIIII", 40, w, h, 1, 24, b"MJPG", max_len, 0, 0, 0)
    strl = chunk(b"LIST", b"strl" + chunk(b"strh", strh) +
                 chunk(b"strf", strf))
    hdrl = chunk(b"LIST", b"hdrl" + chunk(b"avih", avih) + strl)

    movi_body = b"movi"
    index = b""
    offset = 4  # 相对 'movi' 起始（含 'movi' 四字符后）
    for j in jpegs:
        data = chunk(b"00dc", j)
        movi_body += data
        index += b"00dc" + struct.pack("<II", 0x10, offset + 8)
        offset += len(data)
    movi = chunk(b"LIST", movi_body)
    idx1 = chunk(b"idx1", index)

    riff_body = b"AVI " + hdrl + movi + idx1
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(riff_body)) + riff_body)


def main():
    frames_dir, out_avi, out_gif = sys.argv[1], sys.argv[2], sys.argv[3]
    fps = float(sys.argv[4]) if len(sys.argv) > 4 else 1000.0 / 150.0
    files = sorted(glob.glob(os.path.join(frames_dir, "frame_*.bmp")))
    if not files:
        print("no frames in", frames_dir)
        return 1
    W = H = 900
    fcap = load_font(30)

    # 标题卡放最前（1.8s）
    title = make_title(W, H)
    title_frames = int(fps * 1.8)

    jpegs = [jpeg_bytes(title, 90)] * title_frames
    gif_frames = []
    gif_every = 3  # GIF 降采样
    gif_idx = 0

    for i, fp in enumerate(files):
        im = Image.open(fp).convert("RGB")
        text = next((t for a, b, t in SCENES if a <= i < b), "")
        if text:
            im = caption_frame(im, text, fcap)
        if i % gif_every == 0:
            g = im.resize((450, 450), Image.LANCZOS)
            gif_frames.append(g)
        jpegs.append(jpeg_bytes(im))
        if i % 40 == 0:
            print(f"  {i}/{len(files)}")

    # GIF 首帧换成标题
    gif_frames.insert(0, make_title(450, 450).resize((450, 450)))
    mux_mjpeg_avi(jpegs, W, H, fps, out_avi)
    print("avi:", out_avi, os.path.getsize(out_avi) // 1024, "KB,",
          len(jpegs), "frames")

    gif_frames[0].save(out_gif, save_all=True, append_images=gif_frames[1:],
                       duration=int(1000 * gif_every / fps), loop=0,
                       optimize=True)
    print("gif:", out_gif, os.path.getsize(out_gif) // 1024, "KB,",
          len(gif_frames), "frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
