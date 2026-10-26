# 轻量级可行性探针（C / Win32）

用来回答一个问题：**如果用 C 重写这个托盘工具，能轻到什么程度？**
这里只有一个 60 行的最小托盘程序（图标 + 右键菜单 + 气泡 + 800ms 定时器），
不含 HID 和自绘面板 —— 目的是量**运行时底座**，不是功能验证。

## 编译

```bash
# 工具链：D:\tools\env\mingw64 (GCC 16.2.0, x86_64-w64-mingw32)
gcc -O2 -s -mwindows -municode -static -o tray_probe.exe tray_probe.c -lshell32
```

- `-mwindows` 不要控制台窗口
- `-municode` 用 `wWinMain` 时必须加，否则链接报 `undefined reference to WinMain`
- `-static` 静态链接运行库，exe 拷到任何 Win10/11 都能直接跑，**不需要装任何东西**
- 源码里必须 `#define UNICODE` / `_UNICODE`，否则 `IDI_APPLICATION` 会展开成
  `MAKEINTRESOURCEA`（`CHAR*`），`LoadIconW` 直接报 incompatible pointer type

## 实测（2026-10-26，Windows 11 x64）

| 指标 | Python 版（现状） | C 探针 |
|---|---:|---:|
| 可执行文件 | 10~14 MB（PyInstaller 打包后） | **17,920 字节** |
| 私有提交 | 18~19 MB | **2.07 MB** |
| 工作集 | 30 MB（活跃）/ 2.5 MB（空闲修剪后） | **10.5 MB**（大部分是共享的系统 DLL） |
| GDI / USER 句柄 | 37 / 18 | 0 / 3 |
| 线程 | 4~10 | 4 |

私有内存约为 Python 版的 **1/9**，exe 体积小约 **600 倍**。

## 复测

```bash
python measure.py tray_probe.exe 3
```

`measure.py` 是通用的：给任意 exe 路径都能量私有内存、工作集、GDI/USER、线程数。
