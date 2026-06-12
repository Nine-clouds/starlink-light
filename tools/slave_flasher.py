#!/usr/bin/env python3
"""
STC8H 从机烧写工具 — 简洁交互式
用法:
  python slave_flasher.py slave.hex              # 只生成 6 个 .hex
  python slave_flasher.py slave.hex --flash COM3  # 生成 + 烧录
  python slave_flasher.py slave.hex --start 1 --end 10  # 指定范围
  python slave_flasher.py slave.hex --clear      # 清零模式
"""

import os, sys, json, copy, subprocess, argparse

STATE_FILE = "slave_burn_state.json"

# ──── Intel HEX ────

def load_hex(path):
    records = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] != ':': continue
            b = bytes.fromhex(line[1:])
            if b[3] == 0x00:
                records.append(((b[1]<<8)|b[2], b[4:4+b[0]]))
            elif b[3] == 0x01:
                break
    return sorted(records, key=lambda r: r[0])

def save_hex(path, records):
    with open(path, 'w') as f:
        for addr, data in records:
            n = len(data)
            chk = (-(n + (addr>>8) + (addr&0xFF) + sum(data))) & 0xFF
            f.write(f":{n:02X}{addr:04X}00{data.hex().upper()}{chk:02X}\n")
        f.write(":00000001FF\n")

def patch_addr(records, new_addr):
    """修改 DEVICE_ADDR (mov direct, #imm = 0x75 xx old)"""
    for i, (a, d) in enumerate(records):
        for j in range(len(d)-2):
            if d[j] == 0x75 and d[j+1] < 0x80:
                ba = bytearray(d)
                ba[j+2] = new_addr
                records[i] = (a, bytes(ba))
                return True
    return False

# ──── 状态管理 ────

def load_state():
    return json.load(open(STATE_FILE)) if os.path.exists(STATE_FILE) else {}

def save_state(s):
    json.dump(s, open(STATE_FILE, 'w'))

# ──── 烧录 ────

def flash(hex_path, port, baud=115200):
    """调用 stcgal 烧录"""
    cmd = f"stcgal -P {port} -b {baud} -p {hex_path}"
    print(f"  >>> {cmd}")
    r = subprocess.run(cmd.split(), capture_output=True, text=True, timeout=60)
    if r.returncode == 0:
        print(f"  ✓ 烧录成功\n")
        return True
    else:
        print(f"  ✗ 失败: {r.stderr.strip()}\n")
        return False

# ──── 主逻辑 ────

def main():
    parser = argparse.ArgumentParser(description="STC8H 从机批量烧写")
    parser.add_argument("hexfile", help="编译好的 .hex 文件")
    parser.add_argument("--start", type=lambda x: int(x,0), default=1, help="起始地址 (默认 1)")
    parser.add_argument("--end", type=lambda x: int(x,0), default=6, help="结束地址 (默认 6)")
    parser.add_argument("--clear", action="store_true", help="清零模式 (DEVICE_ADDR=0x00)")
    parser.add_argument("--flash", help="串口 (如 COM3)，自动烧录")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", default="output")
    args = parser.parse_args()

    records = load_hex(args.hexfile)
    state = load_state()
    os.makedirs(args.output, exist_ok=True)

    print(f"固件: {args.hexfile}")
    print(f"地址: 0x{args.start:02X} ~ 0x{args.end:02X}")
    print(f"模式: {'清零' if args.clear else '写入地址'}")
    print(f"-" * 40)

    for addr in range(args.start, args.end + 1):
        key = f"{addr:02X}"
        label = f"0x{addr:02X}"

        if not args.clear and state.get(key) == "burned":
            print(f"[{label}] 跳过 (已烧录)")
            continue

        rec = copy.deepcopy(records)
        ok = patch_addr(rec, 0x00 if args.clear else addr)

        if args.clear:
            out = f"{args.output}/slave_cleared.hex"
        else:
            out = f"{args.output}/slave_addr{addr:02X}.hex"

        save_hex(out, rec)
        state[key] = "done" if args.clear else "burned"
        save_state(state)
        print(f"[{label}] 生成 -> {out}")

        if args.flash:
            input(f"  连接从机 [{label}]，按 Enter 烧录...")
            flash(out, args.flash, args.baud)

    print(f"-" * 40)
    print(f"完成，文件在 {args.output}/")

if __name__ == "__main__":
    main()
