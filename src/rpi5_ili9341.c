// SPDX-License-Identifier: GPL-2.0
/*
 * Raspberry Pi 5 ILI9341 SPI LCD character device + fbdev driver
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <uapi/linux/rpi5_ili9341.h>

#define ILI9341_NATIVE_WIDTH		240U
#define ILI9341_NATIVE_HEIGHT		320U
#define ILI9341_LANDSCAPE_WIDTH		320U
#define ILI9341_LANDSCAPE_HEIGHT	240U
#define ILI9341_BPP			16U
#define ILI9341_BYTES_PER_PIXEL		2U
#define ILI9341_VMEM_SIZE		(ILI9341_NATIVE_WIDTH * ILI9341_NATIVE_HEIGHT * ILI9341_BYTES_PER_PIXEL)
#define ILI9341_TXBUF_SIZE		(ILI9341_LANDSCAPE_WIDTH * 16U * ILI9341_BYTES_PER_PIXEL)
#define ILI9341_DEFIO_DELAY_MS		16U
#define ILI9341_DEFAULT_SPEED_HZ	32000000U
#define ILI9341_WAIT_TE_TIMEOUT_MS	20U

#define ILI9341_SWRESET			0x01
#define ILI9341_SLPIN			0x10
#define ILI9341_SLPOUT			0x11
#define ILI9341_DISPOFF			0x28
#define ILI9341_DISPON			0x29
#define ILI9341_CASET			0x2A
#define ILI9341_PASET			0x2B
#define ILI9341_RAMWR			0x2C
#define ILI9341_MADCTL			0x36
#define ILI9341_PIXFMT			0x3A
#define ILI9341_FRMCTR1			0xB1
#define ILI9341_DFUNCTR			0xB6
#define ILI9341_PWCTR1			0xC0
#define ILI9341_PWCTR2			0xC1
#define ILI9341_VMCTR1			0xC5
#define ILI9341_VMCTR2			0xC7
#define ILI9341_GMCTRP1			0xE0
#define ILI9341_GMCTRN1			0xE1

#define ILI9341_MADCTL_MY		BIT(7)
#define ILI9341_MADCTL_MX		BIT(6)
#define ILI9341_MADCTL_MV		BIT(5)
#define ILI9341_MADCTL_BGR		BIT(3)

struct rpi5_ili9341 {
	struct spi_device *spi;
	struct fb_info *info;
	struct miscdevice miscdev;
	struct gpio_desc *dc_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *led_gpio;
	struct gpio_desc *te_gpio;
	struct completion te_completion;
	struct mutex lock;
	spinlock_t dirty_lock;
	struct work_struct flush_work;
	struct fb_deferred_io defio;
	void *vmem;
	u8 *txbuf;
	u32 pseudo_palette[16];
	u32 rotation;
	u32 max_speed_hz;
	unsigned int dirty_x1;
	unsigned int dirty_y1;
	unsigned int dirty_x2;
	unsigned int dirty_y2;
	bool dirty_valid;
	bool bgr;
	bool sync_to_te;
	int te_irq;
	char misc_name[32];
};

static int rpi5_ili9341_write_buf(struct rpi5_ili9341 *lcd, const void *buf,
				  size_t len, bool is_data)
{
	if (lcd->dc_gpio)
		gpiod_set_value_cansleep(lcd->dc_gpio, is_data);

	return spi_write(lcd->spi, buf, len);
}

static int rpi5_ili9341_write_cmd(struct rpi5_ili9341 *lcd, u8 cmd)
{
	return rpi5_ili9341_write_buf(lcd, &cmd, sizeof(cmd), false);
}

static int rpi5_ili9341_write_data(struct rpi5_ili9341 *lcd, const void *buf,
				   size_t len)
{
	return rpi5_ili9341_write_buf(lcd, buf, len, true);
}

static int rpi5_ili9341_write_reg(struct rpi5_ili9341 *lcd, u8 reg,
				  const void *data, size_t len)
{
	int ret;

	ret = rpi5_ili9341_write_cmd(lcd, reg);
	if (ret || !len)
		return ret;

	return rpi5_ili9341_write_data(lcd, data, len);
}

static int rpi5_ili9341_wait_te(struct rpi5_ili9341 *lcd, u32 timeout_ms)
{
	unsigned long timeout;

	if (lcd->te_irq <= 0)
		return -EOPNOTSUPP;

	reinit_completion(&lcd->te_completion);
	timeout = wait_for_completion_timeout(&lcd->te_completion,
					      msecs_to_jiffies(timeout_ms));

	return timeout ? 0 : -ETIMEDOUT;
}

static irqreturn_t rpi5_ili9341_te_irq(int irq, void *dev_id)
{
	struct rpi5_ili9341 *lcd = dev_id;

	(void)irq;
	complete(&lcd->te_completion);

	return IRQ_HANDLED;
}

static u8 rpi5_ili9341_madctl_value(struct rpi5_ili9341 *lcd, u32 rotation)
{
	u8 madctl = lcd->bgr ? ILI9341_MADCTL_BGR : 0;

	switch (rotation) {
	case 0:
		madctl |= ILI9341_MADCTL_MX;
		break;
	case 90:
		madctl |= ILI9341_MADCTL_MV;
		break;
	case 180:
		madctl |= ILI9341_MADCTL_MY;
		break;
	case 270:
		madctl |= ILI9341_MADCTL_MX | ILI9341_MADCTL_MY |
			  ILI9341_MADCTL_MV;
		break;
	default:
		madctl |= ILI9341_MADCTL_MV;
		break;
	}

	return madctl;
}

static void rpi5_ili9341_update_geometry(struct rpi5_ili9341 *lcd, u32 rotation)
{
	struct fb_info *info = lcd->info;
	u32 xres;
	u32 yres;

	if (rotation == 90 || rotation == 270) {
		xres = ILI9341_LANDSCAPE_WIDTH;
		yres = ILI9341_LANDSCAPE_HEIGHT;
	} else {
		xres = ILI9341_NATIVE_WIDTH;
		yres = ILI9341_NATIVE_HEIGHT;
	}

	info->var.xres = xres;
	info->var.yres = yres;
	info->var.xres_virtual = xres;
	info->var.yres_virtual = yres;
	info->var.rotate = rotation / 90;
	info->fix.line_length = xres * ILI9341_BYTES_PER_PIXEL;
	info->fix.smem_len = ILI9341_VMEM_SIZE;
	info->screen_size = ILI9341_VMEM_SIZE;
	lcd->rotation = rotation;
}

static int rpi5_ili9341_set_rotation_locked(struct rpi5_ili9341 *lcd,
					    u32 rotation)
{
	u8 madctl;
	int ret;

	if (rotation != 0 && rotation != 90 && rotation != 180 &&
	    rotation != 270)
		return -EINVAL;

	rpi5_ili9341_update_geometry(lcd, rotation);
	madctl = rpi5_ili9341_madctl_value(lcd, rotation);

	ret = rpi5_ili9341_write_reg(lcd, ILI9341_MADCTL, &madctl,
				     sizeof(madctl));
	if (ret)
		return ret;

	dev_dbg(&lcd->spi->dev, "rotation set to %u degrees\n", rotation);

	return 0;
}

static int rpi5_ili9341_set_window(struct rpi5_ili9341 *lcd, u16 x0, u16 y0,
				   u16 x1, u16 y1)
{
	u8 buf[4];
	int ret;

	buf[0] = x0 >> 8;
	buf[1] = x0 & 0xff;
	buf[2] = x1 >> 8;
	buf[3] = x1 & 0xff;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_CASET, buf, sizeof(buf));
	if (ret)
		return ret;

	buf[0] = y0 >> 8;
	buf[1] = y0 & 0xff;
	buf[2] = y1 >> 8;
	buf[3] = y1 & 0xff;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_PASET, buf, sizeof(buf));
	if (ret)
		return ret;

	return rpi5_ili9341_write_cmd(lcd, ILI9341_RAMWR);
}

static int rpi5_ili9341_flush_rect_locked(struct rpi5_ili9341 *lcd,
					  unsigned int x, unsigned int y,
					  unsigned int width,
					  unsigned int height)
{
	struct fb_info *info = lcd->info;
	unsigned int bytes_per_line;
	unsigned int lines_per_chunk;
	unsigned int stride = info->fix.line_length;
	unsigned int line;
	int ret;

	if (!width || !height)
		return 0;

	if (x + width > info->var.xres || y + height > info->var.yres)
		return -EINVAL;

	if (lcd->sync_to_te && lcd->te_irq > 0)
		rpi5_ili9341_wait_te(lcd, ILI9341_WAIT_TE_TIMEOUT_MS);

	ret = rpi5_ili9341_set_window(lcd, x, y, x + width - 1, y + height - 1);
	if (ret)
		return ret;

	bytes_per_line = width * ILI9341_BYTES_PER_PIXEL;
	lines_per_chunk = max(1U, ILI9341_TXBUF_SIZE / bytes_per_line);

	for (line = 0; line < height; ) {
		unsigned int chunk_lines = min(lines_per_chunk, height - line);
		unsigned int row;
		u8 *dst = lcd->txbuf;

		for (row = 0; row < chunk_lines; ++row) {
			const u16 *src = (u16 *)((u8 *)lcd->vmem +
				((y + line + row) * stride) +
				(x * ILI9341_BYTES_PER_PIXEL));
			unsigned int col;

			for (col = 0; col < width; ++col) {
				u16 pixel = src[col];

				*dst++ = pixel >> 8;
				*dst++ = pixel & 0xff;
			}
		}

		ret = rpi5_ili9341_write_data(lcd, lcd->txbuf,
					      chunk_lines * bytes_per_line);
		if (ret)
			return ret;

		line += chunk_lines;
	}

	return 0;
}

static void rpi5_ili9341_mark_dirty(struct rpi5_ili9341 *lcd, unsigned int x,
				    unsigned int y, unsigned int width,
				    unsigned int height)
{
	struct fb_info *info = lcd->info;
	unsigned long flags;
	unsigned int x2;
	unsigned int y2;

	if (!width || !height)
		return;

	if (x >= info->var.xres || y >= info->var.yres)
		return;

	x2 = min(x + width, info->var.xres);
	y2 = min(y + height, info->var.yres);

	spin_lock_irqsave(&lcd->dirty_lock, flags);
	if (!lcd->dirty_valid) {
		lcd->dirty_x1 = x;
		lcd->dirty_y1 = y;
		lcd->dirty_x2 = x2;
		lcd->dirty_y2 = y2;
		lcd->dirty_valid = true;
	} else {
		lcd->dirty_x1 = min(lcd->dirty_x1, x);
		lcd->dirty_y1 = min(lcd->dirty_y1, y);
		lcd->dirty_x2 = max(lcd->dirty_x2, x2);
		lcd->dirty_y2 = max(lcd->dirty_y2, y2);
	}
	spin_unlock_irqrestore(&lcd->dirty_lock, flags);
}

static void rpi5_ili9341_mark_dirty_range(struct rpi5_ili9341 *lcd,
					  loff_t offset, size_t len)
{
	struct fb_info *info = lcd->info;
	unsigned int stride = info->fix.line_length;
	unsigned int start_line;
	unsigned int end_line;

	if (!len)
		return;

	start_line = offset / stride;
	end_line = (offset + len - 1) / stride;

	if (start_line >= info->var.yres)
		return;

	end_line = min(end_line, info->var.yres - 1);
	rpi5_ili9341_mark_dirty(lcd, 0, start_line, info->var.xres,
				end_line - start_line + 1);
}

static void rpi5_ili9341_flush_workfn(struct work_struct *work)
{
	struct rpi5_ili9341 *lcd = container_of(work, struct rpi5_ili9341,
						flush_work);

	for (;;) {
		unsigned long flags;
		unsigned int x1;
		unsigned int y1;
		unsigned int x2;
		unsigned int y2;

		spin_lock_irqsave(&lcd->dirty_lock, flags);
		if (!lcd->dirty_valid) {
			spin_unlock_irqrestore(&lcd->dirty_lock, flags);
			break;
		}

		x1 = lcd->dirty_x1;
		y1 = lcd->dirty_y1;
		x2 = lcd->dirty_x2;
		y2 = lcd->dirty_y2;
		lcd->dirty_valid = false;
		spin_unlock_irqrestore(&lcd->dirty_lock, flags);

		mutex_lock(&lcd->lock);
		rpi5_ili9341_flush_rect_locked(lcd, x1, y1, x2 - x1, y2 - y1);
		mutex_unlock(&lcd->lock);
	}
}

static void rpi5_ili9341_schedule_flush(struct rpi5_ili9341 *lcd)
{
	schedule_work(&lcd->flush_work);
}

static void rpi5_ili9341_deferred_io(struct fb_info *info,
				     struct list_head *pagelist)
{
	struct rpi5_ili9341 *lcd = info->par;

	(void)pagelist;
	rpi5_ili9341_mark_dirty(lcd, 0, 0, info->var.xres, info->var.yres);
	rpi5_ili9341_schedule_flush(lcd);
}

static int rpi5_ili9341_hw_reset(struct rpi5_ili9341 *lcd)
{
	if (!lcd->reset_gpio)
		return 0;

	gpiod_set_value_cansleep(lcd->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(lcd->reset_gpio, 0);
	msleep(120);

	return 0;
}

static int rpi5_ili9341_init_display(struct rpi5_ili9341 *lcd)
{
	static const u8 power_b[] = { 0x00, 0x83, 0x30 };
	static const u8 power_seq[] = { 0x64, 0x03, 0x12, 0x81 };
	static const u8 driver_timing_a[] = { 0x85, 0x01, 0x79 };
	static const u8 power_a[] = { 0x39, 0x2c, 0x00, 0x34, 0x02 };
	static const u8 pump_ratio[] = { 0x20 };
	static const u8 driver_timing_b[] = { 0x00, 0x00 };
	static const u8 pwctr1[] = { 0x26 };
	static const u8 pwctr2[] = { 0x11 };
	static const u8 vmctr1[] = { 0x35, 0x3e };
	static const u8 vmctr2[] = { 0xbe };
	static const u8 pixfmt[] = { 0x55 };
	static const u8 frmctr1[] = { 0x00, 0x1b };
	static const u8 dfunctr[] = { 0x08, 0x82, 0x27 };
	static const u8 enable_3g[] = { 0x00 };
	static const u8 gamma[] = { 0x01 };
	static const u8 pos_gamma[] = {
		0x1f, 0x1a, 0x18, 0x0a, 0x0f, 0x06, 0x45, 0x87,
		0x32, 0x0a, 0x07, 0x02, 0x07, 0x05, 0x00
	};
	static const u8 neg_gamma[] = {
		0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3a, 0x78,
		0x4d, 0x05, 0x18, 0x0d, 0x38, 0x3a, 0x1f
	};
	u8 madctl;
	int ret;

	ret = rpi5_ili9341_hw_reset(lcd);
	if (ret)
		return ret;

	ret = rpi5_ili9341_write_cmd(lcd, ILI9341_SWRESET);
	if (ret)
		return ret;
	msleep(120);

	ret = rpi5_ili9341_write_cmd(lcd, ILI9341_DISPOFF);
	if (ret)
		return ret;

	ret = rpi5_ili9341_write_reg(lcd, 0xCF, power_b, sizeof(power_b));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, 0xED, power_seq, sizeof(power_seq));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, 0xE8, driver_timing_a,
				     sizeof(driver_timing_a));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, 0xCB, power_a, sizeof(power_a));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, 0xF7, pump_ratio,
				     sizeof(pump_ratio));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, 0xEA, driver_timing_b,
				     sizeof(driver_timing_b));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_PWCTR1, pwctr1,
				     sizeof(pwctr1));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_PWCTR2, pwctr2,
				     sizeof(pwctr2));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_VMCTR1, vmctr1,
				     sizeof(vmctr1));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_VMCTR2, vmctr2,
				     sizeof(vmctr2));
	if (ret)
		return ret;

	madctl = rpi5_ili9341_madctl_value(lcd, lcd->rotation);
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_MADCTL, &madctl,
				     sizeof(madctl));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_PIXFMT, pixfmt,
				     sizeof(pixfmt));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_FRMCTR1, frmctr1,
				     sizeof(frmctr1));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_DFUNCTR, dfunctr,
				     sizeof(dfunctr));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, 0xF2, enable_3g,
				     sizeof(enable_3g));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, 0x26, gamma, sizeof(gamma));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_GMCTRP1, pos_gamma,
				     sizeof(pos_gamma));
	if (ret)
		return ret;
	ret = rpi5_ili9341_write_reg(lcd, ILI9341_GMCTRN1, neg_gamma,
				     sizeof(neg_gamma));
	if (ret)
		return ret;

	ret = rpi5_ili9341_write_cmd(lcd, ILI9341_SLPOUT);
	if (ret)
		return ret;
	msleep(120);

	ret = rpi5_ili9341_write_cmd(lcd, ILI9341_DISPON);
	if (ret)
		return ret;
	msleep(20);

	if (lcd->led_gpio)
		gpiod_set_value_cansleep(lcd->led_gpio, 1);

	return 0;
}

static ssize_t rpi5_ili9341_fb_read(struct fb_info *info, char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct rpi5_ili9341 *lcd = info->par;

	return simple_read_from_buffer(buf, count, ppos, lcd->vmem,
				       info->screen_size);
}

static ssize_t rpi5_ili9341_fb_write(struct fb_info *info,
				     const char __user *buf, size_t count,
				     loff_t *ppos)
{
	struct rpi5_ili9341 *lcd = info->par;
	loff_t pos = *ppos;
	size_t available;

	if (pos < 0)
		return -EINVAL;
	if (pos >= info->screen_size)
		return -ENOSPC;

	available = info->screen_size - pos;
	if (count > available)
		count = available;

	if (copy_from_user((u8 *)lcd->vmem + pos, buf, count))
		return -EFAULT;

	*ppos += count;
	rpi5_ili9341_mark_dirty_range(lcd, pos, count);
	rpi5_ili9341_schedule_flush(lcd);

	return count;
}

static int rpi5_ili9341_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	return fb_deferred_io_mmap(info, vma);
}

static void rpi5_ili9341_fb_fillrect(struct fb_info *info,
				     const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
	rpi5_ili9341_mark_dirty(info->par, rect->dx, rect->dy, rect->width,
				rect->height);
	rpi5_ili9341_schedule_flush(info->par);
}

static void rpi5_ili9341_fb_copyarea(struct fb_info *info,
				     const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	rpi5_ili9341_mark_dirty(info->par, area->dx, area->dy, area->width,
				area->height);
	rpi5_ili9341_schedule_flush(info->par);
}

static void rpi5_ili9341_fb_imageblit(struct fb_info *info,
				      const struct fb_image *image)
{
	sys_imageblit(info, image);
	rpi5_ili9341_mark_dirty(info->par, image->dx, image->dy, image->width,
				image->height);
	rpi5_ili9341_schedule_flush(info->par);
}

static int rpi5_ili9341_setcolreg(unsigned int regno, unsigned int red,
				  unsigned int green, unsigned int blue,
				  unsigned int transp, struct fb_info *info)
{
	struct rpi5_ili9341 *lcd = info->par;
	u32 value;

	(void)transp;
	if (regno >= ARRAY_SIZE(lcd->pseudo_palette))
		return -EINVAL;

	value = ((red >> 11) << info->var.red.offset) |
		((green >> 10) << info->var.green.offset) |
		((blue >> 11) << info->var.blue.offset);
	lcd->pseudo_palette[regno] = value;

	return 0;
}

static int rpi5_ili9341_check_var(struct fb_var_screeninfo *var,
				  struct fb_info *info)
{
	(void)info;
	if (var->bits_per_pixel != ILI9341_BPP)
		return -EINVAL;

	if (var->rotate > FB_ROTATE_CCW)
		return -EINVAL;

	return 0;
}

static int rpi5_ili9341_set_par(struct fb_info *info)
{
	struct rpi5_ili9341 *lcd = info->par;
	u32 rotation = info->var.rotate * 90;
	int ret;

	mutex_lock(&lcd->lock);
	ret = rpi5_ili9341_set_rotation_locked(lcd, rotation);
	mutex_unlock(&lcd->lock);
	if (ret)
		return ret;

	rpi5_ili9341_mark_dirty(lcd, 0, 0, info->var.xres, info->var.yres);
	rpi5_ili9341_schedule_flush(lcd);

	return 0;
}

static const struct fb_ops rpi5_ili9341_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= rpi5_ili9341_fb_read,
	.fb_write	= rpi5_ili9341_fb_write,
	.fb_fillrect	= rpi5_ili9341_fb_fillrect,
	.fb_copyarea	= rpi5_ili9341_fb_copyarea,
	.fb_imageblit	= rpi5_ili9341_fb_imageblit,
	.fb_setcolreg	= rpi5_ili9341_setcolreg,
	.fb_check_var	= rpi5_ili9341_check_var,
	.fb_set_par	= rpi5_ili9341_set_par,
	.fb_mmap	= rpi5_ili9341_fb_mmap,
};

static int rpi5_ili9341_chr_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct rpi5_ili9341 *lcd = container_of(misc, struct rpi5_ili9341,
						miscdev);

	(void)inode;
	file->private_data = lcd;
	return 0;
}

static loff_t rpi5_ili9341_chr_llseek(struct file *file, loff_t off, int whence)
{
	struct rpi5_ili9341 *lcd = file->private_data;

	return fixed_size_llseek(file, off, whence, lcd->info->screen_size);
}

static ssize_t rpi5_ili9341_chr_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct rpi5_ili9341 *lcd = file->private_data;

	return simple_read_from_buffer(buf, count, ppos, lcd->vmem,
				       lcd->info->screen_size);
}

static ssize_t rpi5_ili9341_chr_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct rpi5_ili9341 *lcd = file->private_data;
	loff_t pos = *ppos;
	size_t available;

	if (pos < 0)
		return -EINVAL;
	if (pos >= lcd->info->screen_size)
		return -ENOSPC;

	available = lcd->info->screen_size - pos;
	if (count > available)
		count = available;

	if (copy_from_user((u8 *)lcd->vmem + pos, buf, count))
		return -EFAULT;

	*ppos += count;
	rpi5_ili9341_mark_dirty_range(lcd, pos, count);
	rpi5_ili9341_schedule_flush(lcd);

	return count;
}

static long rpi5_ili9341_chr_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct rpi5_ili9341 *lcd = file->private_data;
	struct rpi5_ili9341_panel_info info;
	struct rpi5_ili9341_rect rect;
	u32 value;
	u16 color;
	unsigned int x;
	unsigned int y;
	unsigned int stride;
	int ret;

	switch (cmd) {
	case RPI5_ILI9341_IOC_GET_INFO:
		info.width = lcd->info->var.xres;
		info.height = lcd->info->var.yres;
		info.stride = lcd->info->fix.line_length;
		info.bpp = lcd->info->var.bits_per_pixel;
		info.rotation = lcd->rotation;
		info.has_te = lcd->te_irq > 0;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	case RPI5_ILI9341_IOC_FLUSH:
		rpi5_ili9341_mark_dirty(lcd, 0, 0, lcd->info->var.xres,
					lcd->info->var.yres);
		rpi5_ili9341_schedule_flush(lcd);
		flush_work(&lcd->flush_work);
		return 0;
	case RPI5_ILI9341_IOC_FLUSH_RECT:
		if (copy_from_user(&rect, (void __user *)arg, sizeof(rect)))
			return -EFAULT;
		rpi5_ili9341_mark_dirty(lcd, rect.x, rect.y, rect.width,
					rect.height);
		rpi5_ili9341_schedule_flush(lcd);
		flush_work(&lcd->flush_work);
		return 0;
	case RPI5_ILI9341_IOC_CLEAR:
		if (copy_from_user(&color, (void __user *)arg, sizeof(color)))
			return -EFAULT;
		stride = lcd->info->fix.line_length / sizeof(u16);
		for (y = 0; y < lcd->info->var.yres; ++y) {
			u16 *line = (u16 *)((u8 *)lcd->vmem +
				(y * lcd->info->fix.line_length));

			for (x = 0; x < lcd->info->var.xres; ++x)
				line[x] = color;
			for (; x < stride; ++x)
				line[x] = 0;
		}
		rpi5_ili9341_mark_dirty(lcd, 0, 0, lcd->info->var.xres,
					lcd->info->var.yres);
		rpi5_ili9341_schedule_flush(lcd);
		flush_work(&lcd->flush_work);
		return 0;
	case RPI5_ILI9341_IOC_SET_ROTATION:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		mutex_lock(&lcd->lock);
		ret = rpi5_ili9341_set_rotation_locked(lcd, value);
		mutex_unlock(&lcd->lock);
		if (ret)
			return ret;
		rpi5_ili9341_mark_dirty(lcd, 0, 0, lcd->info->var.xres,
					lcd->info->var.yres);
		rpi5_ili9341_schedule_flush(lcd);
		flush_work(&lcd->flush_work);
		return 0;
	case RPI5_ILI9341_IOC_SET_BACKLIGHT:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		if (!lcd->led_gpio)
			return -EOPNOTSUPP;
		gpiod_set_value_cansleep(lcd->led_gpio, !!value);
		return 0;
	case RPI5_ILI9341_IOC_WAIT_TE:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		if (!value)
			value = ILI9341_WAIT_TE_TIMEOUT_MS;
		return rpi5_ili9341_wait_te(lcd, value);
	default:
		return -ENOTTY;
	}
}

static int rpi5_ili9341_chr_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct rpi5_ili9341 *lcd = file->private_data;
	unsigned long requested = vma->vm_end - vma->vm_start;

	/*
	 * Userspace mmap length is rounded up to a page-sized VMA. Accept
	 * that rounded size as long as it stays within the page-aligned
	 * framebuffer allocation window.
	 */
	if (requested > PAGE_ALIGN(lcd->info->screen_size))
		return -EINVAL;

	return remap_vmalloc_range(vma, lcd->vmem, 0);
}

