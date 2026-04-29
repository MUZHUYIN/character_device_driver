# 基于树莓派5的 SPI 字符设备驱动程序开发

这是一个面向树莓派5的完整 SPI LCD 驱动项目，目标是把 `ILI9341` 320x240 TFT 屏深度集成进 Linux 内核显示链路，同时保留一个易于实验的字符设备入口。

项目同时提供两条用户空间访问路径：

- `fbdev`：驱动注册 `/dev/fbX`，支持标准 framebuffer 写入、`mmap` 和基本图元操作。
- `char device`：驱动注册 `/dev/rpi5_ili9341`，支持 `read`、`write`、`mmap`、`ioctl`，便于课程实验和定制控制。

## 功能特性

- 基于 `spi_driver` 的 ILI9341 内核模块，适配树莓派5 SPI0。
- 内核级帧缓冲显存，像素格式为 `RGB565`。
- 自定义字符设备接口，支持整帧/局部刷新。
- `fb_deferred_io` 延迟刷新路径，适合 `/dev/fbX` 的 `mmap` 绘图。
- 可选 TE GPIO 中断同步，降低撕裂风险。
- 设备树 overlay、用户态 demo、DKMS 配置、构建脚本一并提供。

## 目录结构

```text
.
├── Makefile
├── README.md
├── dkms.conf
├── docs
│   ├── architecture.md
│   └── bringup.md
├── include/uapi/linux/rpi5_ili9341.h
├── overlay/rpi5-ili9341-spi0.dts
├── src/rpi5_ili9341.c
└── tools/ili9341_demo.c
```

## 硬件连接

以下是默认 overlay 对应的接线，GPIO 编号使用树莓派5 40Pin Header 的 BCM 编号：

| LCD 引脚 | 树莓派5 | 说明 |
| --- | --- | --- |
| `VCC` | `3V3` | 模块供电 |
| `GND` | `GND` | 地 |
| `SCK` | `GPIO11 / SPI0_SCLK` | SPI 时钟 |
| `MOSI` | `GPIO10 / SPI0_MOSI` | SPI 数据输出 |
| `MISO` | 可不接 | ILI9341 显示常用不到 |
| `CS` | `GPIO8 / SPI0_CE0` | SPI 片选 |
| `DC` | `GPIO24` | 数据/命令选择 |
| `RST` | `GPIO25` | 硬件复位 |
| `LED` | `GPIO18` | 背光控制 |
| `TE` | `GPIO23` | 可选撕裂同步输入 |

如果你的屏模块没有导出 `TE`，可以直接删除 `overlay/rpi5-ili9341-spi0.dts` 里的 `te-gpios` 一行。

## 构建

在树莓派5 本机执行：

```bash
sudo apt update
sudo apt install -y raspberrypi-kernel-headers device-tree-compiler build-essential
make
```

单独构建：

```bash
make module
make tools
make dtbo
```

## 部署

1. 复制 overlay 到系统目录：

```bash
sudo cp overlay/rpi5-ili9341-spi0.dtbo /boot/firmware/overlays/
```

2. 编辑 `/boot/firmware/config.txt`，加入：

```ini
dtparam=spi=on
dtoverlay=rpi5-ili9341-spi0
```

3. 重启后加载驱动：

```bash
sudo insmod rpi5_ili9341.ko
```

4. 查看设备：

```bash
dmesg | tail -n 20
ls -l /dev/fb*
ls -l /dev/rpi5_ili9341
```

## 用户空间验证

字符设备 demo：

```bash
make tools
sudo ./tools/ili9341_demo info
sudo ./tools/ili9341_demo bars
sudo ./tools/ili9341_demo gradient
sudo ./tools/ili9341_demo clear 0000
sudo ./tools/ili9341_demo rotate 270
```

直接写 framebuffer：

```bash
sudo sh -c 'cat /dev/zero > /dev/fb0'
```

如果系统里已经有其它 framebuffer，请先用 `dmesg | grep rpi5_ili9341` 确认本驱动对应的 `fb` 编号。

## 字符设备接口

头文件见 `include/uapi/linux/rpi5_ili9341.h`。

支持的 ioctl：

- `RPI5_ILI9341_IOC_GET_INFO`：获取当前分辨率、stride、bpp、旋转和 TE 能力。
- `RPI5_ILI9341_IOC_FLUSH`：刷新整帧。
- `RPI5_ILI9341_IOC_FLUSH_RECT`：刷新指定矩形区域。
- `RPI5_ILI9341_IOC_CLEAR`：内核态清屏。
- `RPI5_ILI9341_IOC_SET_ROTATION`：设置 `0/90/180/270` 旋转。
- `RPI5_ILI9341_IOC_SET_BACKLIGHT`：开关背光 GPIO。
- `RPI5_ILI9341_IOC_WAIT_TE`：等待一次 TE 中断。

## 设计说明

- 显存常驻内核 `vmalloc` 空间，大小固定为 `320 * 240 * 2 = 153600` 字节。
- `/dev/fbX` 走 `fbdev` 标准路径，`mmap` 写显存后由 `fb_deferred_io` 触发刷新。
- `/dev/rpi5_ili9341` 既能 `write` 原始 RGB565，也能 `mmap` 后配合 `ioctl(FLUSH)` 做零拷贝显示。
- SPI 发包前会把 CPU 本地小端 `RGB565` 转成 ILI9341 期望的高字节在前格式。

## 调试建议

- 无显示：先确认 `dc-gpios` 和 `reset-gpios` 是否接对，这两个脚最容易导致黑屏。
- 花屏：把 `spi-max-frequency` 从 `32000000` 降到 `16000000` 再试。
- 显示方向错：执行 `./tools/ili9341_demo rotate 90` 或调整 overlay 里的 `rotation`。
- 刷新撕裂：如果模块有 `TE` 引脚，保留 `te-gpios` 并在设备树里增加 `sync-to-te;`。

## 参考资料

- Linux SPI 子系统文档：https://docs.kernel.org/spi/index.html
- Linux framebuffer 文档：https://docs.kernel.org/fb/index.html
- Raspberry Pi Linux 设备树仓库：https://github.com/raspberrypi/linux
- ILI9341 数据手册：https://www.buydisplay.com/download/ic/ILI9341.pdf
