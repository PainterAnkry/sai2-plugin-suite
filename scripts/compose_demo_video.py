"""把抓帧序列合成为带标题卡/字幕/进度条的视频帧序列（1280x720）。

用法： python compose.py <frames_dir> <out_dir>
产出： out_dir/f_%05d.png + out_dir/list.txt（ffmpeg concat 清单，含每帧时长）

特性：
  * 场景可指定裁剪框（crop）——液化场景裁到工具列 + 画布，形变看得更清楚；
  * 场景可指定 split=N ——末尾 N 帧显示「原图 / 处理后」并排对比；
  * 自动丢弃末尾的全黑帧（演示退出时抓到的空屏）。
"""
import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

CANVAS = (1280, 720)
MATTE = (247, 246, 246)
BG_TOP, BG_BOT = (18, 21, 30), (11, 13, 19)
ACCENT = (10, 132, 255)
TEXT = (233, 237, 245)
MUTED = (150, 159, 178)
DIM = (104, 114, 134)
LINE = (42, 49, 64)

F_TITLE = r"C:\Windows\Fonts\msyhbd.ttc"
F_BODY = r"C:\Windows\Fonts\msyh.ttc"

TITLE_DUR = 4.0
END_DUR = 6.0
LIQ_CANVAS = (150, 230, 600, 690)   # 液化窗口里画布的位置（900x900 抓帧坐标）

# name, 起, 止, 编号, 标题, 正文, 提示, 每帧时长, crop, split
SCENES = [
    dict(a=0, b=16, num="01", title="伴随宿主主面板",
         body=["预览画布 + SAI2 图层列表",
               "点图层行 = 在 SAI2 里选中该层并抓取",
               "「连接注入」读真实图层名 / 混合模式",
               "下方日志实时显示每次操作与耗时"],
         hint="全局热键  Ctrl+Alt+G 抓取 · Ctrl+Alt+A 应用 · Ctrl+Alt+Z 撤销 · Ctrl+Alt+R 复位",
         dur=0.11),

    dict(a=17, b=58, num="02", title="模糊 · 4 种算法",
         body=["高斯 / 方框 / 动感 / 径向",
               "滑杆 + 数值输入（PS 风格）",
               "高斯走 DirectX 11 硬件加速",
               "预览随参数实时刷新"],
         hint="4K 高斯预览 5.9 ms/帧 ≈ 169 FPS（实测）",
         dur=0.11),

    dict(a=59, b=70, num="03", title="应用 → 剪贴板 → SAI2",
         body=["「应用 → 剪贴板」写入 CF_DIBV5 + CF_DIB",
               "自动切回 SAI2 并发送 Ctrl+V 粘贴回原图层",
               "自检往返 maxDiff = 0（像素级无损）"],
         hint="不想自动粘贴可在主面板取消勾选",
         dur=0.12),

    dict(a=71, b=146, num="04", title="液化 · 7 种工具",
         body=["向前变形 / 膨胀 / 收缩 / 顺逆时针旋转",
               "平滑 / 重建",
               "画布内按住拖动 = 逐笔累积",
               "[ ] 调笔刷大小，Shift+[ ] 调强度",
               "右键平移 · 滚轮缩放 · Ctrl+Z 撤销上一笔"],
         hint="演示图用方格 + 同心圆，直线弯曲、圆环被拧一眼可见",
         dur=0.10, crop=(0, 128, 660, 762), split=9),

    dict(a=147, b=155, num="05", title="每一步都能回退",
         body=["应用进撤销栈（Ctrl+Alt+Z）",
               "液化支持逐笔撤销",
               "「复位」回到刚抓取到的原图"],
         hint="撤销不会重写剪贴板，语义与 PS 一致",
         dur=0.12),

    dict(a=156, b=174, num="06", title="滤镜库 · 10 个滤镜",
         body=["油画 / 水彩 / 粗糙蜡笔 / 素描 / 炭笔",
               "粉笔 / 纹理化 / 颗粒 / 浮雕 / 查找边缘",
               "左侧缩略图墙 + 分类树，按可见区域惰性生成",
               "右侧参数面板由插件声明式描述生成"],
         hint="点击缩略图即换滤镜，参数即时生效",
         dur=0.12),

    dict(a=175, b=229, num="07", title="调整 · PS 风格 5 项",
         body=["反相 / 去色 / 亮度对比度 / 色相饱和度 / 色阶",
               "实时预览 + 数值输入框",
               "勾选「应用后合并回原层」只作用于目标层"],
         hint="每个调整都是独立插件（com.sai2.adjust）",
         dur=0.11),

    dict(a=230, b=246, num="08", title="主题与界面",
         body=["主题：跟随系统 / 浅色 / 深色（这里是浅色 → 深色）",
               "圆角窗口（Win11）、悬停高亮",
               "所有窗口都有画布缩放滑杆（左拖缩小 / 右拖放大）",
               "窗口位置记忆，重开即在原处"],
         hint="界面用 Win32 自绘，零第三方依赖",
         dur=0.13),

    dict(a=247, b=259, num="09", title="一功能一窗口",
         body=["模糊 / 滤镜 / 调整 / 液化 各开各的独立窗口",
               "每个窗口持有独立预览会话",
               "缩放、平移、液化置换场互不干扰",
               "主面板只当启动器与素材入口"],
         hint="SAI2 内菜单栏右侧也有一排功能按钮",
         dur=0.12),

    dict(a=260, b=9999, num="10", title="插件体系",
         body=["4 个功能插件 + HelloWorld 示例",
               "插件源 index.json：搜索 / 更新 / SHA-256 校验",
               "RSA PKCS#1 v1.5 签名，安装链路强制验签",
               "异常隔离：插件崩溃不影响宿主"],
         hint="14/14 测试套件 · 自检 PASS=58 · 注入自检 8/8",
         dur=0.12),
]

