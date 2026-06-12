#!/usr/bin/env python3
"""
STC 从机地址烧录管理器 v2
交互式管理 0x01~0xFE 地址的烧录状态，调用 stcgal 烧录 STC8H 从机。
"""

import os, sys, json, copy, shutil, subprocess, argparse, tempfile, platform

# ======================= 常量 =======================

VALID_RANGE = (0x01, 0xFE)          # 有效地址范围
BROADCAST_ADDR = 0xFF               # 广播地址，禁止烧录
DEFAULT_PORT = "COM3" if platform.system() == "Windows" else "/dev/ttyUSB0"
PAGE_SIZE = 20

# ======================= Intel HEX =======================

def load_hex(path):
    """加载 .hex，返回 [(addr, bytes), ...]"""
    records = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] != ':':
                continue
            b = bytes.fromhex(line[1:])
            if b[3] == 0x00:
                records.append(((b[1] << 8) | b[2], bytes(b[4:4 + b[0]])))
            elif b[3] == 0x01:
                break
    return sorted(records, key=lambda r: r[0])

def save_hex(path, records):
    """保存 records 为 .hex"""
    with open(path, 'w') as f:
        for addr, data in records:
            n = len(data)
            chk = (-(n + (addr >> 8) + (addr & 0xFF) + sum(data))) & 0xFF
            f.write(f":{n:02X}{addr:04X}00{data.hex().upper()}{chk:02X}\n")
        f.write(":00000001FF\n")

def patch_device_addr(records, new_addr):
    """
    搜索 mov DEVICE_ADDR, #imm (0x75 xx old) 并替换 old -> new_addr。
    返回 True 如果成功修补。
    """
    for i, (addr, data) in enumerate(records):
        for j in range(len(data) - 2):
            if data[j] == 0x75 and data[j + 1] < 0x80:
                ba = bytearray(data)
                ba[j + 2] = new_addr
                records[i] = (addr, bytes(ba))
                return True
    return False

# ======================= 状态管理 =======================

def load_status(path):
    """加载地址状态 JSON，不存在则初始化"""
    if os.path.exists(path):
        try:
            with open(path) as f:
                state = json.load(f)
            # 补齐可能缺失的地址
            for addr in range(0x01, 0xFF):
                state.setdefault(f"{addr:02X}", "empty")
            return state
        except (json.JSONDecodeError, KeyError):
            print(f"[警告] 状态文件损坏，重新初始化")
    # 初始化
    state = {}
    for addr in range(0x01, 0xFF):
        state[f"{addr:02X}"] = "empty"
    save_status(path, state)
    return state

def save_status(path, state):
    """保存状态到 JSON"""
    with open(path, 'w') as f:
        json.dump(state, f, indent=2, sort_keys=True)

# ======================= 地址校验 =======================

def parse_addr(s):
    """解析十六进制地址字符串，返回 (int, key_string) 或 None"""
    try:
        val = int(s.strip(), 16)
    except ValueError:
        return None
    if val < 0x01 or val > 0xFE:
        return None
    return val, f"{val:02X}"

# ======================= 烧录逻辑 =======================

def gen_patched_hex(base_hex_path, addr, output_dir=None):
    """
    从基址 .hex 生成指定地址的临时 .hex。
    返回临时文件路径。
    """
    records = load_hex(base_hex_path)
    ok = patch_device_addr(records, addr)
    if not ok:
        sys.stderr.write(f"[错误] 未在固件中找到 DEVICE_ADDR，请确认编译正确\n")
        sys.exit(1)

    if output_dir:
        out = os.path.join(output_dir, f"slave_addr{addr:02X}.hex")
    else:
        fd, out = tempfile.mkstemp(suffix=f"_addr{addr:02X}.hex", prefix="slave_")
        os.close(fd)
    save_hex(out, records)
    return out

