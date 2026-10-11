# 功能演示视频流水线

演示视频 `sai2_功能演示.mp4`（40 秒，1280×720 H.264 + 轻配乐）的生成方式，全部可复现，
不依赖任何在线素材：

```
1) sai2_bridge.exe --demo 30 --out <帧目录>       # 自动编排演示并逐帧抓屏（BMP 序列）
2) python compose_demo_video.py <帧目录> <合成目录>   # 加标题卡/分场景字幕/进度条/前后对比
3) python make_bgm.py <时长秒> music.wav           # 程序合成原创背景音乐（无版权素材）
4) ffmpeg -f concat -safe 0 -i <合成目录>/list.txt -i music.wav \
        -vsync cfr -r 25 -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p \
        -c:a aac -b:a 192k -shortest -movflags +faststart sai2_功能演示.mp4
   # 预览 GIF（两遍调色板）：
   ffmpeg -i sai2_功能演示.mp4 -vf "fps=10,scale=600:-1,palettegen=stats_mode=diff" pal.png
   ffmpeg -i sai2_功能演示.mp4 -i pal.png -lavfi "fps=10,scale=600:-1[x];[x][1:v]paletteuse" -loop 0 预览.gif
```

## 说明

- `--demo` 会自己在进程内编排一整套流程（抓图 → 模糊调参/应用 → 液化 4 种形变 →
  滤镜库 → 调整 → 主题切换），并把每个演示窗口的客户区按 150ms 抓一帧合成 900×900 BMP。
  它使用内置测试图（方格 + 同心圆 + 斜纹）与模拟图层，**不接触真实画稿**。
- `compose_demo_video.py` 里的 `SCENES` 表按帧区间写场景标题/正文/提示，并支持
  `crop`（裁剪到画布，放大看形变）与 `split`（末尾若干帧做「原图 / 处理后」并排对比）。
  帧区间随演示脚本改动会漂移，可用同目录思路（帧间差异峰值）重新定位。
- `make_bgm.py` 用 numpy 合成 92 BPM 的 pad + 琶音 + 轻打击（Am7-Fmaj7-Cmaj7-G6 循环），
  输出 44.1kHz 立体声 WAV，无第三方素材。
- ffmpeg 只需任意带 libx264 的构建；本机使用 `D:\小丸工具箱\tools\ffmpeg.exe`。
