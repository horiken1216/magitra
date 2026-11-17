"""Bluetooth の SDP キャッシュ（レジストリ）にある HID 記述子を読み、書き換えた記録を作る（調査用）。

Windows はペアリングした機器の SDP の記録を
  HKLM\\SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices\\<アドレス>\\CachedServices\\<番号>
に保存している。HID の記述子は属性 0x0206（HIDDescriptorList）にある。
ここに Magic Trackpad のマルチタッチ用レポート（入力 0x31、切り替えのフィーチャー 0xF1、振動 0xF2）を宣言した
ベンダー定義のコレクションを足すと、Windows の HID の層がレポートを捨てずにアプリへ渡すかを確かめるための道具。

使い方:
  python bt_sdp.py selftest            キャッシュにある全記録を読み、組み立て直して元と同じになるか確かめる
  python bt_sdp.py dump                HID の記述子を持つ記録を探し、記述子を表示する（読み取りだけ）
  python bt_sdp.py patch ADDR VALUE [N]  書き換えた記録を build\\bt_sdp\\ に作る（0x31 を N バイトで宣言。既定 111。
                                     元の記録の控えも保存し、2 回目からは控えから作り直す。レジストリは変えない）
  python bt_sdp.py apply ADDR VALUE    作った記録をレジストリに書く（管理者権限が必要。元に戻すには restore）
  python bt_sdp.py restore ADDR VALUE  控えておいた元の記録に戻す（管理者権限が必要）
"""
import os
import sys
import winreg

DEVICES_KEY = r"SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices"
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "bt_sdp")
ATTR_HID_DESCRIPTOR_LIST = 0x0206

# ---- SDP の data element ----
# ヘッダー 1 バイト: 種類（上位 5 ビット）と大きさの指定（下位 3 ビット）
T_NIL, T_UINT, T_INT, T_UUID, T_TEXT, T_BOOL, T_SEQ, T_ALT, T_URL = range(9)
FIXED_SIZES = {0: 1, 1: 2, 2: 4, 3: 8, 4: 16}
VAR_LEN_BYTES = {5: 1, 6: 2, 7: 4}


class Element:
    """読んだときの大きさの指定（size_index）を覚えておき、変えていなければ同じ形で書き戻す"""

    def __init__(self, etype, size_index, value):
        self.etype = etype
        self.size_index = size_index
        self.value = value  # 入れ物なら Element のリスト、それ以外は bytes

    def is_container(self):
        return self.etype in (T_SEQ, T_ALT)


def parse_element(data, pos):
    header = data[pos]
    etype, size_index = header >> 3, header & 7
    pos += 1
    if etype == T_NIL:
        return Element(etype, size_index, b""), pos
    if size_index in FIXED_SIZES:
        n = FIXED_SIZES[size_index]
    else:
        lb = VAR_LEN_BYTES[size_index]
        n = int.from_bytes(data[pos:pos + lb], "big")
        pos += lb
    body = data[pos:pos + n]
    if len(body) != n:
        raise ValueError(f"data element が途中で切れている（位置 {pos}、長さ {n}）")
    pos += n
    if etype in (T_SEQ, T_ALT):
        children, p = [], 0
        while p < len(body):
            child, p = parse_element(body, p)
            children.append(child)
        return Element(etype, size_index, children), pos
    return Element(etype, size_index, bytes(body)), pos


def serialize(el):
    if el.etype == T_NIL:
        return bytes([0])
    body = b"".join(serialize(c) for c in el.value) if el.is_container() else el.value
    if el.size_index in FIXED_SIZES:
        if len(body) != FIXED_SIZES[el.size_index]:
            raise ValueError("固定長の要素の長さが変わっている")
        return bytes([(el.etype << 3) | el.size_index]) + body
    # 可変長: 元の大きさの指定に収まればそれを使い、収まらなければ広げる
    size_index = el.size_index
    while len(body) >= 1 << (8 * VAR_LEN_BYTES[size_index]):
        size_index += 1
        if size_index > 7:
            raise ValueError("要素が大きすぎる")
    lb = VAR_LEN_BYTES[size_index]
    return bytes([(el.etype << 3) | size_index]) + len(body).to_bytes(lb, "big") + body


def parse_record(data):
    el, pos = parse_element(data, 0)
    if pos != len(data):
        raise ValueError(f"記録の後ろに余分なバイトがある（{len(data) - pos} バイト）")
    if el.etype != T_SEQ:
        raise ValueError("記録が sequence ではない")
    return el


def attributes(record):
    """記録（属性 ID と値の並び）を {属性 ID: 値の Element} にする"""
    items = record.value
    return {int.from_bytes(items[i].value, "big"): items[i + 1] for i in range(0, len(items) - 1, 2)}


def find_hid_descriptor(record):
    """HIDDescriptorList（Seq{ Seq{ uint8 0x22, text 記述子 } }）から、記述子の text 要素を返す"""
    attr = attributes(record).get(ATTR_HID_DESCRIPTOR_LIST)
    if attr is None:
        return None
    for entry in attr.value:
        if entry.is_container() and len(entry.value) >= 2 and entry.value[0].value == b"\x22":
            return entry.value[1]
    return None