static const struct file_operations rpi5_ili9341_chr_fops = {
	.owner		= THIS_MODULE,
	.open		= rpi5_ili9341_chr_open,
	.read		= rpi5_ili9341_chr_read,
	.write		= rpi5_ili9341_chr_write,
	.llseek		= rpi5_ili9341_chr_llseek,
	.unlocked_ioctl	= rpi5_ili9341_chr_ioctl,
	.mmap		= rpi5_ili9341_chr_mmap,
};

static void rpi5_ili9341_setup_fix(struct fb_fix_screeninfo *fix)
{
	strscpy(fix->id, RPI5_ILI9341_NAME, sizeof(fix->id));
	fix->type = FB_TYPE_PACKED_PIXELS;
	fix->visual = FB_VISUAL_TRUECOLOR;
	fix->xpanstep = 0;
	fix->ypanstep = 0;
	fix->ywrapstep = 0;
	fix->accel = FB_ACCEL_NONE;
}

static void rpi5_ili9341_setup_var(struct fb_var_screeninfo *var)
{
	var->bits_per_pixel = ILI9341_BPP;
	var->red.offset = 11;
	var->red.length = 5;
	var->green.offset = 5;
	var->green.length = 6;
	var->blue.offset = 0;
	var->blue.length = 5;
	var->transp.offset = 0;
	var->transp.length = 0;
	var->activate = FB_ACTIVATE_NOW;
	var->width = 49;
	var->height = 37;
}

