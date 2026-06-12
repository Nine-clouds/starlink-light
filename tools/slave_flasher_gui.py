#!/usr/bin/env python3
"""
STC 从机地址烧录管理器 v2 — GUI
"""

import os, json, copy, subprocess, threading, shutil
import tkinter as tk
from tkinter import ttk, messagebox, filedialog

STATE_FILE = "addr_status.json"

# ==================== HEX ====================

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
    for i, (a, d) in enumerate(records):
        for j in range(len(d)-2):
            if d[j] == 0x75 and d[j+1] < 0x80:
                ba = bytearray(d)
                ba[j+2] = new_addr
                records[i] = (a, bytes(ba))
                return True
    return False

# ==================== 状态 ====================

def load_state():
    if os.path.exists(STATE_FILE):
        try:
            s = json.load(open(STATE_FILE))
            for a in range(0x01, 0xFF):
                s.setdefault(f"{a:02X}", "empty")
            return s
        except: pass
    s = {}
    for a in range(0x01, 0xFF):
        s[f"{a:02X}"] = "empty"
    save_state(s)
    return s

def save_state(s):
    json.dump(s, open(STATE_FILE, 'w'), indent=2, sort_keys=True)

def parse_addr(s):
    try:
        v = int(s.strip(), 16)
    except: return None
    return (v, f"{v:02X}") if 0x01 <= v <= 0xFE else None

def scan_ports():
    """扫描可用串口"""
    try:
        import serial.tools.list_ports
        return [p.device for p in serial.tools.list_ports.comports()]
    except:
        return []

# ==================== GUI ====================

