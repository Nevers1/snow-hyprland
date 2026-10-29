/* The colour of the desktop picture, for the tint the snow's shadows lean
 * toward. Uses libpng; if it is not available the build falls back to plain
 * snow-white shading. */
#ifndef SNOW_IMAGE_H
#define SNOW_IMAGE_H

/* Samples the bottom quarter of the image and writes {r, g, b, amount}. */
int sample_image_tint(const char *path, float out[4]);

#endif
