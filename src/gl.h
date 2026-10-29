/* The GPU side: two programs, one for flakes and one for drifts.
 *
 * Same shading as the Metal original: point-size sprites become instanced
 * quads (a 4-vertex strip per sprite), and the drift is a triangle strip over
 * a height field read from a float texture.
 */
#ifndef SNOW_GL_H
#define SNOW_GL_H

#include "sim.h"

typedef struct Renderer Renderer;

Renderer *renderer_new(void);
void renderer_free(Renderer *r);

void renderer_draw(Renderer *r,
                   int width, int height, float scale, float time,
                   const Sprite *sprites, int spriteCount,
                   const float *heights, int heightCount,
                   const PileLayer *piles, int pileCount,
                   const float light[3], const float shadowTint[4]);

#endif
