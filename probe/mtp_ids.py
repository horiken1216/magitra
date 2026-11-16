"""調査スクリプト共通の機器 ID（core/include/mtp_core.h と同じ値）。

- 0x0265: Lightning の Magic Trackpad（2015 年の Magic Trackpad 2 と 2021 年の第 3 世代）。第 3 世代で実機確認済み
- 0x0324: USB-C の Magic Trackpad（第 4 世代）。仮対応（実機未確認）
"""

VID_BT = 0x004C
VID_USB = 0x05AC
VIDS = {VID_BT: "BT", VID_USB: "USB"}

PID_LIGHTNING = 0x0265
PID_USBC = 0x0324
PIDS = (PID_LIGHTNING, PID_USBC)
PID_NAMES = {PID_LIGHTNING: "Lightning (0x0265)", PID_USBC: "USB-C (0x0324, 仮対応)"}


def is_trackpad(d) -> bool:
    """hid.enumerate() の 1 件が対象のトラックパッドなら True"""
    return d["vendor_id"] in VIDS and d["product_id"] in PIDS