TITLE_CARD = dict(
    title="SAI2 插件套件",
    subtitle="功能演示 · 模糊 / 滤镜 / 调整 / 液化",
    bullets=[
        "伴随宿主 + 剪贴板桥：在 SAI2 里点功能，处理完自动贴回原图层",
        "一功能一窗口：模糊 4 种 · 滤镜 10 个 · 液化 7 工具 · 调整 5 项",
        "插件系统：manifest / 插件源 / SHA-256 / RSA 验签 / 异常隔离",
        "GPU 加速：DX11 compute 优先，CPU SIMD 兜底",
    ],
    footer="github.com/PainterAnkry/sai2-plugin-suite",
)

END_CARD = dict(
    title="怎么用（4 步）",
    steps=[
        "解压 sai2-plugin-suite-win64.zip，先启动 SAI2 并打开一幅画",
        "双击「启动插件面板.bat」→ 点「连接注入」，图层列表出现",
        "点「模糊 / 滤镜 / 调整 / 液化」任一按钮，自动抓取当前图层并弹出功能窗口",
        "调完点「应用 → 剪贴板」，自动切回 SAI2 发 Ctrl+V 粘贴回原图层",
    ],
    extra="也可以不用注入：在 SAI2 里框选 → Ctrl+C，面板会自动抓图，再点功能按钮开窗",
    note="本片用内置测试图（方格 + 同心圆）+ 模拟图层录制，不接触真实画稿。",
    footer="github.com/PainterAnkry/sai2-plugin-suite · MIT License",
)


def font(path, size):
    return ImageFont.truetype(path, size)


def bg_canvas():
    im = Image.new("RGB", CANVAS, BG_BOT)
    dr = ImageDraw.Draw(im)
    for y in range(CANVAS[1]):
        t = y / (CANVAS[1] - 1)
        c = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        dr.line([(0, y), (CANVAS[0], y)], fill=c)
    return im


def wrap(dr, text, fnt, max_w):
    lines, cur = [], ""
    for ch in text:
        if dr.textlength(cur + ch, font=fnt) <= max_w:
            cur += ch
        else:
            lines.append(cur)
            cur = ch
    if cur:
        lines.append(cur)
    return lines


def crop_window(im):
    a = np.asarray(im, dtype=np.int16)
    diff = np.abs(a - np.array(MATTE, dtype=np.int16)).sum(axis=2)
    ys, xs = np.where(diff > 12)
    if len(xs) < 100:
        return im
    pad = 4
    return im.crop((max(0, xs.min() - pad), max(0, ys.min() - pad),
                    min(im.width, xs.max() + 1 + pad), min(im.height, ys.max() + 1 + pad)))


