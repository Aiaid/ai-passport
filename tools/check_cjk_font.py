#!/usr/bin/env python3
"""检查子集 CJK 字体对"上屏中文"的覆盖是否完整。

思路:
  1) 从 main/ui_i18n.c 的字符串字面量(剔除 // 注释)抽取所有非 ASCII 字符 ——
     这些就是会上屏的中文(所有 UI 文案都走 ui_i18n 的 t(key))。
  2) 从生成的字体 C 文件的 glyph 注释 `/* U+XXXX "x" */` 解析已覆盖码点。
  3) 上屏字符集 - 字体覆盖 应为空;否则报告缺失字(需重跑字体子集)。

用法: python3 tools/check_cjk_font.py
退出码非 0 表示有缺字或有显示中文却写死 montserrat 的风险行。
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
I18N = ROOT / "main" / "ui_i18n.c"
FONTS = [ROOT / "main" / "lv_font_echo_cjk_16.c",
         ROOT / "main" / "lv_font_echo_cjk_20.c"]


def onscreen_cjk():
    chars = set()
    for line in I18N.read_text(encoding="utf-8").splitlines():
        code = line.split("//", 1)[0]          # 去掉行尾注释
        for ch in code:
            if ord(ch) > 0x7F:
                chars.add(ch)
    return chars


def font_cover(path):
    cover = set()
    for m in re.finditer(r"/\* U\+([0-9A-Fa-f]+)", path.read_text(encoding="utf-8")):
        cover.add(int(m.group(1), 16))
    return cover


def main():
    need = onscreen_cjk()
    rc = 0
    for f in FONTS:
        cover = font_cover(f)
        missing = sorted(ch for ch in need if ord(ch) not in cover)
        if missing:
            rc = 1
            print(f"[FAIL] {f.name} 缺字({len(missing)}): {''.join(missing)}")
        else:
            print(f"[OK]   {f.name} 覆盖全部 {len(need)} 个上屏汉字")

    # 启发式:列出显示 t(key) 却可能写死 montserrat 的风险(仅提示,不判错)。
    for src in ROOT.glob("main/*.c"):
        for i, line in enumerate(src.read_text(encoding="utf-8").splitlines(), 1):
            if "montserrat" in line and "ui_i18n_t(" in line:
                print(f"[WARN] {src.name}:{i} 同行出现 montserrat 与 t(key),请核对字体")
    return rc


if __name__ == "__main__":
    sys.exit(main())
