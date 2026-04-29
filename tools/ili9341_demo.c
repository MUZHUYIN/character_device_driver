#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/rpi5_ili9341.h>

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xf8) << 8) |
			  ((g & 0xfc) << 3) |
			  (b >> 3));
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [--device /dev/rpi5_ili9341] info\n"
		"  %s [--device /dev/rpi5_ili9341] clear <rgb565-hex>\n"
		"  %s [--device /dev/rpi5_ili9341] gradient\n"
		"  %s [--device /dev/rpi5_ili9341] bars\n"
		"  %s [--device /dev/rpi5_ili9341] solid-blue\n"
		"  %s [--device /dev/rpi5_ili9341] rotate <0|90|180|270>\n"
		"  %s [--device /dev/rpi5_ili9341] backlight <0|1>\n",
		prog, prog, prog, prog, prog, prog, prog);
}

static void fill_color(uint16_t *fb, const struct rpi5_ili9341_panel_info *info,
		       uint16_t color)
{
	uint32_t stride = info->stride / sizeof(uint16_t);

	for (uint32_t y = 0; y < info->height; ++y) {
		uint16_t *line = fb + y * stride;

		for (uint32_t x = 0; x < info->width; ++x)
			line[x] = color;
	}
}

static void draw_gradient(uint16_t *fb,
			  const struct rpi5_ili9341_panel_info *info)
{
	uint32_t stride = info->stride / sizeof(uint16_t);

	for (uint32_t y = 0; y < info->height; ++y) {
		uint16_t *line = fb + y * stride;

		for (uint32_t x = 0; x < info->width; ++x) {
			uint8_t r = (uint8_t)((x * 255U) / (info->width - 1U));
			uint8_t g = (uint8_t)((y * 255U) / (info->height - 1U));
			uint8_t b = (uint8_t)(255U - r);

			line[x] = rgb565(r, g, b);
		}
	}
}

static void draw_bars(uint16_t *fb,
		      const struct rpi5_ili9341_panel_info *info)
{
	static const uint16_t palette[] = {
		0xf800, 0x07e0, 0x001f, 0xffe0,
		0xf81f, 0x07ff, 0xffff, 0x0000
	};
	uint32_t stride = info->stride / sizeof(uint16_t);
	uint32_t bar_width = info->width / (uint32_t)(sizeof(palette) / sizeof(palette[0]));

	for (uint32_t y = 0; y < info->height; ++y) {
		uint16_t *line = fb + y * stride;

		for (uint32_t x = 0; x < info->width; ++x) {
			size_t idx = x / (bar_width ? bar_width : 1U);

			if (idx >= sizeof(palette) / sizeof(palette[0]))
				idx = (sizeof(palette) / sizeof(palette[0])) - 1U;
			line[x] = palette[idx];
		}
	}
}

static int map_and_draw(int fd, const struct rpi5_ili9341_panel_info *info,
			void (*draw_fn)(uint16_t *fb,
					const struct rpi5_ili9341_panel_info *info))
{
	size_t length = (size_t)info->stride * info->height;
	uint16_t *fb = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (fb == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	draw_fn(fb, info);

	if (ioctl(fd, RPI5_ILI9341_IOC_FLUSH) < 0) {
		perror("ioctl(FLUSH)");
		munmap(fb, length);
		return -1;
	}

	munmap(fb, length);
	return 0;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/rpi5_ili9341";
	struct rpi5_ili9341_panel_info info;
	int fd;
	int argi = 1;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	if (argc > 3 && strcmp(argv[1], "--device") == 0) {
		device = argv[2];
		argi = 3;
	}

	if (argi >= argc) {
		usage(argv[0]);
		return 1;
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror(device);
		return 1;
	}

	if (ioctl(fd, RPI5_ILI9341_IOC_GET_INFO, &info) < 0) {
		perror("ioctl(GET_INFO)");
		close(fd);
		return 1;
	}

	if (strcmp(argv[argi], "info") == 0) {
		printf("device=%s\n", device);
		printf("resolution=%ux%u\n", info.width, info.height);
		printf("stride=%u\n", info.stride);
		printf("bpp=%u\n", info.bpp);
		printf("rotation=%u\n", info.rotation);
		printf("te=%s\n", info.has_te ? "yes" : "no");
		close(fd);
		return 0;
	}

	if (strcmp(argv[argi], "clear") == 0) {
		unsigned long color;
		uint16_t rgb;

		if (argi + 1 >= argc) {
			usage(argv[0]);
			close(fd);
			return 1;
		}

		errno = 0;
		color = strtoul(argv[argi + 1], NULL, 16);
		if (errno || color > 0xffffUL) {
			fprintf(stderr, "invalid rgb565 color: %s\n", argv[argi + 1]);
			close(fd);
			return 1;
		}

		rgb = (uint16_t)color;
		if (ioctl(fd, RPI5_ILI9341_IOC_CLEAR, &rgb) < 0) {
			perror("ioctl(CLEAR)");
			close(fd);
			return 1;
		}

		close(fd);
		return 0;
	}

	if (strcmp(argv[argi], "gradient") == 0) {
		int rc = map_and_draw(fd, &info, draw_gradient);

		close(fd);
		return rc ? 1 : 0;
	}

	if (strcmp(argv[argi], "bars") == 0) {
		int rc = map_and_draw(fd, &info, draw_bars);

		close(fd);
		return rc ? 1 : 0;
	}

	if (strcmp(argv[argi], "rotate") == 0) {
		uint32_t rotate;

		if (argi + 1 >= argc) {
			usage(argv[0]);
			close(fd);
			return 1;
		}

		errno = 0;
		rotate = (uint32_t)strtoul(argv[argi + 1], NULL, 10);
		if (errno) {
			perror("strtoul");
			close(fd);
			return 1;
		}

		if (ioctl(fd, RPI5_ILI9341_IOC_SET_ROTATION, &rotate) < 0) {
			perror("ioctl(SET_ROTATION)");
			close(fd);
			return 1;
		}

		close(fd);
		return 0;
	}

	if (strcmp(argv[argi], "backlight") == 0) {
		uint32_t on;

		if (argi + 1 >= argc) {
			usage(argv[0]);
			close(fd);
			return 1;
		}

		errno = 0;
		on = (uint32_t)strtoul(argv[argi + 1], NULL, 10);
		if (errno || on > 1U) {
			fprintf(stderr, "backlight must be 0 or 1\n");
			close(fd);
			return 1;
		}

		if (ioctl(fd, RPI5_ILI9341_IOC_SET_BACKLIGHT, &on) < 0) {
			perror("ioctl(SET_BACKLIGHT)");
			close(fd);
			return 1;
		}

		close(fd);
		return 0;
	}

	if (strcmp(argv[argi], "solid-blue") == 0) {
		size_t length = (size_t)info.stride * info.height;
		uint16_t *fb = mmap(NULL, length, PROT_READ | PROT_WRITE,
				    MAP_SHARED, fd, 0);

		if (fb == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return 1;
		}

		fill_color(fb, &info, 0x001f);
		if (ioctl(fd, RPI5_ILI9341_IOC_FLUSH) < 0)
			perror("ioctl(FLUSH)");
		munmap(fb, length);
		close(fd);
		return 0;
	}

	usage(argv[0]);
	close(fd);
	return 1;
}
