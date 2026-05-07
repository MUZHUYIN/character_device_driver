# 基于树莓派 5 的 SPI 字符设备驱动程序开发

这是一个面向树莓派 5 的完整 SPI LCD 驱动项目，目标是把 `ILI9341` 240x320 TFT 屏集成进 Linux 内核显示链路，同时保留一个便于实验和课程演示的字符设备接口。

项目提供两条用户空间访问路径：

- `fbdev`：驱动注册 `/dev/fbX`，支持标准 framebuffer `write`、`mmap` 和基础图元操作。
- `char device`：驱动注册 `/dev/rpi5_ili9341`，支持 `read`、`write`、`mmap`、`ioctl`，便于实验验证和自定义控制。

## 功能特性

- 基于 `spi_driver` 的 ILI9341 内核模块，适配树莓派 5 SPI0。
- 默认竖屏显示，分辨率 `240x320`，像素格式为 `RGB565`。
- 自定义字符设备接口，支持整帧刷新和局部刷新。
- `fb_deferred_io` 延迟刷新路径，适合 `/dev/fbX` 的 `mmap` 绘图。
- 可选 TE GPIO 中断同步，降低撕裂风险。
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

本项目适用于“真正的 SPI 接口 ILI9341 显示屏”。模块通常会直接暴露以下显示引脚：

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

如果模块页面写的是以下关键词，就不应直接用本仓库点屏：

- `8-bit parallel`
- `16-bit parallel`
- `8080 interface`
- `MCU parallel`

## 实测默认接线

以下配置对应当前仓库默认 overlay，GPIO 编号使用 BCM 编号：

| LCD 引脚 | 树莓派 5 | 说明 |
| --- | --- | --- |
| `VCC` | `3V3` | 模块供电 |
| `GND` | `GND` | 地 |
| `LED/BL` | `3V3` | 背光直连，先确保稳定点亮 |
| `SCK` | `GPIO11 / SPI0_SCLK` | SPI 时钟 |
| `MOSI` | `GPIO10 / SPI0_MOSI` | SPI 数据输出 |
| `CS` | `GPIO8 / SPI0_CE0` | SPI 片选 |
| `DC` | `GPIO24` | 数据/命令选择 |
| `RST` | `GPIO25` | 硬件复位 |

注意：`GPIO24` 不是物理针脚 `24`，`GPIO25` 也不是物理针脚 `25`。排线时请核对 BCM 编号和 40Pin 实体引脚表。

当前最终默认接法中，`LED/BL` 直接接 `3V3`，不经过 GPIO 控制。这意味着背光会常亮，但能最大程度保证点屏稳定性。

## 默认显示配置

当前仓库默认使用这组已实测点亮的配置：

- `spi-max-frequency = <16000000>;`
- `dc-gpios = <&rp1_gpio 24 0>;`
- `reset-gpios = <&rp1_gpio 25 1>;`
- `rotation = <0>;`
- `bgr;`

这组配置默认输出竖屏 `240x320`。

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
sudo ./tools/ili9341_demo clear f800
sudo ./tools/ili9341_demo bars
sudo ./tools/ili9341_demo gradient
```

如果你想切换方向：

```bash
sudo ./tools/ili9341_demo rotate 90
sudo ./tools/ili9341_demo rotate 180
sudo ./tools/ili9341_demo rotate 270
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

说明：当前默认接线把背光直接接到了 `3V3`，所以如果没有额外接入 `led-gpios`，`SET_BACKLIGHT` 可能返回 `-EOPNOTSUPP`，这是正常现象。

## 调试建议

- 白屏：优先检查屏幕是否真的是 SPI 显示模块，以及 `DC/RST/CS` 接线是否正确。
- 黑屏但背光不亮：优先检查 `LED/BL` 是否已接到 `3V3`。
- 花屏：把 `spi-max-frequency` 从 `16000000` 再降到 `8000000`。
- 方向错误：修改 `rotation`，或者用 `./tools/ili9341_demo rotate <角度>` 运行时切换。

## 参考资料

- [Linux SPI 子系统文档](https://docs.kernel.org/spi/index.html)
- [Linux framebuffer 文档](https://docs.kernel.org/fb/index.html)
- [Raspberry Pi Linux 设备树仓库](https://github.com/raspberrypi/linux)
- [ILI9341 数据手册](https://www.buydisplay.com/download/ic/ILI9341.pdf)
