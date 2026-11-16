"""BT マルチタッチ有効化 (F1 02 01) を各コレクションに送り、届いた入力レポートを記録する。

Linux hid-magicmouse と同じ手順。Windows が記述子にない ID を通すかを確認する。
"""
import ctypes
import sys
import time

import hid

k32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)
hid_dll = ctypes.WinDLL("hid.dll", use_last_error=True)
k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
hid_dll.HidD_SetFeature.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
hid_dll.HidD_GetFeature.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]

GENERIC_RW = 0x80000000 | 0x40000000
SHARE_RW = 0x1 | 0x2
OPEN_EXISTING = 3


def open_raw(path: str):
    h = k32.CreateFileW(path, GENERIC_RW, SHARE_RW, None, OPEN_EXISTING, 0, None)
    return None if h in (None, ctypes.c_void_p(-1).value) else h


def main() -> None:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 8
    devs = [d for d in hid.enumerate() if d["vendor_id"] in (0x004C, 0x05AC) and d["product_id"] == 0x0265]

    for d in devs:
        path = d["path"].decode()
        tag = f"0x{d['usage_page']:04X}/0x{d['usage']:04X}"
        h = open_raw(path)
        if not h:
            print(f"{tag}: CreateFile(RW) failed err={ctypes.get_last_error()}")
            continue
        for payload in (b"\xF1\x02\x01", b"\x02\x01"):
            buf = ctypes.create_string_buffer(payload)
            ok = hid_dll.HidD_SetFeature(h, buf, len(payload))
            print(f"{tag}: SetFeature {payload.hex(' ')} -> ok={bool(ok)} err={ctypes.get_last_error()}")
        buf = ctypes.create_string_buffer(65)
        buf[0] = 0x55
        ok = hid_dll.HidD_GetFeature(h, buf, 65)
        print(f"{tag}: GetFeature 0x55 -> ok={bool(ok)} err={ctypes.get_last_error()} "
              f"data={buf.raw[:16].hex(' ') if ok else '-'}")
        k32.CloseHandle(h)

    print(f"\n{seconds:.0f} 秒間、入力レポートを記録します。トラックパッドを複数の指で触ってください。")
    handles = []
    for d in devs:
        try:
            dev = hid.device()
            dev.open_path(d["path"])
            dev.set_nonblocking(True)
            handles.append((f"0x{d['usage_page']:04X}", dev))
        except OSError as e:
            print(f"0x{d['usage_page']:04X}: open failed ({e})")

    counts: dict[tuple[str, int], int] = {}
    samples: dict[tuple[str, int], bytes] = {}
    end = time.time() + seconds
    while time.time() < end:
        for tag, dev in list(handles):
            try:
                data = dev.read(256)
            except OSError:
                print(f"{tag}: read 不可（OS 専有）")
                handles.remove((tag, dev))
                continue
            if data:
                key = (tag, data[0])
                counts[key] = counts.get(key, 0) + 1
                samples.setdefault(key, bytes(data))
        time.sleep(0.001)

    if not counts:
        print("入力レポートは届きませんでした")
    for (tag, rid), n in sorted(counts.items()):
        print(f"{tag} report 0x{rid:02X}: {n} 件 len={len(samples[(tag, rid)])} "
              f"例={samples[(tag, rid)][:24].hex(' ')}")
    for _, dev in handles:
        dev.close()


if __name__ == "__main__":
    main()