# ---- HID の記述子 ----
MAIN_ITEMS = {0x8: "Input", 0x9: "Output", 0xB: "Feature"}


def describe_hid(desc):
    """レポート ID ごとの入力・出力・フィーチャーのビット数と、コレクションの Usage を表にする"""
    pos, report_id, size, count, usage_page = 0, 0, 0, 0, 0
    totals, lines, depth, pending_usage = {}, [], 0, None
    while pos < len(desc):
        b = desc[pos]
        if b == 0xFE:  # long item
            pos += 3 + desc[pos + 1]
            continue
        n = {0: 0, 1: 1, 2: 2, 3: 4}[b & 3]
        tag, typ = b >> 4, (b >> 2) & 3
        val = int.from_bytes(desc[pos + 1:pos + 1 + n], "little")
        pos += 1 + n
        if typ == 1:  # global
            if tag == 0x0:
                usage_page = val
            elif tag == 0x7:
                size = val
            elif tag == 0x8:
                report_id = val
            elif tag == 0x9:
                count = val
        elif typ == 2 and tag == 0x0:  # local Usage
            pending_usage = val
        elif typ == 0:  # main
            if tag in MAIN_ITEMS:
                key = (report_id, MAIN_ITEMS[tag])
                totals[key] = totals.get(key, 0) + size * count
            elif tag == 0xA:  # Collection
                lines.append(f"{'  ' * depth}Collection 0x{val:02X}  UsagePage 0x{usage_page:04X} Usage 0x{(pending_usage or 0):04X}")
                depth += 1
            elif tag == 0xC:
                depth -= 1
            pending_usage = None
    for (rid, kind), bits in sorted(totals.items()):
        lines.append(f"Report 0x{rid:02X} {kind:7s} {bits} bits ({(bits + 7) // 8} bytes)")
    return lines


# Magic Trackpad（Bluetooth）のマルチタッチ用に足すコレクション
# 入力 0x31: 見出し 3 バイト + 指 1 本 9 バイト（Linux hid-magicmouse の TRACKPAD2_BT_HEADER_SIZE=4 は ID を含む）。12 本分を宣言する
# フィーチャー 0xF1: マルチタッチへの切り替え（F1 02 01）
# 出力・フィーチャー 0xF2: 振動（F2 53 01 ...、ID を除いて 14 バイト。MagicTrackpad2ForWindows PR #63）
MT_INPUT_BYTES = 3 + 9 * 12


def extra_collection(mt_bytes=MT_INPUT_BYTES):
    """足すコレクション。mt_bytes は入力 0x31 として宣言する長さ（ID を除く）"""
    c = bytearray(EXTRA_COLLECTION)
    i = c.index(bytes([0x96]))  # Report Count（2 バイト）の位置
    c[i + 1], c[i + 2] = mt_bytes & 0xFF, mt_bytes >> 8
    return bytes(c)


EXTRA_COLLECTION = bytes([
    0x06, 0x31, 0xFF,              # Usage Page (Vendor 0xFF31)
    0x09, 0x01,                    # Usage (0x01)
    0xA1, 0x01,                    # Collection (Application)
    0x15, 0x00, 0x26, 0xFF, 0x00,  #   Logical 0..255
    0x75, 0x08,                    #   Report Size 8
    0x85, 0x31,                    #   Report ID 0x31
    0x09, 0x02,                    #   Usage (0x02)
    0x96, MT_INPUT_BYTES & 0xFF, MT_INPUT_BYTES >> 8,  # Report Count
    0x81, 0x02,                    #   Input (Data, Var, Abs)
    0x85, 0xF1,                    #   Report ID 0xF1
    0x09, 0x03,                    #   Usage (0x03)
    0x95, 0x02,                    #   Report Count 2
    0xB1, 0x02,                    #   Feature (Data, Var, Abs)
    0x85, 0xF2,                    #   Report ID 0xF2
    0x09, 0x04,                    #   Usage (0x04)
    0x95, 0x0E,                    #   Report Count 14
    0x91, 0x02,                    #   Output (Data, Var, Abs)
    0x09, 0x05,                    #   Usage (0x05)
    0xB1, 0x02,                    #   Feature (Data, Var, Abs)
    0xC0,                          # End Collection
])


# ---- レジストリ ----

def read_cached(addr, value):
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, rf"{DEVICES_KEY}\{addr}\CachedServices") as k:
        data, typ = winreg.QueryValueEx(k, value)
    if typ != winreg.REG_BINARY:
        raise ValueError(f"{addr}\\{value} は REG_BINARY ではない（{typ}）")
    return bytes(data)


def write_cached(addr, value, data):
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, rf"{DEVICES_KEY}\{addr}\CachedServices", 0,
                        winreg.KEY_SET_VALUE) as k:
        winreg.SetValueEx(k, value, 0, winreg.REG_BINARY, data)


