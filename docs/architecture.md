# 驱动架构说明

## 1. 总体链路

```text
用户空间 demo / 图形程序
        │
        ├── /dev/rpi5_ili9341  (write / mmap / ioctl)
        │
        └── /dev/fbX           (fbdev write / mmap)
                         │
                    内核显存 vmalloc
                         │
                  dirty 区域合并 / deferred io
                         │
                    SPI 刷新工作队列
                         │
                     ILI9341 LCD
```

## 2. 核心模块划分

### `spi_driver`

- 负责从设备树解析 SPI 设备、GPIO、旋转参数和 SPI 速率。
- 在 `probe()` 中初始化 LCD、注册 framebuffer 和 misc 字符设备。

### `fbdev`

- 向系统提供 `/dev/fbX` 标准接口。
- 支持 `fb_read`、`fb_write`、`fb_fillrect`、`fb_copyarea`、`fb_imageblit`。
- 通过 `fb_deferred_io` 捕获 `mmap` 写显存场景。

### `misc char device`

- 暴露 `/dev/rpi5_ili9341`。
- 适合实验课程和定制应用，支持局部刷新、背光控制、旋转切换和 TE 等待。

### `workqueue + dirty merge`

- 所有刷新都走同一个 `flush_work`。
- 多次局部写入会在内核里合并成更大的 dirty rectangle，减少 SPI 小包数量。

### `TE IRQ`

- 如果外接 `TE` 信号，驱动可在刷帧前等待一次 tearing effect 脉冲。
- 该能力是可选的，不接线时驱动仍可正常工作。

## 3. 为什么同时保留 framebuffer 和字符设备

- 只有 framebuffer：更标准，但实验灵活性不足，控制粒度也不够细。
- 只有字符设备：能做一切，但不容易接入 Linux 现有显示生态。
- 两者同时保留：既能展示 Linux 图形子系统接口，也能保留嵌入式课程里最重要的设备控制链路。

## 4. 内存与像素格式

- 像素格式：`RGB565`
- 分辨率：默认 `320x240`
- 显存大小：`153600` 字节
- 内核显存字节序：CPU 原生小端
- SPI 发送字节序：驱动转换为 ILI9341 需要的大端高字节优先

## 5. 局限与后续优化

- 当前版本使用 `fbdev`，若后续要进一步靠近主线显示栈，可迁移到 `DRM tiny/MIPI-DBI`。
- 目前 dirty 合并策略偏保守，后续可优化为多矩形或按页脏区映射。
- 背光目前走普通 GPIO，高级版本可以改为 PWM 背光。
