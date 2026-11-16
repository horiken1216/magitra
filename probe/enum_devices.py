"""Magic Trackpad の HID コレクションを列挙する。

BT 接続は VID 0x004C、USB 接続は VID 0x05AC として現れる。PID は機種で決まり、BT と USB で同じ
（Lightning: 0x0265、USB-C: 0x0324。mtp_ids.py）。
"""
import hid

from mtp_ids import PID_NAMES, VIDS, is_trackpad


def main() -> None:
    found = False
    for d in hid.enumerate():
        if not is_trackpad(d):
            continue
        found = True
        print(f"[{VIDS[d['vendor_id']]} {PID_NAMES[d['product_id']]}] usage_page=0x{d['usage_page']:04X} "
              f"usage=0x{d['usage']:04X} iface={d['interface_number']}")
        print(f"    product={d['product_string']!r} path={d['path'].decode(errors='replace')}")
    if not found:
        print("Magic Trackpad が見つかりません")


if __name__ == "__main__":
    main()
