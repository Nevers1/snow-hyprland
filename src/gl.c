#include "gl.h"

#include <GLES3/gl3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *spriteVertexSource =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aVel;\n"
    "layout(location=2) in float aSize;\n"
    "layout(location=3) in float aAlpha;\n"
    "layout(location=4) in float aSoft;\n"
    "layout(location=5) in float aTintAmount;\n"
    "layout(location=6) in vec3 aTint;\n"
    "layout(location=7) in float aLump;\n"
    "uniform vec2 uViewSize;\n"
    "uniform float uScale;\n"
    "uniform float uTime;\n"
    "uniform vec3 uLight;\n"
    "out vec2 vCoord;\n"
    "out float vAlpha;\n"
    "out float vSoft;\n"
    "out vec4 vTint;\n"
    "out vec3 vLight;\n"
    "out vec2 vStreak;\n"
    "out float vRadius;\n"
    "out float vLump;\n"
    "out float vSpin;\n"
    "void main() {\n"
    "  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  float pixels = aSize * uScale;\n"
    "  // About a 1/55 s exposure, capped so even the fastest grain stays a short smear.\n"
    "  vec2 travel = aVel * 0.018;\n"
    "  float smear = min(length(travel), aSize * (aLump > 0.0 ? 1.2 : 2.2));\n"
    "  vec2 streak = smear > 0.25 ? normalize(travel) * smear * uScale : vec2(0.0);\n"
    "  float reach = aLump > 0.0 ? 1.3 : 1.0;\n"
    "  float extent = max(pixels * reach + length(streak), 2.0);\n"
    "  vec2 offset = vec2(corner.x - 0.5, 0.5 - corner.y) * extent;\n"
    "  gl_Position = vec4((aPos + offset) / uViewSize * 2.0 - 1.0, 0.0, 1.0);\n"
    "  vCoord = corner;\n"
    "  vStreak = streak / extent;\n"
    "  vRadius = pixels / extent * 0.5;\n"
    "  // A streak spreads the same flake over more pixels, so each is fainter.\n"
    "  vAlpha = aAlpha * min(pixels / 2.0, 1.0) * pixels / max(pixels + length(streak) * 0.6, 0.001);\n"
    "  vSoft = aSoft;\n"
    "  vTint = vec4(aTint, aTintAmount);\n"
    "  vLight = uLight;\n"
    "  vLump = aLump;\n"
    "  vSpin = aLump > 0.0 ? uTime * (aLump - 0.5) * 9.0 + aLump * 40.0 : 0.0;\n"
    "}\n";

static const char *spriteFragmentSource =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "in vec2 vCoord;\n"
    "in float vAlpha;\n"
    "in float vSoft;\n"
    "in vec4 vTint;\n"
    "in vec3 vLight;\n"
    "in vec2 vStreak;\n"
    "in float vRadius;\n"
    "in float vLump;\n"
    "in float vSpin;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  // Point coordinates run down the screen; velocities run up.\n"
    "  vec2 p = vec2(vCoord.x - 0.5, 0.5 - vCoord.y);\n"
    "  vec2 half_ = vStreak * 0.5;\n"
    "  float span = dot(half_, half_);\n"
    "  float t = span > 0.0 ? clamp(dot(p, half_) / span, -1.0, 1.0) : 0.0;\n"
    "  vec2 d = p - half_ * t;\n"
    "  float r = length(d) / max(vRadius, 0.001);\n"
    "  float shading = 0.0;\n"
    "  if (vLump > 0.0) {\n"
    "    float angle = atan(d.y, d.x) + vSpin;\n"
    "    r /= 1.0 + 0.14 * sin(3.0 * angle) + 0.08 * sin(5.0 * angle + vLump * 21.0)\n"
    "                + 0.05 * sin(2.0 * angle + vLump * 9.0);\n"
    "    shading = clamp(dot(d / max(vRadius, 0.001), vec2(0.45, -0.6)), 0.0, 1.0) * 0.4;\n"
    "  }\n"
    "  // Brighter at the head, where the flake is now, than at the tail, where it was.\n"
    "  float head = span > 0.0 ? 0.15 + 0.85 * pow(t * 0.5 + 0.5, 1.5) : 1.0;\n"
    "  float a = (1.0 - smoothstep(mix(0.6, 0.0, vSoft), 1.0, r)) * vAlpha * head;\n"
    "  if (a < 0.002) discard;\n"
    "  vec3 core = vec3(0.965, 0.975, 0.995);\n"
    "  vec3 rim = vec3(0.62, 0.67, 0.77);\n"
    "  vec3 color = mix(core, rim, clamp(smoothstep(0.35, 1.0, r) * 0.65 + shading, 0.0, 1.0));\n"
    "  color = mix(color, vTint.rgb, vTint.a) * vLight;\n"
    "  fragColor = vec4(color * a, a);\n"
    "}\n";

