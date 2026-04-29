/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_RPI5_ILI9341_H
#define _UAPI_LINUX_RPI5_ILI9341_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define RPI5_ILI9341_NAME "rpi5_ili9341"

struct rpi5_ili9341_panel_info {
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 bpp;
	__u32 rotation;
	__u32 has_te;
};

struct rpi5_ili9341_rect {
	__u16 x;
	__u16 y;
	__u16 width;
	__u16 height;
};

#define RPI5_ILI9341_IOC_MAGIC		'I'
#define RPI5_ILI9341_IOC_GET_INFO	_IOR(RPI5_ILI9341_IOC_MAGIC, 0x00, struct rpi5_ili9341_panel_info)
#define RPI5_ILI9341_IOC_FLUSH		_IO(RPI5_ILI9341_IOC_MAGIC, 0x01)
#define RPI5_ILI9341_IOC_FLUSH_RECT	_IOW(RPI5_ILI9341_IOC_MAGIC, 0x02, struct rpi5_ili9341_rect)
#define RPI5_ILI9341_IOC_CLEAR		_IOW(RPI5_ILI9341_IOC_MAGIC, 0x03, __u16)
#define RPI5_ILI9341_IOC_SET_ROTATION	_IOW(RPI5_ILI9341_IOC_MAGIC, 0x04, __u32)
#define RPI5_ILI9341_IOC_SET_BACKLIGHT	_IOW(RPI5_ILI9341_IOC_MAGIC, 0x05, __u32)
#define RPI5_ILI9341_IOC_WAIT_TE	_IOW(RPI5_ILI9341_IOC_MAGIC, 0x06, __u32)

#endif /* _UAPI_LINUX_RPI5_ILI9341_H */
