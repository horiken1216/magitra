"""bt_sdp.py で書き換えた後の、レポート 0x31 の通り方を細かく調べる（調査用）。

  python bt_mt_probe2.py count [秒数]   届いた 0x31 を長さごとに数える（指の本数を決めて触る）
  python bt_mt_probe2.py get [回数]     HidD_GetInputReport（GET_REPORT）で 0x31 を取りにいく
"""
import ctypes
import sys
import time

import hid

from mtp_ids import PIDS, VID_BT

MT_PAGE = 0xFF31


def open_mt():
    devs = [d for d in hid.enumerate(VID_BT) if d["product_id"] in PIDS and d["usage_page"] == MT_PAGE]
    if not devs:
        raise SystemExit("0xFF31 のコレクションがありません")
    dev = hid.device()
    dev.open_path(devs[0]["path"])
    return dev, devs[0]["path"]


def cmd_count(seconds):
    dev, _ = open_mt()
    dev.set_nonblocking(True)
    lengths, first, last = {}, None, None
    end = time.time() + seconds
    while time.time() < end:
        data = dev.read(512)
        if not data:
            time.sleep(0.001)
            continue
        now = time.time()
        first = first or now
        last = now
        lengths[len(data)] = lengths.get(len(data), 0) + 1
    dev.close()
    total = sum(lengths.values())
    rate = total / (last - first) if total > 1 and last > first else 0
    print(f"届いた件数 {total}（届いていた間は毎秒 {rate:.0f} 件）。長さ別: "
          + (", ".join(f"{k} バイト×{v}" for k, v in sorted(lengths.items())) or "なし"))


def cmd_get(times):
    # hidapi の get_input_report は使える長さを記述子から決めるので、hid.dll を直接呼んで長さを変えて試す
    _, path = open_mt()
    k32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)
    hd = ctypes.WinDLL("hid.dll", use_last_error=True)
    k32.CreateFileW.restype = ctypes.c_void_p
    k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    hd.HidD_GetInputReport.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    h = k32.CreateFileW(path.decode(), 0xC0000000, 3, None, 3, 0, None)
    if h in (None, ctypes.c_void_p(-1).value):
        raise SystemExit(f"開けませんでした err={ctypes.get_last_error()}")
    try:
        for size in (13, 22, 31, 112):
            buf = (ctypes.c_ubyte * size)()
            buf[0] = 0x31
            ok = hd.HidD_GetInputReport(h, buf, size)
            err = ctypes.get_last_error()
            print(f"長さ {size:3d}: {'成功 ' + bytes(buf[:24]).hex(' ') if ok else f'失敗 err={err}'}")
        print(f"長さ 13 で {times} 回続けて取る（触りながら）")
        got, t0 = 0, time.time()
        for _ in range(times):
            buf = (ctypes.c_ubyte * 13)()
            buf[0] = 0x31
            if hd.HidD_GetInputReport(h, buf, 13):
                got += 1
                if got <= 5:
                    print("  " + bytes(buf).hex(" "))
        dt = time.time() - t0
        print(f"{got}/{times} 回成功、{dt:.2f} 秒（1 回 {dt / times * 1000:.1f} ms）")
    finally:
        k32.CloseHandle(h)


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "count":
        cmd_count(float(sys.argv[2]) if len(sys.argv) > 2 else 15)
    elif len(sys.argv) >= 2 and sys.argv[1] == "get":
        cmd_get(int(sys.argv[2]) if len(sys.argv) > 2 else 50)
    else:
        print(__doc__)
