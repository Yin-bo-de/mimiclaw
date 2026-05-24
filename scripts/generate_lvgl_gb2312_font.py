#!/usr/bin/env python3
"""Generate LVGL GB2312 Chinese font for MimiClaw e-paper dashboard."""

import argparse
import subprocess
import sys
from pathlib import Path

GB2312_HANZI_START = 0xB0
GB2312_HANZI_END = 0xF7
GB2312_ROW_START = 0xA1
GB2312_ROW_END = 0xFE
CJK_BLOCK_START = 0x4E00
CJK_BLOCK_END = 0x9FFF

ASCII = ''.join(chr(cp) for cp in range(0x20, 0x7F))
EXTRA_SYMBOLS = '，。！？：；、（）【】《》“”''—…·℃%/-'
PROJECT_TEXT = '周一二三四五六日天气当前位置多云晴阴雨雪北京上海待办天气待更新Time'
REQUIRED_CHARS = '周日天气当前位置多云晴北京上海待办℃'


def decode_gb2312_hanzi():
    chars = []
    for hi in range(GB2312_HANZI_START, GB2312_HANZI_END + 1):
        for lo in range(GB2312_ROW_START, GB2312_ROW_END + 1):
            try:
                char = bytes([hi, lo]).decode('gb2312')
            except UnicodeDecodeError:
                continue
            if len(char) == 1 and CJK_BLOCK_START <= ord(char) <= CJK_BLOCK_END:
                chars.append(char)
    return chars


def build_symbols():
    symbols = sorted(set(ASCII + EXTRA_SYMBOLS + PROJECT_TEXT + ''.join(decode_gb2312_hanzi())))
    missing = [char for char in REQUIRED_CHARS if char not in symbols]
    if missing:
        raise RuntimeError(f'missing required chars: {"".join(missing)}')
    return ''.join(symbols)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--font', required=True, help='Path to font file')
    parser.add_argument('--size', type=int, default=16)
    parser.add_argument('--bpp', type=int, default=2)
    parser.add_argument('--output', default='main/display/display_font_zh_gb2312_16.c')
    parser.add_argument('--font-name', default='mimi_font_zh_gb2312_16')
    args = parser.parse_args()

    font_path = Path(args.font)
    if not font_path.exists():
        print(f'FATAL: font file not found: {font_path}', file=sys.stderr)
        return 1

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    symbols_path = output_path.with_suffix('.symbols.txt')
    symbols = build_symbols()
    symbols_path.write_text(symbols, encoding='utf-8')

    command = [
        'npx', 'lv_font_conv',
        '--format', 'lvgl',
        '--font', str(font_path),
        '--size', str(args.size),
        '--bpp', str(args.bpp),
        '--no-compress',
        '--no-prefilter',
        '--force-fast-kern-format',
        '--symbols', symbols,
        '--lv-font-name', args.font_name,
        '-o', str(output_path),
    ]
    subprocess.run(command, check=True)

    print(f'Generated {output_path}')
    print(f'Characters: {len(symbols)}')
    print(f'Symbols file: {symbols_path}')
    print(f'Output size: {output_path.stat().st_size:,} bytes')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
