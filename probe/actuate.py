"""Actuator（USB MI_02）に振動コマンド（出力レポート 0x53）を送る試験スクリプト。

テンプレートは imbushuo の公開 gist（USB, PID 0x0265, MI_02 で動作報告あり）:
  https://gist.github.com/imbushuo/2d18a9e6f771d0055223a78930a9171c
  idx:  0  1  2  3  4  5  6  7  8  9 10 11 12 13
        53 01 15 6C 02 00 21 2B 06 01 00 16 41 13
強さは idx 2/5/10 の 3 バイト（s0/s1/s2）。MagicTrackpad2ForWindows PR #63 の BT 版
  F2 | 53 01 s0 78 02 s1 24 30 06 01 s2 18 48 12
と並びを突き合わせて特定した。プリセット値も PR #63 のもの（下位バイトから s0, s1, s2）。
  https://github.com/vitoplantamura/MagicTrackpad2ForWindows/pull/63

安全のための制限:
- 強さ以外のバイトは gist の値に固定する
- 強さの上限は s0 <= 0x30、s1 <= 0x20、s2 <= 0x40
  - s1/s2: LinuxMagicForce が「超える値はハードウェアに負荷をかける実験的な設定」と警告している上限
  - s0: knespii/Magic-TP-extended と Nocehi/dms-lookup が BT で既定値にしている 0x30
    （LinuxMagicForce は s0 に上限を設けていないが、公開ツールの既定値を超えない）
- 回数は最大 5 回、間隔は 0.3 秒以上（gist の作者いわく、速すぎる書き込みは拒否される）

使い方: python actuate.py [--preset gist|light|medium|firm | --strength S0,S1,S2] [--count 1-5] [--interval 秒] [--no-prompt]
  --strength は 16 進で指定（例: 30,00,00）。上限を超える値は送らずにエラーにする。
  ターミナルから実行すると、1 回ごとに Enter を押してから送る（準備ができてから体感で確かめるため）。
  --no-prompt を付けると、Enter を待たずに --interval 間隔で続けて送る。
"""
import argparse
import sys
import time

import hid

from mtp_ids import PID_USBC, PIDS, VID_USB, is_trackpad

TEMPLATE = bytes([0x53, 0x01, 0x15, 0x6C, 0x02, 0x00, 0x21, 0x2B, 0x06, 0x01, 0x00, 0x16, 0x41, 0x13])
STRENGTH_INDEX = (2, 5, 10)
STRENGTH_LIMIT = (0x30, 0x20, 0x40)
PRESETS = {
    "gist": (0x15, 0x00, 0x00),    # gist のまま
    "light": (0x15, 0x04, 0x04),   # 0x040415
    "medium": (0x17, 0x06, 0x06),  # 0x060617
    "firm": (0x1E, 0x08, 0x08),    # 0x08081E
}
OUTPUT_REPORT_LENGTH = 64
MAX_PULSES = 5
MIN_INTERVAL_S = 0.3


def build_report(strength: tuple[int, int, int]) -> bytes:
    body = bytearray(TEMPLATE)
    for idx, value, limit in zip(STRENGTH_INDEX, strength, STRENGTH_LIMIT):
        if not 0 <= value <= limit:
            raise ValueError(f"strength byte at idx {idx} = 0x{value:02X} exceeds limit 0x{limit:02X}")
        body[idx] = value
    return bytes(body) + bytes(OUTPUT_REPORT_LENGTH - len(body))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", choices=sorted(PRESETS), default="gist")
    ap.add_argument("--strength", help="S0,S1,S2 を 16 進で（--preset より優先）")
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--no-prompt", action="store_true")
    args = ap.parse_args()
    count = max(1, min(MAX_PULSES, args.count))
    interval = max(MIN_INTERVAL_S, args.interval)
    prompt = not args.no_prompt and sys.stdin.isatty()

    path = None
    found_pid = None
    for d in [x for pid in PIDS for x in hid.enumerate(VID_USB, pid)]:
        if d["interface_number"] == 2 and d["usage_page"] == 0xFF00 and d["usage"] == 0x0D:
            path = d["path"]
            found_pid = d["product_id"]
    if path is None:
        print("Actuator (USB MI_02, 0xFF00/0x0D) が見つかりません。USB で接続されていますか？")
        return
    if found_pid == PID_USBC:
        # テンプレートは PID 0x0265 で確かめたもの。USB-C 版で同じ中身が通じるかは未確認
        print("注意: USB-C 版（PID 0x0324）の Actuator です。この振動レポートが USB-C 版で同じ意味かは確かめていません。")
        if sys.stdin.isatty():
            if input("それでも送る場合は yes と入力: ").strip().lower() != "yes":
                print("送らずに終了しました")
                return
        else:
            print("確認できないため送らずに終了します（ターミナルから実行してください）")
            return

    if args.strength:
        parts = [int(p, 16) for p in args.strength.split(",")]
        if len(parts) != 3:
            raise SystemExit("--strength は S0,S1,S2 の 3 つを指定する")
        strength = (parts[0], parts[1], parts[2])
        label = "custom"
    else:
        strength = PRESETS[args.preset]
        label = args.preset
    report = build_report(strength)
    print(f"strength={label} bytes={report[:14].hex(' ')} count={count} interval={interval:.1f}s")
    dev = hid.device()
    dev.open_path(path)
    try:
        for i in range(count):
            if prompt:
                input(f"[{i + 1}/{count}] 指をトラックパッドに軽く置き、Enter で送信（Ctrl+C で中止）: ")
            n = dev.write(report)
            print(f"pulse {i + 1}/{count}: write -> {n} (期待値 {OUTPUT_REPORT_LENGTH}; -1 は失敗)")
            if n < 0:
                print(f"  error: {dev.error()}")
                break
            if i + 1 < count and not prompt:
                time.sleep(interval)
    finally:
        dev.close()


if __name__ == "__main__":
    main()
