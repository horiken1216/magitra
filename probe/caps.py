"""各 HID コレクションのレポート ID・長さを Windows HID API (HidP_*) で調べる。"""
import ctypes
from ctypes import wintypes

import hid

hid_dll = ctypes.WinDLL("hid.dll")
k32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)

GENERIC_NONE = 0
FILE_SHARE_RW = 0x1 | 0x2
OPEN_EXISTING = 3
HIDP_STATUS_SUCCESS = 0x00110000
HidP_Input, HidP_Output, HidP_Feature = 0, 1, 2


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
        ("InputReportByteLength", wintypes.USHORT),
        ("OutputReportByteLength", wintypes.USHORT),
        ("FeatureReportByteLength", wintypes.USHORT),
        ("Reserved", wintypes.USHORT * 17),
        ("NumberLinkCollectionNodes", wintypes.USHORT),
        ("NumberInputButtonCaps", wintypes.USHORT),
        ("NumberInputValueCaps", wintypes.USHORT),
        ("NumberInputDataIndices", wintypes.USHORT),
        ("NumberOutputButtonCaps", wintypes.USHORT),
        ("NumberOutputValueCaps", wintypes.USHORT),
        ("NumberOutputDataIndices", wintypes.USHORT),
        ("NumberFeatureButtonCaps", wintypes.USHORT),
        ("NumberFeatureValueCaps", wintypes.USHORT),
        ("NumberFeatureDataIndices", wintypes.USHORT),
    ]


# HIDP_VALUE_CAPS / HIDP_BUTTON_CAPS はどちらも 72 バイト。先頭の UsagePage と ReportID だけ使う。
class CAPS_HEAD(ctypes.Structure):
    _fields_ = [("UsagePage", wintypes.USHORT), ("ReportID", ctypes.c_ubyte),
                ("_rest", ctypes.c_ubyte * 69)]


k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]


def report_ids(pp, kind: int, n_btn: int, n_val: int) -> set[int]:
    ids: set[int] = set()
    for fn, n in ((hid_dll.HidP_GetButtonCaps, n_btn), (hid_dll.HidP_GetValueCaps, n_val)):
        if not n:
            continue
        arr = (CAPS_HEAD * n)()
        ln = wintypes.USHORT(n)
        if fn(kind, arr, ctypes.byref(ln), pp) == HIDP_STATUS_SUCCESS:
            ids.update(c.ReportID for c in arr[: ln.value])
    return ids


def dump(path: str) -> None:
    h = k32.CreateFileW(path, GENERIC_NONE, FILE_SHARE_RW, None, OPEN_EXISTING, 0, None)
    if h in (None, wintypes.HANDLE(-1).value):
        print(f"  open failed: {ctypes.get_last_error()}")
        return
    pp = ctypes.c_void_p()
    try:
        if not hid_dll.HidD_GetPreparsedData(h, ctypes.byref(pp)):
            print("  HidD_GetPreparsedData failed")
            return
        caps = HIDP_CAPS()
        hid_dll.HidP_GetCaps(pp, ctypes.byref(caps))
        print(f"  len: input={caps.InputReportByteLength} output={caps.OutputReportByteLength} "
              f"feature={caps.FeatureReportByteLength}")
        for name, kind, nb, nv in (
            ("input", HidP_Input, caps.NumberInputButtonCaps, caps.NumberInputValueCaps),
            ("output", HidP_Output, caps.NumberOutputButtonCaps, caps.NumberOutputValueCaps),
            ("feature", HidP_Feature, caps.NumberFeatureButtonCaps, caps.NumberFeatureValueCaps),
        ):
            ids = report_ids(pp, kind, nb, nv)
            if ids:
                print(f"  {name} report IDs: {', '.join(f'0x{i:02X}' for i in sorted(ids))}")
    finally:
        if pp:
            hid_dll.HidD_FreePreparsedData(pp)
        k32.CloseHandle(h)


def main() -> None:
    for d in hid.enumerate():
        if d["vendor_id"] in (0x004C, 0x05AC) and d["product_id"] == 0x0265:
            print(f"usage_page=0x{d['usage_page']:04X} usage=0x{d['usage']:04X}")
            dump(d["path"].decode())


if __name__ == "__main__":
    main()
