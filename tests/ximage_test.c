#define _GNU_SOURCE

#include "xeh_ximage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static XImage
image_888(char *data)
{
    XImage image = {0};
    image.width = 2;
    image.height = 2;
    image.format = ZPixmap;
    image.data = data;
    image.byte_order = LSBFirst;
    image.bitmap_unit = 32;
    image.bitmap_bit_order = LSBFirst;
    image.bitmap_pad = 32;
    image.depth = 24;
    image.bytes_per_line = 12;
    image.bits_per_pixel = 32;
    image.red_mask = 0x00ff0000UL;
    image.green_mask = 0x0000ff00UL;
    image.blue_mask = 0x000000ffUL;
    return image;
}

static void
test_888_image(void)
{
    char source[24] = {
        0, 0, (char)255, 99, 0, (char)255, 0, 99, 7, 7, 7, 7,
        (char)255, 0, 0, 99, (char)255, (char)255, (char)255, 99,
        7, 7, 7, 7,
    };
    const uint8_t expected[16] = {
        0, 0, 255, 0, 0, 255, 0, 0,
        255, 0, 0, 0, 255, 255, 255, 0,
    };
    XImage image = image_888(source);
    xeh_shm_info info;
    uint8_t actual[16];
    int fd = xeh_ximage_to_memfd(&image, &info);
    CHECK(fd >= 0);
    CHECK(info.width == 2 && info.height == 2 && info.stride == 8);
    CHECK(info.format == XEH_BUFFER_FORMAT_XRGB8888 && info.size == 16);
    CHECK(pread(fd, actual, sizeof(actual), 0) == (ssize_t)sizeof(actual));
    CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    CHECK((fcntl(fd, F_GET_SEALS) &
           (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW)) ==
          (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW));
    CHECK(image.f.get_pixel == NULL);
    CHECK(source[3] == 99);
    close(fd);
}

static void
test_565_image(void)
{
    char source[4] = {0, (char)0xf8, (char)0xe0, 0x07};
    const uint8_t expected[8] = {0, 0, 255, 0, 0, 255, 0, 0};
    XImage image = image_888(source);
    xeh_shm_info info;
    uint8_t actual[8];
    int fd;
    image.width = 2;
    image.height = 1;
    image.depth = 16;
    image.bits_per_pixel = 16;
    image.bitmap_unit = 16;
    image.bitmap_pad = 16;
    image.bytes_per_line = 4;
    image.red_mask = 0xf800UL;
    image.green_mask = 0x07e0UL;
    image.blue_mask = 0x001fUL;
    fd = xeh_ximage_to_memfd(&image, &info);
    CHECK(fd >= 0);
    CHECK(info.stride == 8 && info.size == 8);
    CHECK(pread(fd, actual, sizeof(actual), 0) == (ssize_t)sizeof(actual));
    CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
    close(fd);
}

static void
test_byte_orders(void)
{
    char big_endian[4] = {0x11, 0x22, 0x33, 0x44};
    char packed_24[3] = {0x12, 0x34, 0x56};
    uint8_t actual[4];
    xeh_shm_info info;
    XImage image = image_888(big_endian);
    int fd;
    image.width = 1;
    image.height = 1;
    image.bytes_per_line = 4;
    image.byte_order = MSBFirst;
    image.bitmap_bit_order = MSBFirst;
    fd = xeh_ximage_to_memfd(&image, &info);
    CHECK(fd >= 0);
    CHECK(pread(fd, actual, sizeof(actual), 0) == (ssize_t)sizeof(actual));
    CHECK(actual[0] == 0x44 && actual[1] == 0x33 &&
          actual[2] == 0x22 && actual[3] == 0);
    close(fd);

    image = image_888(packed_24);
    image.width = 1;
    image.height = 1;
    image.bits_per_pixel = 24;
    image.bitmap_unit = 8;
    image.bitmap_pad = 8;
    image.bytes_per_line = 3;
    fd = xeh_ximage_to_memfd(&image, &info);
    CHECK(fd >= 0);
    CHECK(pread(fd, actual, sizeof(actual), 0) == (ssize_t)sizeof(actual));
    CHECK(actual[0] == 0x12 && actual[1] == 0x34 &&
          actual[2] == 0x56 && actual[3] == 0);
    close(fd);
}

static void
test_invalid_image(void)
{
    char source[24] = {0};
    XImage image = image_888(source);
    xeh_shm_info info;
    CHECK(xeh_ximage_to_memfd(NULL, &info) < 0 && errno == EINVAL);
    CHECK(xeh_ximage_to_memfd(&image, NULL) < 0 && errno == EINVAL);
    image.bytes_per_line = 7;
    CHECK(xeh_ximage_to_memfd(&image, &info) < 0 && errno == EINVAL);
    CHECK(info.size == 0);
    image = image_888(source);
    image.red_mask = 0;
    CHECK(xeh_ximage_to_memfd(&image, &info) < 0 && errno == EINVAL);
    image = image_888(source);
    image.format = XYPixmap;
    CHECK(xeh_ximage_to_memfd(&image, &info) < 0 && errno == EINVAL);
    image = image_888(source);
    image.width = (int)XEH_BUFFER_MAX_DIMENSION + 1;
    CHECK(xeh_ximage_to_memfd(&image, &info) < 0 && errno == EINVAL);
    image = image_888(source);
    image.data = NULL;
    CHECK(xeh_ximage_to_memfd(&image, &info) < 0 && errno == EINVAL);
    CHECK(xeh_import_ximage(NULL, &image, NULL, NULL) == XEH_ERR_ARGUMENT);
}

int
main(void)
{
    test_888_image();
    test_565_image();
    test_byte_orders();
    test_invalid_image();
    return 0;
}
