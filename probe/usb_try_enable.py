"""USB 接続で、マルチタッチ切り替え（フィーチャーレポート 02 01）を各 HID コレクションへ送れるか試す。

記述子にフィーチャーレポート 0x02 がないため、hidclass が弾く（ERROR_INVALID_FUNCTION = 1）と予想している。
"""
import ctypes

import hid

from mtp_ids import PIDS, VID_USB, is_trackpad

k32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)
hid_dll = ctypes.WinDLL("hid.dll", use_last_error=True)
k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
hid_dll.HidD_SetFeature.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]

GENERIC_RW = 0x80000000 | 0x40000000
SHARE_RW = 0x1 | 0x2
OPEN_EXISTING = 3
INVALID = ctypes.c_void_p(-1).value


def main() -> None:
    for d in [x for pid in PIDS for x in hid.enumerate(VID_USB, pid)]:
        tag = f"MI_{d['interface_number']:02d} 0x{d['usage_page']:04X}/0x{d['usage']:04X}"
        h = k32.CreateFileW(d["path"].decode(), GENERIC_RW, SHARE_RW, None, OPEN_EXISTING, 0, None)
        if h in (None, INVALID):
            print(f"{tag}: open failed err={ctypes.get_last_error()}")
            continue
        for payload in (b"\x02\x01", b"\x02\x01\x00\x00"):
            buf = ctypes.create_string_buffer(payload, len(payload))
            ok = hid_dll.HidD_SetFeature(h, buf, len(payload))
            print(f"{tag}: SetFeature {payload.hex(' ')} -> ok={bool(ok)} err={ctypes.get_last_error()}")
        k32.CloseHandle(h)


if __name__ == "__main__":
    main()
