#include "image.h"

#ifdef SNOW_NO_PNG

int sample_image_tint(const char *path, float out[4])
{
    (void)path;
    out[0] = out[1] = out[2] = out[3] = 0;
    return 0;
}

#else

#include <png.h>
#include <stdlib.h>
#include <string.h>

/* The average colour of the picture's bottom quarter, brought to the
 * brightness of snow in shade: same curve as the original's CoreGraphics pass. */
int sample_image_tint(const char *path, float out[4])
{
    out[0] = out[1] = out[2] = 0;
    out[3] = 0;

    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path)) return 0;

    image.format = PNG_FORMAT_RGBA;
    size_t size = PNG_IMAGE_SIZE(image);
    unsigned char *pixels = malloc(size);
    if (!pixels) {
        png_image_free(&image);
        return 0;
    }
    if (!png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
        free(pixels);
        png_image_free(&image);
        return 0;
    }

    unsigned width = image.width, height = image.height;
    double sum[3] = {0, 0, 0};
    double count = 0;
    for (unsigned y = height * 3 / 4; y < height; y += 2) {
        for (unsigned x = 0; x < width; x += 2) {
            size_t i = ((size_t)y * width + x) * 4;
            sum[0] += pixels[i] / 255.0;
            sum[1] += pixels[i + 1] / 255.0;
            sum[2] += pixels[i + 2] / 255.0;
            count += 1;
        }
    }
    free(pixels);
    png_image_free(&image);
    if (count <= 0) return 0;

    double average[3];
    for (int k = 0; k < 3; k++) average[k] = sum[k] / count;
    double luminance = average[0] * 0.2126 + average[1] * 0.7152 + average[2] * 0.0722;
    double shade = 0.775;
    double scale = shade / (luminance > 0.04 ? luminance : 0.04);
    for (int k = 0; k < 3; k++) {
        double value = average[k] * scale;
        out[k] = (float)(value > 1 ? 1 : value);
    }
    out[3] = 0.35f;
    return 1;
}

#endif