static const char *pileVertexSource =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform vec2 uViewSize;\n"
    "uniform float uColumnWidth;\n"
    "uniform int uColumnCount;\n"
    "uniform float uPileTop;\n"
    "uniform float uPileBottom;\n"
    "uniform vec2 uOrigin;\n"
    "out vec2 vPoint;\n"
    "void main() {\n"
    "  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 p = vec2(corner.x * float(uColumnCount) * uColumnWidth,\n"
    "                mix(uPileBottom, uPileTop, corner.y));\n"
    "  gl_Position = vec4((p + uOrigin) / uViewSize * 2.0 - 1.0, 0.0, 1.0);\n"
    "  vPoint = p;\n"
    "}\n";

static const char *pileFragmentSource =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "in vec2 vPoint;\n"
    "uniform highp sampler2D uHeights;\n"
    "uniform vec2 uViewSize;\n"
    "uniform float uScale;\n"
    "uniform float uTime;\n"
    "uniform float uColumnWidth;\n"
    "uniform int uColumnCount;\n"
    "uniform vec2 uOrigin;\n"
    "uniform vec3 uLight;\n"
    "uniform vec4 uShadowTint;\n"
    "uniform int uOccluderCount;\n"
    "uniform vec4 uOccluders[16];\n"
    "uniform int uHeightBase;\n"
    "out vec4 fragColor;\n"
    "float heightAt(float x, int base) {\n"
    "  float f = x / uColumnWidth - 0.5;\n"
    "  float fl = floor(f);\n"
    "  int i = clamp(int(fl), 0, uColumnCount - 1);\n"
    "  int j = min(i + 1, uColumnCount - 1);\n"
    "  int off = uHeightBase + base;\n"
    "  float a = texelFetch(uHeights, ivec2(off + i, 0), 0).r;\n"
    "  float b = texelFetch(uHeights, ivec2(off + j, 0), 0).r;\n"
    "  return mix(a, b, clamp(f - fl, 0.0, 1.0));\n"
    "}\n"
    "float hash(vec2 p) {\n"
    "  p = fract(p * vec2(123.34, 456.21));\n"
    "  p += dot(p, p + 45.32);\n"
    "  return fract(p.x * p.y);\n"
    "}\n"
    "void main() {\n"
    "  vec2 world = vPoint + uOrigin;\n"
    "  for (int k = 0; k < 16; k++) {\n"
    "    if (k >= uOccluderCount) break;\n"
    "    vec4 o = uOccluders[k];\n"
    "    if (world.x >= o.x && world.x < o.z && world.y >= o.y && world.y < o.w) discard;\n"
    "  }\n"
    "  float x = vPoint.x;\n"
    "  float level = heightAt(x, 0);\n"
    "  float bed = heightAt(x, uColumnCount);\n"
    "  float depth = level - vPoint.y;\n"
    "  float coverage = clamp(depth * uScale + 0.5, 0.0, 1.0)\n"
    "      * clamp((vPoint.y - bed) * uScale + 0.5, 0.0, 1.0)\n"
    "      * clamp((level - bed) / 0.75, 0.0, 1.0);\n"
    "  if (coverage <= 0.0) discard;\n"
    "  float slope = (heightAt(x + 2.0, 0) - heightAt(x - 2.0, 0)) * 0.25;\n"
    "  float light = dot(normalize(vec2(-slope, 1.0)), normalize(vec2(-0.5, 1.0)));\n"
    "  vec3 shadow = mix(vec3(0.72, 0.78, 0.88), uShadowTint.rgb, uShadowTint.a);\n"
    "  vec3 surface = mix(vec3(0.925, 0.94, 0.965), min(uShadowTint.rgb * 1.18, vec3(1.0)),\n"
    "                     uShadowTint.a * 0.2);\n"
    "  float shade = (1.0 - exp(-depth / 30.0)) * 0.32 + (1.0 - light) * 0.6 * exp(-depth / 5.0);\n"
    "  vec3 color = mix(surface, shadow, clamp(shade, 0.0, 1.0));\n"
    "  color = mix(color, vec3(0.975, 0.982, 0.995), exp(-depth * 0.9) * 0.6);\n"
    "  color += (hash(floor(gl_FragCoord.xy)) - 0.5) * 0.02;\n"
    "  float seed = hash(floor(gl_FragCoord.xy * 0.5) + 31.7);\n"
    "  if (seed > 0.992) {\n"
    "    float twinkle = pow(clamp(sin(uTime * (0.6 + seed * 1.8) + seed * 90.0), 0.0, 1.0), 28.0);\n"
    "    color = mix(color, vec3(1.0), twinkle * exp(-depth / 7.0));\n"
    "  }\n"
    "  color *= uLight;\n"
    "  fragColor = vec4(color * coverage, coverage);\n"
    "}\n";

