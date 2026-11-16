"""USB 接続の Magic Trackpad 2 で、アプリから開ける全コレクションの入力レポートを記録する。

ドライバーは使わない。OS が専有しているコレクション（マウスなど）は開けないので飛ばす。
使い方: python usb_listen.py [秒数] [--dump-dir DIR]
  --dump-dir を付けると、レポート ID ごとに最初の数件を .bin で保存する（形式の解析用）。
"""
import argparse
import os
import time

import hid

USB_VID = 0x05AC
MT2_PID = 0x0265
SAMPLES_PER_ID = 5


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", nargs="?", type=float, default=10)
    ap.add_argument("--dump-dir")
    args = ap.parse_args()

    handles = []
    for d in hid.enumerate(USB_VID, MT2_PID):
        tag = f"MI_{d['interface_number']:02d} 0x{d['usage_page']:04X}/0x{d['usage']:04X}"
        dev = hid.device()
        try:
            dev.open_path(d["path"])
            dev.set_nonblocking(True)
            handles.append((tag, dev))
            print(f"open  {tag} ({d['product_string']})")
        except OSError as e:
            print(f"skip  {tag}: {e}")

    print(f"\n{args.seconds:.0f} 秒間記録します。トラックパッドを 1 本指・2 本指で動かし、押し込んでクリックもしてください。")
    counts: dict[tuple[str, int], int] = {}
    samples: dict[tuple[str, int], list[bytes]] = {}
    lengths: dict[tuple[str, int], set[int]] = {}
    end = time.time() + args.seconds
    while time.time() < end and handles:
        for tag, dev in list(handles):
            try:
                data = dev.read(4096)
            except OSError:
                print(f"read 不可: {tag}（OS 専有）")
                handles.remove((tag, dev))
                continue
            if data:
                key = (tag, data[0])
                counts[key] = counts.get(key, 0) + 1
                lengths.setdefault(key, set()).add(len(data))
                s = samples.setdefault(key, [])
                if len(s) < SAMPLES_PER_ID:
                    s.append(bytes(data))
        time.sleep(0.0005)

    print()
    if not counts:
        print("入力レポートは届きませんでした")
    for key in sorted(counts):
        tag, rid = key
        first = samples[key][0]
        print(f"{tag} report 0x{rid:02X}: {counts[key]} 件 len={sorted(lengths[key])} 例={first[:32].hex(' ')}")

    if args.dump_dir:
        os.makedirs(args.dump_dir, exist_ok=True)
        for (tag, rid), items in samples.items():
            safe = tag.replace(" ", "_").replace("/", "-")
            for i, b in enumerate(items):
                with open(os.path.join(args.dump_dir, f"{safe}_id{rid:02X}_{i}.bin"), "wb") as f:
                    f.write(b)
        print(f"\nサンプルを {args.dump_dir} に保存しました")

    for _, dev in handles:
        dev.close()


if __name__ == "__main__":
    main()