static int rpi5_ili9341_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct fb_info *info;
	struct rpi5_ili9341 *lcd;
	u32 rotation = 90;
	int ret;

	info = framebuffer_alloc(sizeof(*lcd), dev);
	if (!info)
		return -ENOMEM;

	lcd = info->par;
	memset(lcd, 0, sizeof(*lcd));

	lcd->spi = spi;
	lcd->info = info;
	lcd->max_speed_hz = spi->max_speed_hz ?: ILI9341_DEFAULT_SPEED_HZ;
	mutex_init(&lcd->lock);
	spin_lock_init(&lcd->dirty_lock);
	init_completion(&lcd->te_completion);
	INIT_WORK(&lcd->flush_work, rpi5_ili9341_flush_workfn);

	spi_set_drvdata(spi, lcd);
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	spi->max_speed_hz = lcd->max_speed_hz;
	ret = spi_setup(spi);
	if (ret)
		goto err_release_fb;

	lcd->dc_gpio = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->dc_gpio)) {
		ret = PTR_ERR(lcd->dc_gpio);
		goto err_release_fb;
	}

	lcd->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->reset_gpio)) {
		ret = PTR_ERR(lcd->reset_gpio);
		goto err_release_fb;
	}

	lcd->led_gpio = devm_gpiod_get_optional(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->led_gpio)) {
		ret = PTR_ERR(lcd->led_gpio);
		goto err_release_fb;
	}

	lcd->te_gpio = devm_gpiod_get_optional(dev, "te", GPIOD_IN);
	if (IS_ERR(lcd->te_gpio)) {
		ret = PTR_ERR(lcd->te_gpio);
		goto err_release_fb;
	}

	if (!lcd->dc_gpio) {
		dev_err(dev, "dc-gpios is required for ILI9341 SPI panels\n");
		ret = -EINVAL;
		goto err_release_fb;
	}

	device_property_read_u32(dev, "rotation", &rotation);
	lcd->bgr = device_property_read_bool(dev, "bgr");
	lcd->sync_to_te = device_property_read_bool(dev, "sync-to-te");

	lcd->vmem = vmalloc_user(ILI9341_VMEM_SIZE);
	if (!lcd->vmem) {
		ret = -ENOMEM;
		goto err_release_fb;
	}


	lcd->txbuf = vzalloc(ILI9341_TXBUF_SIZE);
	if (!lcd->txbuf) {
		ret = -ENOMEM;
		goto err_free_vmem;
	}

	info->screen_base = (__force char __iomem *)lcd->vmem;
	info->screen_size = ILI9341_VMEM_SIZE;
	info->fbops = &rpi5_ili9341_fb_ops;
	info->pseudo_palette = lcd->pseudo_palette;
	/*
	 * Raspberry Pi 6.12 headers do not expose FBINFO_FLAG_DEFAULT,
	 * and this virtual framebuffer only needs the VIRT flag here.
	 */
	info->flags = FBINFO_VIRTFB;
	rpi5_ili9341_setup_fix(&info->fix);
	rpi5_ili9341_setup_var(&info->var);
	rpi5_ili9341_update_geometry(lcd, rotation);

	lcd->defio.delay = msecs_to_jiffies(ILI9341_DEFIO_DELAY_MS);
	lcd->defio.deferred_io = rpi5_ili9341_deferred_io;
	info->fbdefio = &lcd->defio;
	fb_deferred_io_init(info);

	if (lcd->te_gpio) {
		lcd->te_irq = gpiod_to_irq(lcd->te_gpio);
		if (lcd->te_irq > 0) {
			ret = devm_request_irq(dev, lcd->te_irq,
					       rpi5_ili9341_te_irq,
					       IRQF_TRIGGER_RISING,
					       dev_name(dev), lcd);
			if (ret)
				goto err_defio_cleanup;
		} else {
			lcd->te_irq = 0;
		}
	}

	mutex_lock(&lcd->lock);
	ret = rpi5_ili9341_init_display(lcd);
	mutex_unlock(&lcd->lock);
	if (ret)
		goto err_defio_cleanup;

	ret = register_framebuffer(info);
	if (ret)
		goto err_display_off;

	snprintf(lcd->misc_name, sizeof(lcd->misc_name), "%s", RPI5_ILI9341_NAME);
	lcd->miscdev.minor = MISC_DYNAMIC_MINOR;
	lcd->miscdev.name = lcd->misc_name;
	lcd->miscdev.fops = &rpi5_ili9341_chr_fops;
	lcd->miscdev.parent = dev;
	ret = misc_register(&lcd->miscdev);
	if (ret)
		goto err_unregister_fb;

	rpi5_ili9341_mark_dirty(lcd, 0, 0, info->var.xres, info->var.yres);
	rpi5_ili9341_schedule_flush(lcd);

	dev_info(dev,
		 "registered /dev/%s and /dev/fb%d (%ux%u@%uHz SPI, rotation=%u)\n",
		 lcd->misc_name, info->node, info->var.xres, info->var.yres,
		 lcd->max_speed_hz, lcd->rotation);

	return 0;