def run_stcgal(port, hex_path, dry_run=False):
    """调用 stcgal 烧录，返回 (success, output)"""
    cmd = f"stcgal -P {port} -b 115200 -p {hex_path}"
    print(f"    >>> {cmd}")
    if dry_run:
        return True, "[DRY-RUN] 跳过烧录"
    try:
        r = subprocess.run(cmd.split(), capture_output=True, text=True, timeout=60)
        if r.returncode == 0:
            return True, r.stdout.strip()
        else:
            return False, r.stderr.strip() or r.stdout.strip()
    except FileNotFoundError:
        return False, "stcgal 未安装，请执行 pip install stcgal"
    except subprocess.TimeoutExpired:
        return False, "烧录超时 (60s)"
    except Exception as e:
        return False, str(e)

def check_port(port):
    """检查串口是否存在"""
    if port.startswith("COM"):
        if platform.system() == "Windows":
            import serial.tools.list_ports
            available = [p.device for p in serial.tools.list_ports.comports()]
            if port not in available:
                print(f"[警告] 串口 {port} 未检测到，可能无法使用")
                return False
            return True
    return True  # Linux/macOS 不做检查

# ======================= 显示 =======================

def show_page(state, page=1):
    """分页显示地址状态，返回总页数"""
    start = (page - 1) * PAGE_SIZE
    total = (0xFE - 0x01 + 1)
    total_pages = (total + PAGE_SIZE - 1) // PAGE_SIZE

    print(f"\n  地址状态 (第 {page}/{total_pages} 页):")
    print(f"  {'─' * 30}")
    count = 0
    for addr in range(0x01, 0xFF):
        if count < start:
            count += 1
            continue
        if count >= start + PAGE_SIZE:
            break
        key = f"{addr:02X}"
        mark = "✅ 已烧录" if state[key] == "burned" else "⬜ 未烧录"
        print(f"    {key}: {mark}")
        count += 1
    print(f"  {'─' * 30}")
    return total_pages

# ======================= 操作函数 =======================

def do_view(state, state_path):
    """查看地址状态（支持翻页）"""
    page = 1
    while True:
        total = show_page(state, page)
        if total <= 1:
            input("\n  按回车返回菜单...")
            return
        print(f"\n  [N]下一页  [P]上一页  [Q]返回菜单")
        c = input("  > ").strip().lower()
        if c == 'n' and page < total:
            page += 1
        elif c == 'p' and page > 1:
            page -= 1
        elif c == 'q':
            return

def do_burn_single(state, state_path, args):
    """烧录单个地址"""
    s = input("  输入地址 (01-FE): ").strip()
    result = parse_addr(s)
    if not result:
        print(f"  [错误] 无效地址: {s}")
        return

    val, key = result
    port = args.port

    if state[key] == "burned":
        ans = input(f"  地址 0x{key} 已烧录，覆盖? (y/N): ").strip().lower()
        if ans != 'y':
            print("  已取消")
            return

    # 生成临时 hex
    print(f"  生成 0x{key} 固件...")
    tmp = gen_patched_hex(args.hexfile, val)

    print(f"  烧录 0x{key} (串口 {port})...")
    ok, msg = run_stcgal(port, tmp, args.dry_run)
    print(f"    {msg}")

    if ok:
        state[key] = "burned"
        save_status(state_path, state)
        print(f"  ✓ 0x{key} 烧录完成")

    # 清理临时文件
    if not args.keep_temp:
        try:
            os.remove(tmp)
        except OSError:
            pass

def do_burn_range(state, state_path, args):
    """烧录地址范围"""
    s_start = input("  起始地址 (01-FE): ").strip()
    s_end = input("  结束地址 (01-FE): ").strip()

    r1 = parse_addr(s_start)
    r2 = parse_addr(s_end)
    if not r1 or not r2:
        print(f"  [错误] 地址无效")
        return

    start, _ = r1
    end, _ = r2
    if start > end:
        start, end = end, start

    skip_burned = not args.force
    port = args.port
    count = 0

    for addr in range(start, end + 1):
        key = f"{addr:02X}"
        label = f"0x{addr:02X}"

        if skip_burned and state[key] == "burned":
            print(f"  [{label}] 跳过 (已烧录)")
            continue

        print(f"  [{label}] 生成固件...")
        tmp = gen_patched_hex(args.hexfile, addr)

        print(f"  [{label}] 烧录中...")
        ok, msg = run_stcgal(port, tmp, args.dry_run)
        print(f"    {msg}")

        if ok:
            state[key] = "burned"
            save_status(state_path, state)
            count += 1
            print(f"    ✓ 完成")
        else:
            print(f"    ✗ 失败，停止")
            try:
                os.remove(tmp)
            except OSError:
                pass
            break

        if not args.keep_temp:
            try:
                os.remove(tmp)
            except OSError:
                pass

    print(f"\n  完成: {count} 个地址已烧录")

