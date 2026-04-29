# 上板与验证步骤

## 1. 安装依赖

```bash
sudo apt update
sudo apt install -y raspberrypi-kernel-headers build-essential device-tree-compiler
```

## 2. 编译工程

```bash
cd ~/character_device_driver-codex
make
```

## 3. 安装 overlay

```bash
sudo cp overlay/rpi5-ili9341-spi0.dtbo /boot/firmware/overlays/
sudoedit /boot/firmware/config.txt
```

附加以下配置：

```ini
dtparam=spi=on
dtoverlay=rpi5-ili9341-spi0
```

## 4. 重启并加载模块

```bash
sudo reboot
sudo insmod ~/character_device_driver-codex/rpi5_ili9341.ko
```

## 5. 查看 probe 日志

```bash
dmesg | grep -i ili9341
```

期望看到：

- SPI 设备成功绑定
- `/dev/rpi5_ili9341` 注册成功
- `/dev/fbX` 注册成功

## 6. 基础显示验证

```bash
sudo ./tools/ili9341_demo info
sudo ./tools/ili9341_demo bars
sudo ./tools/ili9341_demo gradient
```

## 7. TE 中断验证

如果屏模块接了 `TE`，并且 overlay 中保留了 `te-gpios`：

```bash
sudo ./tools/ili9341_demo info
```

确认输出中 `te=yes`。随后可以在自己的测试程序中调用 `RPI5_ILI9341_IOC_WAIT_TE` 做同步显示。

## 8. 常见问题

### 黑屏

- 检查 `SPI0` 是否真正开启。
- 检查 `DC/RST/LED` 接线。
- 降低 `spi-max-frequency`。

### 方向不对

- 修改 `overlay/rpi5-ili9341-spi0.dts` 的 `rotation = <90>;`
- 或在运行时执行 `./tools/ili9341_demo rotate 0`

### 显示内容错位

- 多数情况下是 `MADCTL` 方向和面板安装方向不一致。
- 如果你的面板是反装的，可把默认旋转从 `90` 改成 `270`。
