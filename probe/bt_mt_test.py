"""SDP キャッシュを書き換えた後（bt_sdp.py apply）、Bluetooth の Magic Trackpad からマルチタッチのレポートを読めるか確かめる（調査用）。

1. Windows が見せている HID コレクションを表示する（書き換えが効いていれば UsagePage 0xFF31 が増える）
2. 0xFF31 のコレクションに、マルチタッチへの切り替え（フィーチャー F1 02 01）を送る
3. 届いた入力レポートを、ID・長さ・先頭のバイトで表示する（指を 1 本 → 2 本と動かす）

使い方: python bt_mt_test.py [秒数]
"""
import sys
import time

import hid

from mtp_ids import PIDS, VID_BT

MT_PAGE = 0xFF31


def main() -> None:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10
    devs = [d for d in hid.enumerate(VID_BT) if d["product_id"] in PIDS]
    if not devs:
        print("Bluetooth の Magic Trackpad が見つかりません")
        return
    for d in devs:
        print(f"UsagePage 0x{d['usage_page']:04X} Usage 0x{d['usage']:04X}  {d['path'].decode()}")
    mt = [d for d in devs if d["usage_page"] == MT_PAGE]
    if not mt:
        print("0xFF31 のコレクションがありません（書き換えが効いていない。つなぎ直したか確かめる）")
        return

    dev = hid.device()
    dev.open_path(mt[0]["path"])
    try:
        try:
            n = dev.send_feature_report([0xF1, 0x02, 0x01])
            print(f"切り替え（F1 02 01）を送りました: {n}")
        except (OSError, ValueError) as e:
            print(f"切り替えを送れませんでした: {e}")
        dev.set_nonblocking(True)
        print(f"{seconds:.0f} 秒間、トラックパッドを触ってください（1 本指 → 2 本指 → 押し込み）")
        counts = {}
        end = time.time() + seconds
        shown = 0
        while time.time() < end:
            data = dev.read(512)
            if not data:
                time.sleep(0.002)
                continue
            rid = data[0]
            counts[rid] = counts.get(rid, 0) + 1
            if shown < 40:
                print(f"id=0x{rid:02X} len={len(data):3d}  {bytes(data[:24]).hex(' ')}")
                shown += 1
        print("届いたレポート: " + (", ".join(f"0x{k:02X}×{v}" for k, v in sorted(counts.items())) or "なし"))
    finally:
        dev.close()


if __name__ == "__main__":
    main()
