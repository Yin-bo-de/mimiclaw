# CLAUDE.md — MimiClaw Project Notes

## idf.py 正确使用方式

本机 ESP-IDF 安装在 `~/.espressif/esp-idf-v5.5.2`，`idf.py` 不在全局 PATH 中。
有两种方式调用：

### 方式一：用 `get_idf` 别名激活环境（交互式终端推荐）

```bash
get_idf   # 等价于 . ~/.espressif/esp-idf-v5.5.2/export.sh
idf.py build
```

`get_idf` 是定义在 `~/.zshrc` 中的 shell alias，仅在交互式 shell 生效。

### 方式二：直接用绝对路径调用（脚本/工具调用推荐）

```bash
/Users/yinbo/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/yinbo/.espressif/esp-idf-v5.5.2/tools/idf.py <command>
```

这是在 Claude Code / 脚本中调用 idf.py 的可靠方式，不依赖 shell 环境。

### 常用命令

```bash
# 编译
idf.py build

# 全量清理后重编译（修改 mimi_secrets.h 或 sdkconfig 后必须执行）
idf.py fullclean && idf.py build

# 设置目标芯片（首次或切换目标时）
idf.py set-target esp32s3

# 烧录 + 串口监视（USB JTAG 口）
idf.py -p /dev/cu.usbmodem21201 flash monitor

# 仅烧录
idf.py -p /dev/cu.usbmodem21201 flash

# 仅监视（不烧录）
idf.py -p /dev/cu.usbserial-XXXX monitor
```

### 串口说明

ESP32-S3 有两个 USB-C 口，用途不同：

| 口 | 用途 |
|----|------|
| **USB**（JTAG） | `idf.py flash`、调试 |
| **COM**（UART） | 串口 REPL CLI（`idf.py monitor` 或 115200 串口工具） |

macOS 下查看可用端口：

```bash
ls /dev/cu.usb*
```

## 项目构建说明

- 目标芯片：ESP32-S3，16 MB Flash，8 MB PSRAM
- IDF 版本：v5.5.2
- Python 环境：`~/.espressif/python_env/idf5.5_py3.9_env`
- 烧录脚本参考：`flash.sh`