class App:
    def __init__(self, root):
        self.root = root
        root.title("STC 从机地址烧录管理器 v2")
        root.geometry("420x520")
        root.resizable(False, False)

        self.records = None
        self.state = load_state()
        self.running = False

        self.build_ui()

    def build_ui(self):
        # ── 配置 ──
        f = ttk.LabelFrame(self.root, text="配置", padding=5)
        f.pack(fill='x', padx=8, pady=(8, 0))

        ttk.Label(f, text="固件").grid(row=0, column=0, sticky='w')
        self.hex_var = tk.StringVar()
        ttk.Entry(f, textvariable=self.hex_var, width=28, state='readonly').grid(row=0, column=1, padx=5)
        ttk.Button(f, text="浏览", command=self.browse).grid(row=0, column=2, padx=3)

        ttk.Label(f, text="串口").grid(row=1, column=0, sticky='w', pady=(5, 0))
        ports = scan_ports()
        default_port = ports[0] if ports else "COM3"
        self.port_var = tk.StringVar(value=default_port)
        self.port_cb = ttk.Combobox(f, textvariable=self.port_var, values=ports, width=8)
        self.port_cb.grid(row=1, column=1, padx=5, sticky='w', pady=(5, 0))
        ttk.Button(f, text="扫描", command=self._refresh_ports).grid(row=1, column=2, pady=(5, 0))

        self.dry_var = tk.BooleanVar()
        ttk.Checkbutton(f, text="仅生成不烧录", variable=self.dry_var).grid(row=2, column=1, columnspan=2, sticky='w', pady=(5, 0))

        # 进度条
        self.progress = ttk.Progressbar(self.root, mode='determinate')
        self.progress.pack(fill='x', padx=8, pady=(5, 0))

        # ── 菜单 ──
        m = ttk.LabelFrame(self.root, text="操作", padding=10)
        m.pack(fill='x', padx=8, pady=(8, 0))
        menu = [
            ("[1] 查看地址状态", self.op_view),
            ("[2] 烧录单个地址", self.op_burn_single),
            ("[3] 烧录地址范围", self.op_burn_range),
            ("[4] 清零地址", self.op_clear),
            ("[5] 全量清零", self.op_clear_all),
            ("[6] 退出", root.quit),
        ]
        for text, cmd in menu:
            ttk.Button(m, text=text, command=cmd).pack(fill='x', pady=2)

        # 统计
        self.stat_label = ttk.Label(self.root, text="")
        self.stat_label.pack(padx=8, pady=(5, 0))
        self._update_stat()

        # 日志
        log_f = ttk.LabelFrame(self.root, text="输出")
        log_f.pack(fill='both', expand=True, padx=8, pady=5)
        self.log = tk.Text(log_f, height=6, state='disabled')
        self.log.pack(fill='both', expand=True, padx=2, pady=2)

    def _refresh_ports(self):
        ports = scan_ports()
        self.port_cb['values'] = ports
        if ports:
            self.port_var.set(ports[0])

    def _update_stat(self):
        total = 254
        burned = sum(1 for v in self.state.values() if v == "burned")
        self.stat_label.config(text=f"共 {total} 地址，已烧 {burned}，未烧 {total - burned}")

    def put(self, msg):
        self.log.configure(state='normal')
        self.log.insert('end', msg + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    def browse(self):
        p = filedialog.askopenfilename(filetypes=[("HEX", "*.hex")])
        if p:
            self.hex_var.set(p)
            self.records = load_hex(p)
            self.put(f"加载: {os.path.basename(p)}")

    # ===== [1] =====
    def op_view(self):
        w = tk.Toplevel(self.root)
        w.title("地址状态 (绿=已烧 白=未烧)")
        w.geometry("750x500")
        canvas = tk.Canvas(w, highlightthickness=0)
        scroll = ttk.Scrollbar(w, orient="vertical", command=canvas.yview)
        grid = ttk.Frame(canvas)
        canvas.create_window((0, 0), window=grid, anchor="nw")
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True, padx=5, pady=5)
        scroll.pack(side='right', fill='y')
        for addr in range(1, 255):
            key = f"{addr:02X}"
            bg = "#90EE90" if self.state.get(key) == "burned" else "#f5f5f5"
            frm = tk.Frame(grid, bg=bg, relief='ridge', bd=1, width=42, height=24)
            frm.grid(row=(addr-1)//16, column=(addr-1)%16, padx=1, pady=1)
            frm.pack_propagate(False)
            tk.Label(frm, text=key, bg=bg, font=("Consolas", 8)).pack(expand=True)
        grid.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))

    # ===== [2] =====
    def op_burn_single(self):
        if self.running: return
        if not self.records:
            messagebox.showwarning("提示", "请先选择 .hex 文件"); return
        dlg = tk.Toplevel(self.root)
        dlg.title("烧录单个地址")
        dlg.geometry("220x110")
        dlg.transient(self.root); dlg.grab_set()
        ttk.Label(dlg, text="地址 (01-FE):").pack(padx=10, pady=(10,0))
        e = ttk.Entry(dlg, width=10, font=("", 14))
        e.pack(padx=10, pady=5); e.focus()
        def go():
            r = parse_addr(e.get())
            if not r: messagebox.showwarning("无效", f"地址无效: {e.get()}"); return
            addr, key = r
            if self.state.get(key) == "burned":
                if not messagebox.askyesno("确认", f"0x{key} 已烧录，覆盖?"): return
            dlg.destroy()
            threading.Thread(target=lambda: self._burn(addr), daemon=True).start()
        ttk.Button(dlg, text="烧录", command=go).pack(pady=5)
        dlg.bind('<Return>', lambda e: go())

    # ===== [3] =====
    def op_burn_range(self):
        if self.running: return
        if not self.records:
            messagebox.showwarning("提示", "请先选择 .hex 文件"); return
        dlg = tk.Toplevel(self.root)
        dlg.title("烧录地址范围")
        dlg.geometry("260x170")
        dlg.transient(self.root); dlg.grab_set()
        ttk.Label(dlg, text="起始 (01-FE):").pack(padx=10, pady=(10,0))
        e1 = ttk.Entry(dlg, width=10, font=("", 12)); e1.pack(padx=10, pady=3)
        ttk.Label(dlg, text="结束 (01-FE):").pack(padx=10, pady=(5,0))
        e2 = ttk.Entry(dlg, width=10, font=("", 12)); e2.pack(padx=10, pady=3)
        e1.focus()
        self._force_var = tk.BooleanVar()
        ttk.Checkbutton(dlg, text="强制覆盖已烧录", variable=self._force_var).pack(pady=5)
        def go():
            r1, r2 = parse_addr(e1.get()), parse_addr(e2.get())
            if not r1 or not r2: messagebox.showwarning("无效", "地址无效"); return
            s, e = min(r1[0], r2[0]), max(r1[0], r2[0])
            dlg.destroy()
            threading.Thread(target=lambda: self._burn_range(s, e, self._force_var.get()), daemon=True).start()
        ttk.Button(dlg, text="开始烧录", command=go).pack()
        dlg.bind('<Return>', lambda e: go())

    # ===== [4] =====
    def op_clear(self):
        dlg = tk.Toplevel(self.root)
        dlg.title("清零地址")
        dlg.geometry("250x100")
        dlg.transient(self.root); dlg.grab_set()
        ttk.Label(dlg, text="地址 (如 01 或 01-06):").pack(padx=10, pady=(10,0))
        e = ttk.Entry(dlg, width=12, font=("", 12)); e.pack(padx=10, pady=5); e.focus()
        def go():
            s = e.get().strip()
            if '-' in s:
                parts = s.split('-')
                r1, r2 = parse_addr(parts[0]), parse_addr(parts[1])
                if not r1 or not r2: messagebox.showwarning("无效", "范围无效"); return
                for a in range(min(r1[0], r2[0]), max(r1[0], r2[0])+1):
                    self.state[f"{a:02X}"] = "empty"
                save_state(self.state)
                self.put(f"已清零: {r1[1]}-{r2[1]}")
            else:
                r = parse_addr(s)
                if not r: messagebox.showwarning("无效", f"无效: {s}"); return
                self.state[r[1]] = "empty"
                save_state(self.state)
                self.put(f"已清零: 0x{r[1]}")
            self._update_stat(); dlg.destroy()
        ttk.Button(dlg, text="清零", command=go).pack(pady=5)
        dlg.bind('<Return>', lambda e: go())

    # ===== [5] =====
    def op_clear_all(self):
        if messagebox.askyesno("确认", "清除全部 254 个地址的烧录记录?"):
            for a in range(0x01, 0xFF):
                self.state[f"{a:02X}"] = "empty"
            save_state(self.state)
            self._update_stat()
            self.put("✓ 全部清零")

    # ===== 烧录 =====
    def _run_stcgal(self, addr, out):
        """调用 stcgal，返回 (success, error_msg)"""
        port = self.port_var.get()
        self.put(f"[0x{addr:02X}] {port} 烧录中...")
        cmd = f"stcgal -P auto -p {port} {out}"
        try:
            r = subprocess.run(cmd.split(), capture_output=True, text=True, timeout=60)
            if r.returncode == 0:
                return True, ""
            err = (r.stderr or r.stdout).strip()[:300]
            if "read timeout" in err.lower():
                return False, "连接超时 - 请断电后重新上电，或检查 DTR/RTS 接线"
            if "permission" in err.lower() or "occupied" in err.lower():
                return False, f"端口 {port} 被占用 - 请关闭 STC-ISP 等软件后重试"
            return False, err
        except FileNotFoundError:
            return False, "stcgal 未安装 - pip install stcgal"
        except Exception as ex:
            return False, str(ex)

    def _burn(self, addr):
        self.running = True
        key = f"{addr:02X}"
        os.makedirs("output", exist_ok=True)
        self.put(f"[0x{key}] 生成固件...")
        rec = copy.deepcopy(self.records)
        patch_addr(rec, addr)
        out = f"output/slave_addr{addr:02X}.hex"
        save_hex(out, rec)
        if self.dry_var.get():
            self.put(f"[0x{key}] [跳过烧录]")
            self.state[key] = "burned"
        else:
            ok, err = self._run_stcgal(addr, out)
            if ok:
                self.state[key] = "burned"
                self.put(f"[0x{key}] ✓ 完成")
            else:
                self.put(f"[0x{key}] ✗ {err}")
        save_state(self.state)
        self._update_stat()
        self.running = False

    def _burn_range(self, start, end, force=False):
        self.running = True
        total = end - start + 1
        self.progress.configure(maximum=total, value=0)
        os.makedirs("output", exist_ok=True)
        for addr in range(start, end + 1):
            key = f"{addr:02X}"
            if not force and self.state.get(key) == "burned":
                self.put(f"[0x{key}] 跳过"); self.progress.step(); continue
            self.put(f"[0x{key}] 生成...")
            rec = copy.deepcopy(self.records)
            patch_addr(rec, addr)
            out = f"output/slave_addr{addr:02X}.hex"
            save_hex(out, rec)
            if self.dry_var.get():
                self.put(f"[0x{key}] [跳过烧录]")
                self.state[key] = "burned"
            else:
                ok, err = self._run_stcgal(addr, out)
                if ok:
                    self.state[key] = "burned"
                    self.put(f"[0x{key}] ✓ 完成")
                else:
                    self.put(f"[0x{key}] ✗ {err}")
                    break
            save_state(self.state)
            self._update_stat()
            self.progress.step()
        self.put("─── 完成 ───")
        self.running = False

# ===== 启动 =====
def ensure_deps():
    missing = []
    if not shutil.which("stcgal"):
        try: import stcgal
        except ImportError: missing.append("stcgal")
    try: import serial
    except ImportError: missing.append("pyserial")
    if missing:
        r = messagebox.askyesno("安装依赖", f"缺少: {' '.join(missing)}\n自动安装?")
        if r:
            subprocess.check_call([__import__('sys').executable, "-m", "pip", "install"] + missing)
            messagebox.showinfo("完成", "安装完成，请重启软件")

if __name__ == "__main__":
    root = tk.Tk()
    root.withdraw()
    ensure_deps()
    root.deiconify()
    App(root)
    root.mainloop()
