# 上板与验证步骤

## 1. 接线

先按最小系统接线：

- `VCC -> 3V3`
- `GND -> GND`
- `LED/BL -> 3V3`
- `SCK -> GPIO11`
- `MOSI -> GPIO10`
- `CS -> GPIO8`
- `DC -> GPIO24`
- `RST -> GPIO25`

先不要接 `MISO`、`TE`、触摸和 TF 卡相关引脚。

说明：当前项目最终默认方案里，`LED/BL` 保持直接接 `3V3`。这样背光不会由驱动控制，但点亮最稳定，也最适合课程演示和项目交付。

## 2. 安装依赖

```bash
sudo apt update
sudo apt install -y raspberrypi-kernel-headers build-essential device-tree-compiler
```

## 3. 编译工程

```bash
cd ~/character_device_driver-codex
make
```

## 4. 安装 overlay

```bash
sudo cp overlay/rpi5-ili9341-spi0.dtbo /boot/firmware/overlays/
sudoedit /boot/firmware/config.txt
```

附加以下配置：

```ini
dtparam=spi=on
dtoverlay=rpi5-ili9341-spi0
```

## 5. 重启并加载模块

```bash
sudo reboot
sudo insmod ~/character_device_driver-codex/rpi5_ili9341.ko
```

## 6. 查看 probe 日志

```bash
dmesg | grep -i rpi5_ili9341
```

期望看到：

- SPI 设备成功绑定
- `/dev/rpi5_ili9341` 注册成功
- `/dev/fbX` 注册成功

## 7. 基础显示验证

```bash
sudo ./tools/ili9341_demo info
sudo ./tools/ili9341_demo clear f800
sudo ./tools/ili9341_demo bars
sudo ./tools/ili9341_demo gradient
```

当前默认方向是竖屏 `240x320`。如果想切到其他方向：

```bash
sudo ./tools/ili9341_demo rotate 90
sudo ./tools/ili9341_demo rotate 180
sudo ./tools/ili9341_demo rotate 270
```

## 8. 常见问题

### 白屏

- 确认屏幕是“真正的 SPI ILI9341 模块”
- 检查 `CS/DC/RST` 接线
- 尝试把 `spi-max-frequency` 降到 `8000000`

### 黑屏但背光不亮

- 先检查 `LED/BL` 是否接到了 `3V3`
- 当前默认方案不通过 GPIO 控背光

### 方向不对

- 修改 `overlay/rpi5-ili9341-spi0.dts` 的 `rotation`
- 或用 `./tools/ili9341_demo rotate <角度>` 运行时切换
