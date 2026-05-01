# 上板与验证步骤

## 1. 先确认屏幕类型

这个项目只适用于 SPI 显示接口的 ILI9341 模块。  
如果你的模块页面写的是 `8-bit/16-bit parallel`、`8080 interface`、`MCU interface`，那它不适合本仓库。

已确认不适用的示例：

- `LCDWiki MRB2801`

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
sudo ./tools/ili9341_demo solid-blue
sudo ./tools/ili9341_demo bars
sudo ./tools/ili9341_demo gradient
```

## 8. 常见问题

### 一直白屏

- 优先确认这块屏是不是“真正的 SPI 显示模块”
- 核对 `CS/DC/RST` 接线
- 核对 BCM 编号和实体针脚号没有混淆

### 黑屏但背光不亮

- 先检查 `LED` 接线
- 临时把 `LED` 直接接 `3V3` 验证背光硬件
- 再回头调整 `led-gpios` 极性

### 花屏

- 把 `spi-max-frequency` 从 `32000000` 降到 `16000000`

### 方向不对

- 修改 `overlay/rpi5-ili9341-spi0.dts` 里的 `rotation`
- 或运行 `./tools/ili9341_demo rotate 0|90|180|270`