def draw_progress(im, frac):
    dr = ImageDraw.Draw(im)
    x0, x1, y = 48, 1232, 692
    dr.rounded_rectangle([x0, y, x1, y + 4], radius=2, fill=LINE)
    if frac > 0:
        dr.rounded_rectangle([x0, y, x0 + max(4, int((x1 - x0) * frac)), y + 4],
                             radius=2, fill=ACCENT)
    f = font(F_BODY, 16)
    dr.text((x0, y + 12), "SAI2 插件套件 · 功能演示", font=f, fill=DIM)
    t = "github.com/PainterAnkry/sai2-plugin-suite"
    dr.text((x1 - dr.textlength(t, font=f), y + 12), t, font=f, fill=DIM)


def draw_left(dr, sc):
    dr.ellipse([48, 58, 60, 70], fill=ACCENT)
    dr.text((70, 54), "SAI2 插件套件", font=font(F_BODY, 19), fill=MUTED)
    dr.text((48, 96), sc["num"], font=font(F_TITLE, 64), fill=(38, 48, 68))
    dr.text((50, 168), sc["title"], font=font(F_TITLE, 32), fill=TEXT)
    y = 228
    fb = font(F_BODY, 19)
    for line in sc["body"]:
        for w in wrap(dr, line, fb, 320):
            dr.text((50, y), w, font=fb, fill=MUTED)
            y += 28
        y += 5
    hy = max(y + 18, 560)
    dr.line([(50, hy - 14), (250, hy - 14)], fill=LINE, width=1)
    fh = font(F_BODY, 16)
    for w in wrap(dr, sc["hint"], fh, 320):
        dr.text((50, hy), w, font=fh, fill=(108, 155, 220))
        hy += 24


def draw_frame(src, sc, progress):
    im = bg_canvas()
    dr = ImageDraw.Draw(im)
    draw_left(dr, sc)

    win = src
    if sc.get("crop"):
        win = win.crop(sc["crop"])
    win = crop_window(win)
    box_w, box_h = 820, 612
    s = min(box_w / win.width, box_h / win.height)
    w, h = max(1, int(win.width * s)), max(1, int(win.height * s))
    win = win.resize((w, h), Image.LANCZOS)
    x = 400 + (box_w - w) // 2
    yy = 54 + (box_h - h) // 2
    im.paste(Image.new("RGB", (w + 6, h + 6), (6, 8, 12)), (x - 2, yy - 2))
    im.paste(win, (x, yy))
    dr.rectangle([x - 1, yy - 1, x + w, yy + h], outline=LINE)

    draw_progress(im, progress)
    return im


