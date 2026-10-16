#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 photocraft 的滤镜库规格表生成 GallerySpecs.h。

来源：F:\\workspace\\photocraft\\crates\\algo\\src\\artistic.rs 的 `gallery!{...}`
       （Photoshop 滤镜库 47 个滤镜：key / 显示名 / 分类 / 参数记法）

为什么要这样生成：photocraft 的参数记法自带 min/max/默认值/枚举项，
形如 {"pencilWidth":1..24=4,"texture":"canvas|brick","invert":bool}，
正好能自动生成我们插件的 DefParam(...)，因此 47 个滤镜**不需要手写 47 个
包装类**：一张规格表 + 一个参数解析器 + 47 个算法函数即可。

用法：python gen_gallery_specs.py [photocraft/artistic.rs 路径] [输出路径]
"""
import os
import re
import sys

DEFAULT_SRC = r"F:\workspace\photocraft\crates\algo\src\artistic.rs"
OUT_NAME = "GallerySpecs.h"

ROW_RE = re.compile(
    r'^(\w+)\s*=\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*r#"(.*?)"#;\s*$')


def parse(src_path):
    text = open(src_path, encoding="utf-8").read()
    m = re.search(r"gallery!\s*\{(.*?)\n\}", text, re.S)
    if not m:
        raise SystemExit("找不到 gallery!{...} 块")
    rows = []
    for line in m.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        mm = ROW_RE.match(line)
        if mm:
            rows.append(mm.groups())
    return rows


def cstr(s):
    """转成 C++ 字符串字面量内容（只转义反斜杠和引号）。"""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def emit(rows, out_path):
    L = []
    L.append("#pragma once")
    L.append("// " + "=" * 74)
    L.append("// GallerySpecs.h - 自动生成，请勿手改")
    L.append("// 生成器: plugins/filter-gallery/tools/gen_gallery_specs.py")
    L.append("// 来源:   photocraft crates/algo/src/artistic.rs 的 gallery!{...}")
    L.append("//         （Photoshop 滤镜库的 key / 显示名 / 分类 / 参数记法）")
    L.append("// 参数记法: {\"名\":min..max=默认, \"名\":\"选项a|选项b\", \"名\":bool}")
    L.append("// " + "=" * 74)
    L.append("")
    L.append("namespace sai2::fg {")
    L.append("")
    L.append("struct GallerySpec {")
    L.append("    const char* ident;     // 源标识符（调试用）")
    L.append("    const char* key;       // camelCase key；id = com.sai2.filter.gallery.<key>")
    L.append("    const char* name;      // 显示名")
    L.append("    const char* category;  // 分类")
    L.append("    const char* params;    // 参数记法")
    L.append("};")
    L.append("")
    L.append("// 源规格表条目数。源文件或生成器改动导致条目数变化时编译期即报错，")
    L.append("// 防止\"悄悄少生成几个滤镜\"。")
    L.append("inline constexpr int kGallerySpecCount = %d;" % len(rows))
    L.append("")
    L.append("inline const GallerySpec* GallerySpecs(int& count) {")
    L.append("    static const GallerySpec kSpecs[] = {")
    for ident, key, name, cat, params in rows:
        L.append('        {"%s", "%s", "%s", "%s", "%s"},'
                 % (cstr(ident), cstr(key), cstr(name), cstr(cat), cstr(params)))
    L.append("    };")
    L.append("    count = kGallerySpecCount;")
    L.append("    return kSpecs;")
    L.append("}")
    L.append("")
    L.append("} // namespace sai2::fg")
    L.append("")
    open(out_path, "w", encoding="utf-8", newline="\n").write("\n".join(L))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "src", OUT_NAME)
    rows = parse(src)
    cats = {}
    for _, _, _, c, _ in rows:
        cats[c] = cats.get(c, 0) + 1
    emit(rows, out)
    print("解析 %d 条：%s" % (len(rows), ", ".join("%s=%d" % kv for kv in cats.items())))
    print("已生成 %s (%d 字节)" % (out, os.path.getsize(out)))
    print("样例行: %s" % (rows[0][1] if rows else "-"))


if __name__ == "__main__":
    main()