err_unregister_fb:
	unregister_framebuffer(info);
err_display_off:
	mutex_lock(&lcd->lock);
	rpi5_ili9341_write_cmd(lcd, ILI9341_DISPOFF);
	rpi5_ili9341_write_cmd(lcd, ILI9341_SLPIN);
	mutex_unlock(&lcd->lock);
err_defio_cleanup:
	cancel_work_sync(&lcd->flush_work);
	fb_deferred_io_cleanup(info);
err_free_vmem:
	vfree(lcd->txbuf);
	vfree(lcd->vmem);
err_release_fb:
	framebuffer_release(info);
	return ret;
}

static void rpi5_ili9341_remove(struct spi_device *spi)
{
	struct rpi5_ili9341 *lcd = spi_get_drvdata(spi);

	misc_deregister(&lcd->miscdev);
	unregister_framebuffer(lcd->info);
	cancel_work_sync(&lcd->flush_work);
	fb_deferred_io_cleanup(lcd->info);

	mutex_lock(&lcd->lock);
	if (lcd->led_gpio)
		gpiod_set_value_cansleep(lcd->led_gpio, 0);
	rpi5_ili9341_write_cmd(lcd, ILI9341_DISPOFF);
	rpi5_ili9341_write_cmd(lcd, ILI9341_SLPIN);
	mutex_unlock(&lcd->lock);

	vfree(lcd->txbuf);
	vfree(lcd->vmem);
	framebuffer_release(lcd->info);
}

static const struct of_device_id rpi5_ili9341_of_match[] = {
	{ .compatible = "codex,rpi5-ili9341" },
	{ .compatible = "ilitek,ili9341" },
	{ }
};
MODULE_DEVICE_TABLE(of, rpi5_ili9341_of_match);

static const struct spi_device_id rpi5_ili9341_id[] = {
	{ "codex,rpi5-ili9341", 0 },
	{ "rpi5_ili9341", 0 },
	{ "ili9341", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, rpi5_ili9341_id);

static struct spi_driver rpi5_ili9341_driver = {
	.driver = {
		.name = RPI5_ILI9341_NAME,
		.of_match_table = rpi5_ili9341_of_match,
	},
	.probe = rpi5_ili9341_probe,
	.remove = rpi5_ili9341_remove,
	.id_table = rpi5_ili9341_id,
};
module_spi_driver(rpi5_ili9341_driver);

MODULE_AUTHOR("OpenAI Codex");
MODULE_DESCRIPTION("Raspberry Pi 5 ILI9341 SPI framebuffer and character device driver");
MODULE_LICENSE("GPL");