def draw_split(before, after, sc, progress):
    """并排对比：左=处理前，右=当前（都只看画布区域）。"""
    im = bg_canvas()
    dr = ImageDraw.Draw(im)
    draw_left(dr, sc)

    box = 380
    gap = 34
    x0 = 400
    y0 = 54 + (612 - box) // 2
    b = before.crop(LIQ_CANVAS).resize((box, box), Image.LANCZOS)
    a = after.crop(LIQ_CANVAS).resize((box, box), Image.LANCZOS)
    im.paste(Image.new("RGB", (box + 6, box + 6), (6, 8, 12)), (x0 - 2, y0 - 2))
    im.paste(b, (x0, y0))
    x1 = x0 + box + gap
    im.paste(Image.new("RGB", (box + 6, box + 6), (6, 8, 12)), (x1 - 2, y0 - 2))
    im.paste(a, (x1, y0))
    dr.rectangle([x0 - 1, y0 - 1, x0 + box, y0 + box], outline=LINE)
    dr.rectangle([x1 - 1, y0 - 1, x1 + box, y0 + box], outline=ACCENT)
    f = font(F_TITLE, 22)
    dr.text((x0 + box // 2 - 26, y0 + box + 14), "原图", font=f, fill=MUTED)
    dr.text((x1 + box // 2 - 44, y0 + box + 14), "液化后", font=f, fill=(140, 190, 245))

    draw_progress(im, progress)
    return im


def draw_title_card():
    im = bg_canvas()
    dr = ImageDraw.Draw(im)
    dr.rectangle([0, 0, 6, CANVAS[1]], fill=ACCENT)
    dr.text((90, 150), TITLE_CARD["title"], font=font(F_TITLE, 68), fill=TEXT)
    dr.text((94, 240), TITLE_CARD["subtitle"], font=font(F_BODY, 27), fill=(120, 170, 235))
    dr.line([(94, 296), (94 + 520, 296)], fill=LINE, width=2)
    y = 330
    fb = font(F_BODY, 21)
    for b in TITLE_CARD["bullets"]:
        dr.ellipse([96, y + 9, 104, y + 17], fill=ACCENT)
        for w in wrap(dr, b, fb, 1040):
            dr.text((122, y), w, font=fb, fill=MUTED)
            y += 30
        y += 18
    dr.text((94, 620), TITLE_CARD["footer"], font=font(F_BODY, 18), fill=DIM)
    return im


def draw_end_card():
    im = bg_canvas()
    dr = ImageDraw.Draw(im)
    dr.rectangle([0, 0, 6, CANVAS[1]], fill=ACCENT)
    dr.text((90, 96), END_CARD["title"], font=font(F_TITLE, 52), fill=TEXT)
    y = 190
    fb = font(F_BODY, 20)
    for i, s in enumerate(END_CARD["steps"]):
        dr.ellipse([92, y + 4, 128, y + 40], fill=(30, 62, 104))
        dr.text((103, y + 8), str(i + 1), font=font(F_TITLE, 22), fill=(150, 195, 245))
        lines = wrap(dr, s, fb, 1020)
        for j, w in enumerate(lines):
            dr.text((146, y + 2 + 28 * j), w, font=fb, fill=MUTED)
        y += 34 + 30 * (len(lines) - 1) + 22
    dr.line([(92, y + 6), (1210, y + 6)], fill=LINE, width=1)
    for w in wrap(dr, END_CARD["extra"], font(F_BODY, 18), 1080):
        dr.text((92, y + 24), w, font=font(F_BODY, 18), fill=DIM)
        y += 26
    for w in wrap(dr, END_CARD["note"], font(F_BODY, 17), 1080):
        dr.text((92, y + 22), w, font=font(F_BODY, 17), fill=(122, 132, 152))
        y += 24
    dr.text((92, 648), END_CARD["footer"], font=font(F_BODY, 18), fill=(110, 150, 200))
    return im


def main():
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if f.endswith(".png") or f == "list.txt":
            os.remove(os.path.join(out_dir, f))

    files = sorted(f for f in os.listdir(src_dir) if f.endswith(".bmp"))
    # 丢掉末尾全黑帧（演示退出时抓到的空屏）
    while files:
        a = np.asarray(Image.open(os.path.join(src_dir, files[-1])).convert("L")
                       .resize((40, 40)), dtype=np.int16)
        if a.std() < 2.0:
            print("drop trailing blank:", files[-1])
            files.pop()
        else:
            break
    total = len(files)
    print("usable frames:", total)

    seq = [(draw_title_card(), TITLE_DUR)]
    used = 0
    for sc in SCENES:
        frames = [Image.open(os.path.join(src_dir, files[i])).convert("RGB")
                  for i in range(sc["a"], min(sc["b"], total - 1) + 1)]
        if not frames:
            continue
        before = frames[0]
        for n, fr in enumerate(frames):
            prog = used / max(1, total - 1)
            if sc.get("split") and n >= len(frames) - sc["split"]:
                out = draw_split(before, fr, sc, prog)
            else:
                out = draw_frame(fr, sc, prog)
            seq.append((out, sc["dur"]))
            used += 1

    seq.append((draw_end_card(), END_DUR))

    lines = []
    for n, (im, dur) in enumerate(seq):
        p = os.path.join(out_dir, f"f_{n:05d}.png")
        im.save(p)
        lines.append(f"file '{p}'")
        lines.append(f"duration {dur:.3f}")
        if n % 50 == 0:
            print(f"  composed {n}/{len(seq)}", flush=True)
    lines.append(f"file '{os.path.join(out_dir, f'f_{len(seq)-1:05d}.png')}'")
    with open(os.path.join(out_dir, "list.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    total_s = sum(d for _, d in seq)
    print(f"composed {len(seq)} frames, {total_s:.1f}s -> {out_dir}")


if __name__ == "__main__":
    main()