def device_name(addr):
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, rf"{DEVICES_KEY}\{addr}") as k:
            raw, _ = winreg.QueryValueEx(k, "Name")
            return bytes(raw).rstrip(b"\0").decode("utf-8", "replace")
    except OSError:
        return ""


def all_records():
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, DEVICES_KEY) as dk:
        i = 0
        while True:
            try:
                addr = winreg.EnumKey(dk, i)
            except OSError:
                return
            i += 1
            try:
                ck = winreg.OpenKey(dk, rf"{addr}\CachedServices")
            except OSError:
                continue
            with ck:
                j = 0
                while True:
                    try:
                        value, data, typ = winreg.EnumValue(ck, j)
                    except OSError:
                        break
                    j += 1
                    if typ == winreg.REG_BINARY:
                        yield addr, value, bytes(data)


# ---- 指示 ----

def cmd_selftest():
    ok = ng = 0
    for addr, value, data in all_records():
        try:
            same = serialize(parse_record(data)) == data
        except (ValueError, KeyError, IndexError) as e:
            same, why = False, str(e)
        else:
            why = "組み立て直すと元と違う"
        if same:
            ok += 1
        else:
            ng += 1
            print(f"NG {addr} {value} ({len(data)} bytes): {why}")
    print(f"{ok} 件は元と同じに戻った、{ng} 件は違った")
    return 0 if ng == 0 else 1


def cmd_dump():
    found = 0
    for addr, value, data in all_records():
        try:
            desc_el = find_hid_descriptor(parse_record(data))
        except (ValueError, KeyError, IndexError):
            continue
        if desc_el is None:
            continue
        found += 1
        desc = desc_el.value
        print(f"== {addr} '{device_name(addr)}' CachedServices\\{value}  記録 {len(data)} バイト、記述子 {len(desc)} バイト")
        for line in describe_hid(desc):
            print("   " + line)
        print("   " + desc.hex(" "))
    if not found:
        print("HID の記述子を持つ記録はありません（トラックパッドを Bluetooth でペアリングしてから実行してください）")


def cmd_patch(addr, value, mt_bytes=MT_INPUT_BYTES):
    os.makedirs(OUT_DIR, exist_ok=True)
    orig_path = os.path.join(OUT_DIR, f"{addr}_{value}.orig.bin")
    if os.path.exists(orig_path):  # 2 回目からは、控えた元の記録から作り直す
        with open(orig_path, "rb") as f:
            data = f.read()
    else:
        data = read_cached(addr, value)
    record = parse_record(data)
    desc_el = find_hid_descriptor(record)
    if desc_el is None:
        raise SystemExit(f"{addr}\\{value} には HID の記述子がありません")
    used = {line.split()[1] for line in describe_hid(desc_el.value) if line.startswith("Report ")}
    clash = {"0x31", "0xF1", "0xF2"} & used
    if clash:
        raise SystemExit(f"足そうとしたレポート ID が元の記述子にすでにあります（書き換え済みの記録かもしれない）: {', '.join(sorted(clash))}")
    extra = extra_collection(mt_bytes)
    desc_el.value = desc_el.value + extra
    patched = serialize(record)
    find_hid_descriptor(parse_record(patched))  # 読み直せることを確かめる
    if not os.path.exists(orig_path):  # 控えは最初の 1 回だけ（書き換え後の値で上書きしない）
        with open(orig_path, "wb") as f:
            f.write(data)
    with open(os.path.join(OUT_DIR, f"{addr}_{value}.patched.bin"), "wb") as f:
        f.write(patched)
    print(f"元の記録 {len(data)} バイト → 書き換えた記録 {len(patched)} バイト（記述子 +{len(extra)} バイト、0x31 は {mt_bytes} バイトで宣言）")
    print(f"控え: {orig_path}")
    for line in describe_hid(find_hid_descriptor(parse_record(patched)).value):
        print("   " + line)


def cmd_apply(addr, value, which):
    path = os.path.join(OUT_DIR, f"{addr}_{value}.{which}.bin")
    with open(path, "rb") as f:
        data = f.read()
    parse_record(data)
    write_cached(addr, value, data)
    print(f"{addr}\\{value} に {which} の記録（{len(data)} バイト）を書きました")
    print("Bluetooth をいったんオフにしてオンにするか、トラックパッドの電源を入れ直して、つなぎ直してください")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "selftest":
        return cmd_selftest()
    if cmd == "dump":
        cmd_dump()
        return 0
    if cmd == "patch" and len(argv) in (4, 5):
        cmd_patch(argv[2].lower(), argv[3], int(argv[4]) if len(argv) == 5 else MT_INPUT_BYTES)
        return 0
    if cmd in ("apply", "restore") and len(argv) == 4:
        addr, value = argv[2].lower(), argv[3]
        if cmd == "patch":
            cmd_patch(addr, value)
        else:
            cmd_apply(addr, value, "patched" if cmd == "apply" else "orig")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
