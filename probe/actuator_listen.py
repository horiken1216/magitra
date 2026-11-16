"""Actuator（USB MI_02）の入力レポート 0x3F を時刻付きで記録する（読み取りのみ。何も送らない）。

記述子: 入力 0x3F = 15 バイト、出力 0x53 = 63 バイト（どちらも Usage 0xFF00/0x0D の不透明なバイト列）。
使い方: python actuator_listen.py [秒数]
"""
import sys
import time

import hid

from mtp_ids import PIDS, VID_USB, is_trackpad


def main() -> None:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12
    path = None
    for d in [x for pid in PIDS for x in hid.enumerate(VID_USB, pid)]:
        if d["interface_number"] == 2:
            path = d["path"]
    if path is None:
        print("Actuator (MI_02) が見つかりません")
        return

    dev = hid.device()
    dev.open_path(path)
    dev.set_nonblocking(True)
    print(f"{seconds:.0f} 秒間記録します。クリック・強い押し込み・触れるだけ、を繰り返してください。")

    start = time.perf_counter()
    count = 0
    last = None
    while time.perf_counter() - start < seconds:
        data = dev.read(64)
        if data:
            count += 1
            b = bytes(data)
            t = time.perf_counter() - start
            mark = "" if b != last else " (同上)"
            print(f"{t:7.3f}s len={len(b)} {b.hex(' ')}{mark}")
            last = b
        else:
            time.sleep(0.0005)
    dev.close()
    print(f"\n{count} 件")


if __name__ == "__main__":
    main()