struct Renderer {
    GLuint spriteProgram;
    GLuint pileProgram;
    GLuint spriteVAO;
    GLuint spriteVBO;
    int spriteCapacity;
    GLuint pileVAO;
    GLuint heightsTexture;
    int heightsCapacity;

    GLint uSpriteViewSize, uSpriteScale, uSpriteTime, uSpriteLight;
    GLint uPileViewSize, uPileScale, uPileTime, uPileColumnWidth, uPileColumnCount;
    GLint uPileTop, uPileBottom, uPileOrigin, uPileLight, uPileShadowTint;
    GLint uPileOccluderCount, uPileOccluders, uPileHeights, uPileHeightBase;
};

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "snowhush: shader error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(const char *vertexSource, const char *fragmentSource)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vertexSource);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "snowhush: link error: %s\n", log);
        glDeleteProgram(program);
        program = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

Renderer *renderer_new(void)
{
    Renderer *r = calloc(1, sizeof(Renderer));
    r->spriteProgram = link_program(spriteVertexSource, spriteFragmentSource);
    r->pileProgram = link_program(pileVertexSource, pileFragmentSource);
    if (!r->spriteProgram || !r->pileProgram) {
        renderer_free(r);
        return NULL;
    }

    r->uSpriteViewSize = glGetUniformLocation(r->spriteProgram, "uViewSize");
    r->uSpriteScale = glGetUniformLocation(r->spriteProgram, "uScale");
    r->uSpriteTime = glGetUniformLocation(r->spriteProgram, "uTime");
    r->uSpriteLight = glGetUniformLocation(r->spriteProgram, "uLight");

    r->uPileViewSize = glGetUniformLocation(r->pileProgram, "uViewSize");
    r->uPileScale = glGetUniformLocation(r->pileProgram, "uScale");
    r->uPileTime = glGetUniformLocation(r->pileProgram, "uTime");
    r->uPileColumnWidth = glGetUniformLocation(r->pileProgram, "uColumnWidth");
    r->uPileColumnCount = glGetUniformLocation(r->pileProgram, "uColumnCount");
    r->uPileTop = glGetUniformLocation(r->pileProgram, "uPileTop");
    r->uPileBottom = glGetUniformLocation(r->pileProgram, "uPileBottom");
    r->uPileOrigin = glGetUniformLocation(r->pileProgram, "uOrigin");
    r->uPileLight = glGetUniformLocation(r->pileProgram, "uLight");
    r->uPileShadowTint = glGetUniformLocation(r->pileProgram, "uShadowTint");
    r->uPileOccluderCount = glGetUniformLocation(r->pileProgram, "uOccluderCount");
    r->uPileOccluders = glGetUniformLocation(r->pileProgram, "uOccluders");
    r->uPileHeights = glGetUniformLocation(r->pileProgram, "uHeights");
    r->uPileHeightBase = glGetUniformLocation(r->pileProgram, "uHeightBase");

    glGenVertexArrays(1, &r->spriteVAO);
    glGenBuffers(1, &r->spriteVBO);
    glBindVertexArray(r->spriteVAO);
    glBindBuffer(GL_ARRAY_BUFFER, r->spriteVBO);
    const GLsizei stride = sizeof(Sprite);
#define ATTR(index, count, type, normalized, offset)                                       \
    do {                                                                                    \
        glEnableVertexAttribArray(index);                                                   \
        glVertexAttribPointer(index, count, type, normalized, stride, (void *)(offset));    \
        glVertexAttribDivisor(index, 1);                                                    \
    } while (0)
    ATTR(0, 2, GL_FLOAT, GL_FALSE, offsetof(Sprite, px));
    ATTR(1, 2, GL_FLOAT, GL_FALSE, offsetof(Sprite, vx));
    ATTR(2, 1, GL_FLOAT, GL_FALSE, offsetof(Sprite, size));
    ATTR(3, 1, GL_FLOAT, GL_FALSE, offsetof(Sprite, alpha));
    ATTR(4, 1, GL_FLOAT, GL_FALSE, offsetof(Sprite, soft));
    ATTR(5, 1, GL_FLOAT, GL_FALSE, offsetof(Sprite, tintAmount));
    ATTR(6, 3, GL_FLOAT, GL_FALSE, offsetof(Sprite, tintR));
    ATTR(7, 1, GL_FLOAT, GL_FALSE, offsetof(Sprite, lump));
#undef ATTR
    glBindVertexArray(0);

    glGenVertexArrays(1, &r->pileVAO);
    glGenTextures(1, &r->heightsTexture);
    glBindTexture(GL_TEXTURE_2D, r->heightsTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return r;
}

void renderer_free(Renderer *r)
{
    if (!r) return;
    if (r->spriteProgram) glDeleteProgram(r->spriteProgram);
    if (r->pileProgram) glDeleteProgram(r->pileProgram);
    if (r->spriteVAO) glDeleteVertexArrays(1, &r->spriteVAO);
    if (r->spriteVBO) glDeleteBuffers(1, &r->spriteVBO);
    if (r->pileVAO) glDeleteVertexArrays(1, &r->pileVAO);
    if (r->heightsTexture) glDeleteTextures(1, &r->heightsTexture);
    free(r);
}

static void upload_heights(Renderer *r, const float *heights, int count)
{
    glBindTexture(GL_TEXTURE_2D, r->heightsTexture);
    if (count > r->heightsCapacity) {
        r->heightsCapacity = count * 2 + 256;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, r->heightsCapacity, 1, 0, GL_RED, GL_FLOAT, NULL);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count, 1, GL_RED, GL_FLOAT, heights);
}