def do_clear(state, state_path, args):
    """清零地址"""
    print(f"  [S] 单个地址  [R] 范围  [Q] 返回")
    c = input("  > ").strip().lower()

    if c == 's':
        s = input("  输入地址: ").strip()
        r = parse_addr(s)
        if not r:
            print(f"  [错误] 无效地址")
            return
        _, key = r
        state[key] = "empty"
        save_status(state_path, state)
        print(f"  ✓ 0x{key} 已清零 (状态重置)")
    elif c == 'r':
        s1 = input("  起始: ").strip()
        s2 = input("  结束: ").strip()
        r1, r2 = parse_addr(s1), parse_addr(s2)
        if not r1 or not r2:
            print(f"  [错误] 地址无效")
            return
        for addr in range(min(r1[0], r2[0]), max(r1[0], r2[0]) + 1):
            state[f"{addr:02X}"] = "empty"
        save_status(state_path, state)
        print(f"  ✓ 范围已清零")
    elif c == 'q':
        return

def do_clear_all(state, state_path, args):
    """全量清零"""
    ans = input(f"  确认将全部 254 个地址重置为未烧录? (yes/no): ").strip()
    if ans.lower() != 'yes':
        print("  已取消")
        return
    for addr in range(0x01, 0xFF):
        state[f"{addr:02X}"] = "empty"
    save_status(state_path, state)
    print("  ✓ 全部清零完成")

# ======================= 主菜单 =======================

MENU = """
  ╔══════════════════════════════════╗
  ║  STC 从机地址烧录管理器 v2      ║
  ╠══════════════════════════════════╣
  ║  [1] 查看地址状态               ║
  ║  [2] 烧录单个地址               ║
  ║  [3] 烧录地址范围               ║
  ║  [4] 清零地址                   ║
  ║  [5] 全量清零                   ║
  ║  [6] 退出                       ║
  ╚══════════════════════════════════╝"""

FUNCTIONS = {
    '1': do_view,
    '2': do_burn_single,
    '3': do_burn_range,
    '4': do_clear,
    '5': do_clear_all,
}

def main():
    parser = argparse.ArgumentParser(description="STC 从机地址烧录管理器")
    parser.add_argument("hexfile", nargs='?', help="基址 .hex 文件路径")
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"串口号 (默认: {DEFAULT_PORT})")
    parser.add_argument("--force", action="store_true", help="覆盖已烧录地址")
    parser.add_argument("--dry-run", action="store_true", help="只打印不烧录")
    parser.add_argument("--status", default="addr_status.json", help="状态文件路径")
    parser.add_argument("--keep-temp", action="store_true", help="保留临时 .hex 文件")
    args = parser.parse_args()

    # 检查 stcgal
    if not args.dry_run and not shutil.which("stcgal"):
        print("[警告] stcgal 未安装，烧录功能不可用。pip install stcgal")

    # 加载状态
    state_path = args.status
    state = load_status(state_path)

    # 主循环
    while True:
        print(MENU)
        if args.hexfile:
            print(f"  固件: {args.hexfile}")
        print(f"  串口: {args.port}  {'[DRY-RUN]' if args.dry_run else ''}")
        c = input("  请选择 (1-6): ").strip()

        if c == '6':
            save_status(state_path, state)
            print("  已保存，再见。")
            break
        elif c in FUNCTIONS:
            if c in ('2', '3') and not args.hexfile:
                print("  [错误] 请指定 .hex 文件: --hexfile path/to/firmware.hex")
                input("  按回车继续...")
                continue
            FUNCTIONS[c](state, state_path, args)
        else:
            print("  无效选项")

if __name__ == "__main__":
    main()
