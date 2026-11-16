"""Magic Trackpad 2 の HID コレクションを列挙する。

BT 接続は VID 0x004C、USB 接続は VID 0x05AC として現れる（PID はどちらも 0x0265）。
"""
import hid

APPLE_VIDS = {0x004C: "BT", 0x05AC: "USB"}
MT2_PID = 0x0265


def main() -> None:
    found = False
    for d in hid.enumerate():
        if d["vendor_id"] not in APPLE_VIDS or d["product_id"] != MT2_PID:
            continue
        found = True
        print(f"[{APPLE_VIDS[d['vendor_id']]}] usage_page=0x{d['usage_page']:04X} "
              f"usage=0x{d['usage']:04X} iface={d['interface_number']}")
        print(f"    product={d['product_string']!r} path={d['path'].decode(errors='replace')}")
    if not found:
        print("Magic Trackpad 2 が見つかりません")


if __name__ == "__main__":
    main()
