# 工具集

## slave_flasher_gui.py — 从机烧录工具 (主)

图形界面，编译一次固件，自动改 DEVICE_ADDR，批量烧录。

### 启动

```bash
python3 tools/slave_flasher_gui.py
```

### 打包 EXE

```bash
pip install pyinstaller stcgal
pyinstaller --onefile --name "STC8H_Burn_Tool" tools/slave_flasher_gui.py
# 输出 dist/STC8H_Burn_Tool.exe
```

### 功能

| 按钮 | 说明 |
|------|------|
| [1] 查看地址状态 | 弹窗显示 0x01~0xFE 网格 (绿=已烧, 白=未烧) |
| [2] 烧录单个地址 | 输入地址 (01-FE)，已烧录提示覆盖 |
| [3] 烧录地址范围 | 输入起始/结束，逐个烧录，已烧录自动跳过 |
| [4] 清零地址 | 单个或范围 (如 01-06) |
| [5] 全量清零 | 清除全部烧录记录 |
| [6] 退出 | 保存状态 |

### 前置

```bash
pip install stcgal pyserial
```

## slave_flasher.py — 命令行版 (辅助)

```bash
python3 tools/slave_flasher.py slave/xxx.hex --flash COM3
```

## addr_manager.py — 地址管理器 CLI (辅助)

```bash
python3 tools/addr_manager.py slave/xxx.hex --port COM3
```
