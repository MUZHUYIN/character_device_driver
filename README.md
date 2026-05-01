# 基于树莓派 5 的 SPI 字符设备驱动程序开发

这是一个面向树莓派 5 的完整 SPI LCD 驱动项目，目标是把 `ILI9341` 320x240 TFT 屏集成进 Linux 内核显示链路，同时保留一个便于实验和课程演示的字符设备接口。

项目提供两条用户空间访问路径：

- `fbdev`：驱动注册 `/dev/fbX`，支持标准 framebuffer `write`、`mmap` 和基础图元操作。
- `char device`：驱动注册 `/dev/rpi5_ili9341`，支持 `read`、`write`、`mmap`、`ioctl`，便于实验验证和自定义控制。

## 功能特性

- 基于 `spi_driver` 的 ILI9341 内核模块，面向树莓派 5 SPI0。
- 内核级帧缓冲显存，像素格式为 `RGB565`。
- 自定义字符设备接口，支持整帧刷新和局部刷新。
- `fb_deferred_io` 延迟刷新路径，适合 `/dev/fbX` 的 `mmap` 绘图。
- 可选 TE GPIO 中断同步，降低撕裂风险。
- 提供设备树 overlay、用户态 demo、DKMS 配置和构建脚本。
- 已兼容树莓派 `6.12.y` 内核中的 `FBINFO_FLAG_DEFAULT` 差异。
- 已修复字符设备 `mmap()` 页对齐和 `vmalloc_user()` 映射问题。

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

## 适用硬件

本项目只适用于“真正的 SPI 接口 ILI9341 显示屏”，模块通常会直接暴露以下显示引脚：

- `VCC`
- `GND`
- `CS`
- `RST`
- `DC/RS/A0`
- `MOSI/SDA`
- `SCK/CLK`
- 可选 `LED`
- 可选 `MISO`
- 可选 `TE`

### 已确认不适用的模块

`LCDWiki MRB2801` 这类模块不适用于本项目。它虽然使用 ILI9341 控制器，但显示接口是 `8-bit/16-bit 并口`，不是 SPI 显示总线；板上的 `MOSI/MISO/CLK` 引脚是触摸芯片的 SPI，不是 LCD 显示数据引脚。

如果你手上的屏幕页面写的是以下关键词，就不应该用这个仓库直接点屏：

- `8-bit parallel`
- `16-bit parallel`
- `8080 interface`
- `MCU parallel`

## 参考接线

以下是 `overlay/rpi5-ili9341-spi0.dts` 的示例接线，GPIO 编号使用 BCM 编号：

| LCD 引脚 | 树莓派 5 | 说明 |
| --- | --- | --- |
| `VCC` | `3V3` | 模块供电 |
| `GND` | `GND` | 地 |
| `SCK` | `GPIO11 / SPI0_SCLK` | SPI 时钟 |
| `MOSI` | `GPIO10 / SPI0_MOSI` | SPI 数据输出 |
| `MISO` | 可不接 | 多数 ILI9341 显示不用 |
| `CS` | `GPIO8 / SPI0_CE0` | SPI 片选 |
| `DC` | `GPIO24` | 数据/命令选择 |
| `RST` | `GPIO25` | 硬件复位 |
| `LED` | `GPIO18` | 背光控制 |
| `TE` | `GPIO23` | 可选撕裂同步输入 |

注意：`GPIO24` 不是物理针脚 `24`，`GPIO25` 也不是物理针脚 `25`。排线时请核对 BCM 编号和 40Pin 实体引脚表。

## 构建

在树莓派 5 本机执行：

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
sudo ./tools/ili9341_demo solid-blue
sudo ./tools/ili9341_demo clear 0000
sudo ./tools/ili9341_demo rotate 270
```

如果系统里已经有其它 framebuffer，请先用下面这条命令确认本驱动对应的 `fb` 编号：

```bash
dmesg | grep rpi5_ili9341
```

## 字符设备接口

头文件见 `include/uapi/linux/rpi5_ili9341.h`。

支持的 `ioctl`：

- `RPI5_ILI9341_IOC_GET_INFO`：获取当前分辨率、stride、bpp、旋转和 TE 能力。
- `RPI5_ILI9341_IOC_FLUSH`：刷新整帧。
- `RPI5_ILI9341_IOC_FLUSH_RECT`：刷新指定矩形区域。
- `RPI5_ILI9341_IOC_CLEAR`：内核态清屏。
- `RPI5_ILI9341_IOC_SET_ROTATION`：设置 `0/90/180/270` 旋转。
- `RPI5_ILI9341_IOC_SET_BACKLIGHT`：开关背光 GPIO。
- `RPI5_ILI9341_IOC_WAIT_TE`：等待一次 TE 中断。

## 设备树说明

仓库里的 overlay 是“项目默认示例”，并不是所有模块都能原样即用。不同 SPI 模块常见差异包括：

- `dc-gpios` 极性不同
- `reset-gpios` 极性不同
- `led-gpios` 极性不同
- 最高稳定 SPI 频率不同
- 模块物理安装方向不同

如果点屏失败，优先尝试：

1. 把 `spi-max-frequency` 从 `32000000` 降到 `16000000`
2. 单独翻转 `dc-gpios`
3. 单独翻转 `reset-gpios`
4. 调整 `rotation = <0|90|180|270>;`

## 调试建议

- 黑屏：优先检查 `DC`、`RST`、`LED`、`CS` 接线。
- 白屏：优先检查屏幕是否真的是 SPI 显示模块，而不是并口模块。
- 花屏：降低 `spi-max-frequency`。
- 方向错误：修改 `rotation` 或运行 `./tools/ili9341_demo rotate 90`。
- 背光不亮：先临时把 `LED` 引脚直接接 `3V3` 验证硬件。

## 参考资料

- [Linux SPI 子系统文档](https://docs.kernel.org/spi/index.html)
- [Linux framebuffer 文档](https://docs.kernel.org/fb/index.html)
- [Raspberry Pi Linux 设备树仓库](https://github.com/raspberrypi/linux)
- [ILI9341 数据手册](https://www.buydisplay.com/download/ic/ILI9341.pdf)
- [LCDWiki MRB2801 页面](https://www.lcdwiki.com/zh/2.8inch_16BIT_Module_ILI9341_SKU:MRB2801)