void renderer_draw(Renderer *r,
                   int width, int height, float scale, float time,
                   const Sprite *sprites, int spriteCount,
                   const float *heights, int heightCount,
                   const PileLayer *piles, int pileCount,
                   const float light[3], const float shadowTint[4])
{
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    float viewSize[2] = {(float)width, (float)height};

    if (pileCount > 0 && heightCount > 0) {
        upload_heights(r, heights, heightCount);
        glUseProgram(r->pileProgram);
        glUniform2fv(r->uPileViewSize, 1, viewSize);
        glUniform1f(r->uPileScale, scale);
        glUniform1f(r->uPileTime, time);
        glUniform1f(r->uPileColumnWidth, 2.0f);
        glUniform3fv(r->uPileLight, 1, light);
        glUniform4fv(r->uPileShadowTint, 1, shadowTint);
        glUniform1i(r->uPileHeights, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r->heightsTexture);
        glBindVertexArray(r->pileVAO);
        float occluders[16][4] = {{0}};
        for (int i = 0; i < pileCount; i++) {
            const PileLayer *p = &piles[i];
            glUniform1i(r->uPileColumnCount, p->count);
            glUniform1i(r->uPileHeightBase, p->offset);
            glUniform1f(r->uPileTop, p->top + 2);
            glUniform1f(r->uPileBottom, p->bottom);
            glUniform2f(r->uPileOrigin, p->originX, p->originY);
            glUniform1i(r->uPileOccluderCount, p->occluderCount);
            if (p->occluderCount > 0) {
                for (int k = 0; k < p->occluderCount; k++)
                    memcpy(occluders[k], p->occluders[k], sizeof(float) * 4);
                glUniform4fv(r->uPileOccluders, p->occluderCount, &occluders[0][0]);
            }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glBindVertexArray(0);
    }

    if (spriteCount > 0) {
        if (spriteCount > r->spriteCapacity) {
            r->spriteCapacity = spriteCount * 3 / 2 + 256;
        }
        glBindBuffer(GL_ARRAY_BUFFER, r->spriteVBO);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(Sprite) * (size_t)r->spriteCapacity), NULL, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(sizeof(Sprite) * (size_t)spriteCount), sprites);
        glUseProgram(r->spriteProgram);
        glUniform2fv(r->uSpriteViewSize, 1, viewSize);
        glUniform1f(r->uSpriteScale, scale);
        glUniform1f(r->uSpriteTime, time);
        glUniform3fv(r->uSpriteLight, 1, light);
        glBindVertexArray(r->spriteVAO);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, spriteCount);
        glBindVertexArray(0);
    }
}
