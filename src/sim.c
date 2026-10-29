#include "sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COLUMN_WIDTH 2.0f
#define GROUND_DEPTH 40.0f
#define LEDGE_DEPTH 14.0f
#define THAW_DEPTH 8.0f
#define LEDGE_THAW_DEPTH 4.0f
#define THAW_RATE 0.02f
#define THAW_TIME 30.0f
#define THAW_BALANCE 184.0f
#define THAW_EXPONENT 0.74f
#define DEPOSITION 1.1f
#define REPOSE 0.8f
#define ROUNDING 0.15f
#define CURSOR_RADIUS 10.0f
#define CURSOR_OFFSET_X 4.0f
#define CURSOR_OFFSET_Y -8.0f
#define GRAVITY 2500.0f
#define GRAIN_DRAG 0.8f
#define MAX_GRAINS 12000
#define PHASE_WRAP (20.0f * (float)M_PI)
#define GRIP 3.0f
#define SLIP 2.0f
#define BREAK_LIFT 10.0f
#define ADHESION 3500.0f
#define SHAKING 4.0f
#define JUMP_SPEED 6000.0f
#define TRACK_TIME 0.06f
#define MAX_MOTES 1500
#define GUST_SWEEP 0.6f
#define GUST_LENGTH 2.2f
#define GUST_WIND 1000.0f
#define GUST_LAG 0.3f
#define PACKING 0.6f

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float saturatef(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float sqrf(float v) { return v * v; }

static inline float smoothstepf(float edge0, float edge1, float x)
{
    float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline v2 v2make(float x, float y) { v2 r = {x, y}; return r; }
static inline v2 v2add(v2 a, v2 b) { return v2make(a.x + b.x, a.y + b.y); }
static inline v2 v2sub(v2 a, v2 b) { return v2make(a.x - b.x, a.y - b.y); }
static inline v2 v2mul(v2 a, float s) { return v2make(a.x * s, a.y * s); }
static inline float v2dot(v2 a, v2 b) { return a.x * b.x + a.y * b.y; }
static inline float v2len(v2 a) { return sqrtf(a.x * a.x + a.y * a.y); }
static inline float v2len2(v2 a) { return a.x * a.x + a.y * a.y; }

typedef struct {
    uint64_t state;
} RandomSource;

static float rnd_next(RandomSource *r)
{
    r->state ^= r->state << 13;
    r->state ^= r->state >> 7;
    r->state ^= r->state << 17;
    return (float)(r->state >> 40) / (float)(1u << 24);
}

static float rnd_range(RandomSource *r, float lower, float upper)
{
    return lower + (upper - lower) * rnd_next(r);
}

/* ------------------------------------------------------------------ */
/* particles                                                           */

typedef struct {
    v2 position;
    v2 velocity;
    float fallSpeed;
    float size;
    float depth;
    float alpha;
    float soft;
    float phase;
    float swayRate;
    float swayAmount;
    int settles;
    float mass;
    float tint[3];
    float tintAmount;
    float blown;
} Flake;

typedef struct {
    v2 position;
    v2 velocity;
    float area;
    float size;
    float lump;
    int blown;
} Grain;

typedef struct {
    v2 position;
    v2 velocity;
    float size;
    float alpha;
    float age;
    float life;
} Mote;

/* ------------------------------------------------------------------ */
/* a height field of snow lying on a bed                               */

typedef struct SnowLayer {
    float columnWidth;
    int opensAtEdges;
    v2 origin;

    float *heights;
    float *packed;
    float *bed;
    float *capacity;
    unsigned char *visible;
    unsigned char *supported;
    int count;

    float top;
    float bedDepth;
    float occluders[MAX_OCCLUDERS][4];
    int occluderCount;
    int isDock;

    double movedAt;
    float spillLeft, spillRight;

    v2 track;
    v2 trackVelocity;
    v2 offset;
    v2 offsetVelocity;
    float previousBaseY;

    float agitation;
    v2 swingDirection;
    float swingPeak;
    float previousSwingPeak;

    float depth;
    float phase0, phase1;
    float cornerRadius, cornerExponent;

    int rangeLo, rangeHi;
    int hasRange;
    int parity;
} SnowLayer;

static v2 layer_base(const SnowLayer *l) { return v2add(l->origin, l->offset); }
static float layer_center_x(const SnowLayer *l, int i)
{
    v2 b = layer_base(l);
    return b.x + ((float)i + 0.5f) * l->columnWidth;
}
static float layer_floor_y(const SnowLayer *l, int i) { return layer_base(l).y + l->bed[i]; }
static float layer_surface_y(const SnowLayer *l, int i)
{
    return layer_base(l).y + l->bed[i] + l->heights[i];
}
static int layer_index_at(const SnowLayer *l, float x)
{
    int i = (int)floorf((x - layer_base(l).x) / l->columnWidth);
    return (i >= 0 && i < l->count) ? i : -1;
}
static int layer_clamped_index(const SnowLayer *l, float x)
{
    int i = (int)floorf((x - layer_base(l).x) / l->columnWidth);
    if (i < 0) i = 0;
    if (i > l->count - 1) i = l->count - 1;
    return i;
}
static float layer_fill(const SnowLayer *l, int i)
{
    return fmaxf(0.0f, 1.0f - l->heights[i] / l->capacity[i]);
}

static void layer_update_capacity(SnowLayer *l)
{
    for (int i = 0; i < l->count; i++) {
        float x = (float)i * l->columnWidth;
        float drift = 0.12f * sinf(x * 0.0105f + l->phase0) + 0.07f * sinf(x * 0.037f + l->phase1);
        l->capacity[i] = l->depth * (0.95f + drift);
    }
}

static void layer_update_bed(SnowLayer *l)
{
    float radius = l->cornerRadius;
    float span = (float)l->count * l->columnWidth;
    for (int i = 0; i < l->count; i++) {
        if (radius <= 0) {
            l->bed[i] = 0;
            continue;
        }
        float x = ((float)i + 0.5f) * l->columnWidth;
        float inset = fminf(x, span - x);
        if (inset >= radius) {
            l->bed[i] = 0;
            continue;
        }
        float u = 1 - inset / radius;
        float e = l->cornerExponent;
        l->bed[i] = -radius * (1 - powf(fmaxf(0.0f, 1 - powf(u, e)), 1 / e));
    }
    float lowest = 0;
    for (int i = 0; i < l->count; i++)
        if (l->bed[i] < lowest) lowest = l->bed[i];
    l->bedDepth = -lowest;
}

static void layer_alloc_arrays(SnowLayer *l, int count)
{
    free(l->heights);
    free(l->packed);
    free(l->bed);
    free(l->capacity);
    free(l->visible);
    free(l->supported);
    l->count = count;
    l->heights = calloc((size_t)count, sizeof(float));
    l->packed = calloc((size_t)count, sizeof(float));
    l->bed = calloc((size_t)count, sizeof(float));
    l->capacity = calloc((size_t)count, sizeof(float));
    l->visible = malloc((size_t)count);
    l->supported = calloc((size_t)count, 1);
    memset(l->visible, 1, (size_t)count);
}

static void layer_init(SnowLayer *l, v2 origin, int columns, float columnWidth,
                       float depth, int opensAtEdges, float phase0, float phase1)
{
    memset(l, 0, sizeof(*l));
    l->origin = origin;
    l->track = origin;
    l->previousBaseY = origin.y;
    l->columnWidth = columnWidth;
    l->depth = depth;
    l->opensAtEdges = opensAtEdges;
    l->phase0 = phase0;
    l->phase1 = phase1;
    l->cornerRadius = 0;
    l->cornerExponent = 2;
    l->movedAt = -1;
    l->heights = NULL;
    l->packed = NULL;
    l->bed = NULL;
    l->capacity = NULL;
    l->visible = NULL;
    l->supported = NULL;
    layer_alloc_arrays(l, columns);
    layer_update_capacity(l);
}

static void layer_free(SnowLayer *l)
{
    free(l->heights);
    free(l->packed);
    free(l->bed);
    free(l->capacity);
    free(l->visible);
    free(l->supported);
    memset(l, 0, sizeof(*l));
}

static void layer_set(SnowLayer *l, int i, float height)
{
    l->heights[i] = fmaxf(0.0f, height);
    if (l->packed[i] > l->heights[i]) l->packed[i] = l->heights[i];
}

static void layer_replace(SnowLayer *l, float *fresh, int columns)
{
    layer_alloc_arrays(l, columns);
    memcpy(l->heights, fresh, sizeof(float) * (size_t)columns);
    layer_update_capacity(l);
    layer_update_bed(l);
}

static void layer_resize(SnowLayer *l, int columns)
{
    if (columns == l->count) return;
    float *fresh = calloc((size_t)columns, sizeof(float));
    int keep = l->count < columns ? l->count : columns;
    memcpy(fresh, l->heights, sizeof(float) * (size_t)keep);
    layer_alloc_arrays(l, columns);
    memcpy(l->heights, fresh, sizeof(float) * (size_t)columns);
    free(fresh);
    layer_update_capacity(l);
    layer_update_bed(l);
}

static void layer_refresh_top(SnowLayer *l)
{
    float top = 0;
    for (int i = 0; i < l->count; i++)
        if (l->heights[i] > top) top = l->heights[i];
    l->top = top;
}

static float layer_deposit(SnowLayer *l, float area, float x, int wide)
{
    if (!(area > 0)) return 0;
    int center = (int)floorf((x - layer_base(l).x) / l->columnWidth);
    float amount = area / l->columnWidth;
    static const int wideOff[5] = {-2, -1, 0, 1, 2};
    static const float wideW[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
    static const int narrowOff[3] = {-1, 0, 1};
    static const float narrowW[3] = {0.25f, 0.5f, 0.25f};
    float missed = 0;
    if (wide) {
        for (int k = 0; k < 5; k++) {
            int i = center + wideOff[k];
            if (i >= 0 && i < l->count) l->heights[i] += amount * wideW[k];
            else missed += area * wideW[k];
        }
    } else {
        for (int k = 0; k < 3; k++) {
            int i = center + narrowOff[k];
            if (i >= 0 && i < l->count) l->heights[i] += amount * narrowW[k];
            else missed += area * narrowW[k];
        }
    }
    return missed;
}

static void layer_hold(SnowLayer *l, int i)
{
    l->supported[i] = 1;
    if (l->hasRange) {
        if (i < l->rangeLo) l->rangeLo = i;
        if (i > l->rangeHi) l->rangeHi = i;
    } else {
        l->rangeLo = i;
        l->rangeHi = i;
        l->hasRange = 1;
    }
}

static void layer_release_support(SnowLayer *l)
{
    if (!l->hasRange) return;
    for (int i = l->rangeLo; i <= l->rangeHi; i++) l->supported[i] = 0;
    l->hasRange = 0;
}

/* The snow it takes to raise the surface to a line falling away from `level`
 * at `edge`; adds `scale` of it. */
static float layer_wedge(SnowLayer *l, float level, int edge, int inward, float slope, float scale)
{
    int reach = inward > 0 ? l->count - edge : edge + 1;
    float total = 0;
    for (int k = 0; k < reach; k++) {
        float line = level - slope * (float)k;
        if (!(line > -l->bedDepth)) break;
        int i = edge + inward * k;
        float gap = line - (l->bed[i] + l->heights[i]);
        if (!(gap > 0)) continue;
        total += gap;
        l->heights[i] += gap * scale;
        l->packed[i] += gap * scale;
    }
    return total;
}

static void layer_heap(SnowLayer *l, float area, int edge, int inward, float slope)
{
    float amount = area / l->columnWidth;
    if (!(amount > 0)) return;
    float low = l->bed[edge] + l->heights[edge];
    float high = low + amount;
    for (int k = 0; k < 20; k++) {
        float middle = (low + high) / 2;
        if (layer_wedge(l, middle, edge, inward, slope, 0) < amount) low = middle;
        else high = middle;
    }
    float total = layer_wedge(l, high, edge, inward, slope, 0);
    if (total > 0) layer_wedge(l, high, edge, inward, slope, amount / total);
}

static int layer_relax(SnowLayer *l, float repose, float rounding)
{
    int count = l->count;
    if (count <= 1 || !(l->top > 0)) return 0;
    float limit = repose * l->columnWidth;
    int forwardFirst = l->parity;
    l->parity = !l->parity;
    int moved = 0;
    float spilledLeft = 0, spilledRight = 0;
    int open = l->opensAtEdges;

    float *h = l->heights;
    float *p = l->packed;
    float *b = l->bed;
    unsigned char *held = l->supported;

#define SHIFT(amount, a, c)                                          \
    do {                                                             \
        float _amount = (amount);                                    \
        int _a = (a), _c = (c);                                      \
        float _share = h[_a] > 0 ? fminf(p[_a], _amount * p[_a] / h[_a]) : 0; \
        h[_a] -= _amount;                                            \
        h[_c] += _amount;                                            \
        p[_a] -= _share;                                             \
        p[_c] += _share;                                             \
    } while (0)

    for (int pass = 0; pass < 6; pass++) {
        int forward = ((pass % 2 == 0) != 0) == (forwardFirst != 0);
        for (int k = 0; k < count - 1; k++) {
            int i = forward ? k : count - 2 - k;
            if (held[i] || held[i + 1]) continue;
            float difference = (b[i] + h[i]) - (b[i + 1] + h[i + 1]);
            if (difference > limit + 0.01f) {
                float transfer = fminf((difference - limit) * 0.5f, h[i]);
                SHIFT(transfer, i, i + 1);
                if (transfer > 0) moved = 1;
            } else if (difference < -limit - 0.01f) {
                float transfer = fminf((-difference - limit) * 0.5f, h[i + 1]);
                SHIFT(transfer, i + 1, i);
                if (transfer > 0) moved = 1;
            }
        }
        if (open) {
            if (!held[0] && h[0] > limit + 0.01f) {
                float transfer = (h[0] - limit) * 0.5f;
                h[0] -= transfer;
                p[0] = fminf(p[0], h[0]);
                spilledLeft += transfer;
                moved = 1;
            }
            if (!held[count - 1] && h[count - 1] > limit + 0.01f) {
                float transfer = (h[count - 1] - limit) * 0.5f;
                h[count - 1] -= transfer;
                p[count - 1] = fminf(p[count - 1], h[count - 1]);
                spilledRight += transfer;
                moved = 1;
            }
        }
    }

    int upper = count - 2;
    if (upper < 1) upper = 1;
    for (int i = 1; i < upper; i++) {
        if (held[i - 1] || held[i] || held[i + 1] || held[i + 2]) continue;
        float s0 = b[i - 1] + h[i - 1], s1 = b[i] + h[i];
        float s2 = b[i + 1] + h[i + 1], s3 = b[i + 2] + h[i + 2];
        float kink = (s1 - s2) - 0.5f * ((s0 - s1) + (s2 - s3));
        if (fabsf(kink) <= 0.02f) continue;
        float transfer = kink > 0 ? fminf(kink * rounding, h[i]) : fmaxf(kink * rounding, -h[i + 1]);
        if (transfer > 0) SHIFT(transfer, i, i + 1);
        else SHIFT(-transfer, i + 1, i);
        if (fabsf(transfer) > 0.003f) moved = 1;
    }
#undef SHIFT

    l->spillLeft += spilledLeft * l->columnWidth;
    l->spillRight += spilledRight * l->columnWidth;
    return moved;
}

static int layer_thaw(SnowLayer *l, float dt, float downTo, float rate, float settle)
{
    if (!(l->top > 0)) return 0;
    float fastest = 0;
    for (int i = 0; i < l->count; i++) {
        if (!(l->heights[i] > 0)) continue;
        float h = l->heights[i];
        float loss = fminf(h, (rate + fmaxf(0.0f, h - downTo) / settle) * dt);
        l->heights[i] = h - loss;
        if (l->packed[i] > l->heights[i]) l->packed[i] = l->heights[i];
        if (loss > fastest) fastest = loss;
    }
    return fastest > 0.01f;
}

static void layer_write_smoothed(SnowLayer *l, float *out)
{
    int count = l->count;
    for (int i = 0; i < count; i++) {
        float left = l->heights[i > 0 ? i - 1 : 0];
        float right = l->heights[i < count - 1 ? i + 1 : count - 1];
        out[i] = l->bed[i] + 0.25f * left + 0.5f * l->heights[i] + 0.25f * right;
        out[count + i] = l->bed[i];
    }
}

static void layer_slide_column(SnowLayer *l, int direction, float *outHeight, float *outX, float *outY)
{
    layer_release_support(l);
    if (direction > 0) {
        int last = l->count - 1;
        *outX = layer_center_x(l, last);
        *outY = layer_floor_y(l, last);
        *outHeight = l->heights[last];
        for (int i = last; i > 0; i--) {
            l->heights[i] = l->heights[i - 1];
            l->packed[i] = l->packed[i - 1];
        }
        l->heights[0] = 0;
        l->packed[0] = 0;
        return;
    }
    *outX = layer_center_x(l, 0);
    *outY = layer_floor_y(l, 0);
    *outHeight = l->heights[0];
    for (int i = 0; i < l->count - 1; i++) {
        l->heights[i] = l->heights[i + 1];
        l->packed[i] = l->packed[i + 1];
    }
    l->heights[l->count - 1] = 0;
    l->packed[l->count - 1] = 0;
}

static void layer_update_visibility(SnowLayer *l, float screenWidth, const float (*blockers)[4], int blockerCount)
{
    for (int i = 0; i < l->count; i++) {
        float x = layer_center_x(l, i);
        float y = l->origin.y + l->bed[i] + 1;
        int vis = x >= 0 && x <= screenWidth;
        for (int k = 0; vis && k < blockerCount; k++) {
            if (x >= blockers[k][0] && x < blockers[k][2] && y >= blockers[k][1] && y < blockers[k][3]) vis = 0;
        }
        l->visible[i] = (unsigned char)vis;
    }
}

/* ------------------------------------------------------------------ */
/* id sets                                                             */

void idset_clear(IdSet *s)
{
    s->count = 0;
}

void idset_add(IdSet *s, uint64_t id)
{
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->ids = realloc(s->ids, sizeof(uint64_t) * (size_t)s->cap);
    }
    s->ids[s->count++] = id;
}

bool idset_has(const IdSet *s, uint64_t id)
{
    for (int i = 0; i < s->count; i++)
        if (s->ids[i] == id) return true;
    return false;
}

void idset_free(IdSet *s)
{
    free(s->ids);
    s->ids = NULL;
    s->count = s->cap = 0;
}

/* ------------------------------------------------------------------ */
/* the simulation                                                      */

typedef struct {
    uint64_t id;
    SnowLayer *layer;
    double since;
} Parked;

struct SnowSim {
    float width, height;
    Snowfall snowfall;
    float density;
    int thaws;
    int accumulates;
    int wallLeft, wallRight;
    int snowfallHeld;

    SnowLayer *ground;
    SnowLayer **layers;
    int layerCount, layerCap;

    /* ledges keyed by window id */
    struct {
        uint64_t id;
        SnowLayer *layer;
    } *ledges;
    int ledgeCount, ledgeCap;

    Parked *parking;
    int parkingCount, parkingCap;

    struct {
        uint64_t id;
        SnowLayer *layer;
    } *dormant;
    int dormantCount, dormantCap;

    SnowLayer **held;
    int heldCount, heldCap;

    Flake *flakes;
    int flakeCount, flakeCap;

    Grain *grains;
    int grainCount, grainCap;

    Mote *motes;
    int moteCount, moteCap;

    int targetFlakes;
    RandomSource rnd;
    double clock_;
    v2 cursor;
    int hasCursor;
    v2 cursorVelocity;
    int heightsChanged;
    int windowsChanged;
    double lastWindowMotion;
    double lastCursorPush;
    float strength;
    float strengthTarget;
    double nextShift;
    double scatteredUntil;
    struct {
        int active;
        double start;
        float direction;
        int calm;
    } gust;
    Pace pace;
};

static int sim_column_count_for(float width)
{
    return (int)ceilf(width / COLUMN_WIDTH) + 1;
}

static float sim_breeze(const SnowSim *sim)
{
    float t = (float)sim->clock_;
    float breeze = 10.0f * sinf(t * 0.07f) + 6.0f * sinf(t * 0.19f + 1.7f) + 3.0f * sinf(t * 0.43f + 4.1f);
    return breeze * (0.6f + 0.4f * sim->strength);
}

static float sim_gust_speed(const SnowSim *sim)
{
    if (!sim->gust.active || sim->gust.calm) return 0;
    float age = (float)(sim->clock_ - sim->gust.start);
    return sim->gust.direction * GUST_WIND * smoothstepf(0, 0.12f, age) *
           (1 - smoothstepf(0.9f, GUST_LENGTH, age));
}

static float sim_wind_speed(const SnowSim *sim)
{
    return sim_breeze(sim) + sim_gust_speed(sim);
}

static int sim_blowing(const SnowSim *sim) { return sim->gust.active && !sim->gust.calm; }

static float snowfall_per_megapoint(Snowfall s)
{
    switch (s) {
    case SNOWFALL_OFF: return 0;
    case SNOWFALL_LIGHT: return 150;
    case SNOWFALL_HEAVY: return 800;
    case SNOWFALL_VARIABLE: return 380;
    case SNOWFALL_STEADY:
    default: return 380;
    }
}

static void sim_update_target(SnowSim *sim)
{
    if (sim->snowfallHeld) {
        sim->targetFlakes = 0;
        return;
    }
    float area = sim->width * sim->height / 1000000.0f;
    sim->targetFlakes = (int)(snowfall_per_megapoint(sim->snowfall) * sim->strength * sim->density * area);
}

/* ---- particle pools ---- */

static void sim_reserve_flakes(SnowSim *sim, int n)
{
    if (n <= sim->flakeCap) return;
    sim->flakeCap = n * 3 / 2 + 64;
    sim->flakes = realloc(sim->flakes, sizeof(Flake) * (size_t)sim->flakeCap);
}

static void sim_reserve_grains(SnowSim *sim, int n)
{
    if (n <= sim->grainCap) return;
    sim->grainCap = n * 3 / 2 + 64;
    sim->grains = realloc(sim->grains, sizeof(Grain) * (size_t)sim->grainCap);
}

static void sim_reserve_motes(SnowSim *sim, int n)
{
    if (n <= sim->moteCap) return;
    sim->moteCap = n * 3 / 2 + 64;
    sim->motes = realloc(sim->motes, sizeof(Mote) * (size_t)sim->moteCap);
}

static void sim_ledges_add(SnowSim *sim, uint64_t id, SnowLayer *layer)
{
    if (sim->ledgeCount == sim->ledgeCap) {
        sim->ledgeCap = sim->ledgeCap ? sim->ledgeCap * 2 : 16;
        sim->ledges = realloc(sim->ledges, sizeof(*sim->ledges) * (size_t)sim->ledgeCap);
    }
    sim->ledges[sim->ledgeCount].id = id;
    sim->ledges[sim->ledgeCount].layer = layer;
    sim->ledgeCount++;
}

static SnowLayer *sim_ledges_find(SnowSim *sim, uint64_t id)
{
    for (int i = 0; i < sim->ledgeCount; i++)
        if (sim->ledges[i].id == id) return sim->ledges[i].layer;
    return NULL;
}

static void sim_ledges_remove(SnowSim *sim, uint64_t id)
{
    for (int i = 0; i < sim->ledgeCount; i++) {
        if (sim->ledges[i].id == id) {
            sim->ledges[i] = sim->ledges[sim->ledgeCount - 1];
            sim->ledgeCount--;
            return;
        }
    }
}

static void sim_dormant_put(SnowSim *sim, uint64_t id, SnowLayer *layer)
{
    if (sim->dormantCount == sim->dormantCap) {
        sim->dormantCap = sim->dormantCap ? sim->dormantCap * 2 : 16;
        sim->dormant = realloc(sim->dormant, sizeof(*sim->dormant) * (size_t)sim->dormantCap);
    }
    sim->dormant[sim->dormantCount].id = id;
    sim->dormant[sim->dormantCount].layer = layer;
    sim->dormantCount++;
    if (sim->dormantCount > 200) {
        layer_free(sim->dormant[0].layer);
        free(sim->dormant[0].layer);
        memmove(&sim->dormant[0], &sim->dormant[1], sizeof(*sim->dormant) * (size_t)(sim->dormantCount - 1));
        sim->dormantCount--;
    }
}

static SnowLayer *sim_dormant_take(SnowSim *sim, uint64_t id)
{
    for (int i = 0; i < sim->dormantCount; i++) {
        if (sim->dormant[i].id == id) {
            SnowLayer *l = sim->dormant[i].layer;
            sim->dormant[i] = sim->dormant[sim->dormantCount - 1];
            sim->dormantCount--;
            return l;
        }
    }
    return NULL;
}

static void sim_parking_add(SnowSim *sim, uint64_t id, SnowLayer *layer)
{
    if (sim->parkingCount == sim->parkingCap) {
        sim->parkingCap = sim->parkingCap ? sim->parkingCap * 2 : 16;
        sim->parking = realloc(sim->parking, sizeof(Parked) * (size_t)sim->parkingCap);
    }
    sim->parking[sim->parkingCount].id = id;
    sim->parking[sim->parkingCount].layer = layer;
    sim->parking[sim->parkingCount].since = sim->clock_;
    sim->parkingCount++;
}

static SnowLayer *sim_parking_take(SnowSim *sim, uint64_t id)
{
    for (int i = 0; i < sim->parkingCount; i++) {
        if (sim->parking[i].id == id) {
            SnowLayer *l = sim->parking[i].layer;
            memmove(&sim->parking[i], &sim->parking[i + 1], sizeof(Parked) * (size_t)(sim->parkingCount - i - 1));
            sim->parkingCount--;
            return l;
        }
    }
    return NULL;
}

static void sim_hold_layer(SnowSim *sim, SnowLayer *l)
{
    for (int i = 0; i < sim->heldCount; i++)
        if (sim->held[i] == l) return;
    if (sim->heldCount == sim->heldCap) {
        sim->heldCap = sim->heldCap ? sim->heldCap * 2 : 8;
        sim->held = realloc(sim->held, sizeof(SnowLayer *) * (size_t)sim->heldCap);
    }
    sim->held[sim->heldCount++] = l;
}

static void sim_rebuild_layers(SnowSim *sim)
{
    int need = 1 + sim->ledgeCount;
    if (need > sim->layerCap) {
        sim->layerCap = need * 2 + 8;
        sim->layers = realloc(sim->layers, sizeof(SnowLayer *) * (size_t)sim->layerCap);
    }
    sim->layers[0] = sim->ground;
    for (int i = 0; i < sim->ledgeCount; i++) sim->layers[i + 1] = sim->ledges[i].layer;
    sim->layerCount = need;
}

/* ---- construction ---- */

SnowSim *sim_create(float width, float height, Snowfall snowfall)
{
    SnowSim *sim = calloc(1, sizeof(SnowSim));
    sim->width = width;
    sim->height = height;
    sim->snowfall = snowfall;
    sim->density = 1;
    sim->strength = 1;
    sim->strengthTarget = 1;
    sim->accumulates = 1;
    sim->thaws = 1;
    sim->clock_ = 0;
    sim->lastWindowMotion = -10;
    sim->lastCursorPush = -10;
    sim->scatteredUntil = -10;
    sim->nextShift = 0;
    sim->pace = PACE_CALM;
    sim->rnd.state = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ 0x9E3779B97F4A7C15ull;
    if (sim->rnd.state == 0) sim->rnd.state = 0x1234567890ABCDEFull;

    sim->ground = calloc(1, sizeof(SnowLayer));
    layer_init(sim->ground, v2make(0, 0), sim_column_count_for(width), COLUMN_WIDTH,
               GROUND_DEPTH, 0, rnd_range(&sim->rnd, 0, 6.2831853f), rnd_range(&sim->rnd, 0, 6.2831853f));
    sim_rebuild_layers(sim);
    sim_update_target(sim);
    return sim;
}

void sim_destroy(SnowSim *sim)
{
    if (!sim) return;
    for (int i = 0; i < sim->ledgeCount; i++) {
        layer_free(sim->ledges[i].layer);
        free(sim->ledges[i].layer);
    }
    for (int i = 0; i < sim->dormantCount; i++) {
        layer_free(sim->dormant[i].layer);
        free(sim->dormant[i].layer);
    }
    for (int i = 0; i < sim->parkingCount; i++) {
        layer_free(sim->parking[i].layer);
        free(sim->parking[i].layer);
    }
    layer_free(sim->ground);
    free(sim->ground);
    free(sim->layers);
    free(sim->ledges);
    free(sim->dormant);
    free(sim->parking);
    free(sim->held);
    free(sim->flakes);
    free(sim->grains);
    free(sim->motes);
    free(sim);
}

void sim_resize(SnowSim *sim, float width, float height)
{
    if (width == sim->width && height == sim->height) return;
    layer_resize(sim->ground, sim_column_count_for(width));
    sim->width = width;
    sim->height = height;
    sim_update_target(sim);
}

void sim_set_snowfall(SnowSim *sim, Snowfall snowfall)
{
    if (snowfall == SNOWFALL_VARIABLE && sim->snowfall != SNOWFALL_VARIABLE) sim->nextShift = sim->clock_;
    if (snowfall != SNOWFALL_VARIABLE) {
        sim->strength = 1;
        sim->strengthTarget = 1;
    }
    sim->snowfall = snowfall;
    sim_update_target(sim);
}

void sim_set_density(SnowSim *sim, float density)
{
    sim->density = density;
    sim_update_target(sim);
}

void sim_set_thaws(SnowSim *sim, bool thaws) { sim->thaws = thaws; }
void sim_set_accumulates(SnowSim *sim, bool accumulates) { sim->accumulates = accumulates; }

void sim_set_walls(SnowSim *sim, bool left, bool right)
{
    sim->wallLeft = left;
    sim->wallRight = right;
}

Pace sim_pace(const SnowSim *sim) { return sim->pace; }

bool sim_has_snow(const SnowSim *sim)
{
    for (int i = 0; i < sim->layerCount; i++)
        if (sim->layers[i]->top > 0.5f) return true;
    return false;
}

float sim_breeze_direction(const SnowSim *sim) { return sim_breeze(sim) < 0 ? -1 : 1; }

int sim_sprite_count(const SnowSim *sim)
{
    return sim->flakeCount + sim->grainCount + sim->moteCount;
}

int sim_column_count(const SnowSim *sim)
{
    int total = 0;
    for (int i = 0; i < sim->layerCount; i++) total += 2 * sim->layers[i]->count;
    return total;
}

/* ---- spitting snow back out ---- */

static void sim_release(SnowSim *sim, float area, v2 position, v2 velocity, v2 spread, float scatter);
static void sim_throw_lump(SnowSim *sim, float area, v2 position, v2 velocity);
static void sim_shed(SnowSim *sim, SnowLayer *ledge, float share, v2 velocity);
static void sim_burst(SnowSim *sim, Grain *lump, SnowLayer *layer);
static void sim_scrape(SnowSim *sim, SnowLayer *ledge, int from, int to, int edge, float inward);
static void sim_drop_past(SnowSim *sim, SnowLayer *layer, float area, float x, float velocity);
static v2 sim_edge_point(SnowLayer *layer, int left);
static void sim_carve(SnowSim *sim, SnowLayer *layer, v2 center, v2 velocity, float travel, int final);

static void sim_release(SnowSim *sim, float area, v2 position, v2 velocity, v2 spread, float scatter)
{
    if (!(area > 0.02f)) return;
    int room = MAX_GRAINS - sim->grainCount;
    if (room <= 0) {
        layer_deposit(sim->ground, area, position.x, 1);
        return;
    }
    int count = (int)(area / 3);
    if (count < 1) count = 1;
    if (count > 6) count = 6;
    if (count > room) count = room;
    float share = area / (float)count;
    sim_reserve_grains(sim, sim->grainCount + count);
    for (int k = 0; k < count; k++) {
        Grain *g = &sim->grains[sim->grainCount++];
        g->position = v2add(position, v2make(rnd_range(&sim->rnd, -spread.x, spread.x),
                                             rnd_range(&sim->rnd, 0, spread.y)));
        float jitter = rnd_range(&sim->rnd, -scatter, scatter);
        v2 v = v2mul(velocity, 1 + jitter);
        v.x += rnd_range(&sim->rnd, -12, 12) - 40 * scatter * rnd_range(&sim->rnd, -1, 1);
        v.y += rnd_range(&sim->rnd, -10, 8);
        g->velocity = v;
        g->area = share;
        float s = sqrtf(share) * 1.05f;
        g->size = fminf(5, fmaxf(1.1f, s));
        g->lump = 0;
        g->blown = 0;
    }
}

static void sim_throw_lump(SnowSim *sim, float area, v2 position, v2 velocity)
{
    int pieces = (int)(area / 90) + 1;
    for (int k = 0; k < pieces && sim->grainCount < MAX_GRAINS; k++) {
        float share = area / (float)pieces;
        sim_reserve_grains(sim, sim->grainCount + 1);
        Grain *g = &sim->grains[sim->grainCount++];
        g->position = v2add(position, v2make(rnd_range(&sim->rnd, -2, 2), rnd_range(&sim->rnd, -1, 1)));
        v2 v = v2mul(velocity, 1 + rnd_range(&sim->rnd, -0.2f, 0.2f));
        v.x += rnd_range(&sim->rnd, -25, 25);
        v.y += rnd_range(&sim->rnd, -80, 0);
        g->velocity = v;
        g->area = share;
        g->size = fminf(11, sqrtf(share) * 1.05f);
        g->lump = rnd_range(&sim->rnd, 0.05f, 1);
        g->blown = 0;
    }
}

static void sim_burst(SnowSim *sim, Grain *lump, SnowLayer *layer)
{
    float impact = fmaxf(0, -lump->velocity.y);
    float force = fminf(fmaxf(impact / 900.0f, 0.35f), 1.3f);
    float crumbs = lump->area * 0.18f;
    static const float spots[3] = {-0.3f, 0.0f, 0.3f};
    for (int k = 0; k < 3; k++) {
        float x = lump->position.x + spots[k] * lump->size;
        float missed = layer_deposit(layer, (lump->area - crumbs) / 3.0f, x, 1);
        sim_drop_past(sim, layer, missed, x, lump->velocity.x * 0.3f);
    }
    int idx = layer_index_at(layer, lump->position.x);
    float surface = idx >= 0 ? layer_surface_y(layer, idx) : lump->position.y;
    sim_release(sim, crumbs, v2make(lump->position.x, surface + 1),
                v2make(lump->velocity.x * 0.3f, impact * rnd_range(&sim->rnd, 0.08f, 0.16f)),
                v2make(lump->size * 0.4f, 1), 0.6f);

    int count = (int)(2 + lump->size * 0.6f * force);
    if (count > MAX_MOTES - sim->moteCount) count = MAX_MOTES - sim->moteCount;
    if (count <= 0) return;
    sim_reserve_motes(sim, sim->moteCount + count);
    for (int k = 0; k < count; k++) {
        float angle = rnd_range(&sim->rnd, 0.15f, (float)M_PI - 0.15f);
        float speed = rnd_range(&sim->rnd, 25, 110) * force;
        Mote *m = &sim->motes[sim->moteCount++];
        m->position = v2make(lump->position.x + rnd_range(&sim->rnd, -0.5f, 0.5f) * lump->size,
                             surface + rnd_range(&sim->rnd, 0, lump->size * 0.4f));
        m->velocity = v2make(cosf(angle) * speed * 1.4f + lump->velocity.x * 0.2f,
                             sinf(angle) * speed * 0.6f);
        m->size = rnd_range(&sim->rnd, 0.3f, 0.6f) * lump->size + 1;
        m->alpha = rnd_range(&sim->rnd, 0.1f, 0.22f);
        m->age = 0;
        m->life = rnd_range(&sim->rnd, 0.45f, 0.9f);
    }
}

static v2 sim_edge_point(SnowLayer *layer, int left)
{
    int end = left ? 0 : layer->count - 1;
    v2 base = layer_base(layer);
    float x = left ? base.x - 0.75f : base.x + (float)layer->count * layer->columnWidth + 0.75f;
    return v2make(x, layer_surface_y(layer, end));
}

static void sim_drop_past(SnowSim *sim, SnowLayer *layer, float area, float x, float velocity)
{
    if (!layer->opensAtEdges || !(area > 0.05f)) return;
    v2 base = layer_base(layer);
    int leftSide = x < base.x + (float)layer->count * layer->columnWidth / 2;
    sim_release(sim, area, sim_edge_point(layer, leftSide), v2make(velocity, -10), v2make(0.25f, 1), 0);
}

static void sim_shed(SnowSim *sim, SnowLayer *ledge, float share, v2 velocity)
{
    int start = 0;
    while (start < ledge->count) {
        int chunk = (int)rnd_range(&sim->rnd, 2, 7);
        int end = start + chunk;
        if (end > ledge->count) end = ledge->count;
        float area = 0;
        float lowest = 3.4e38f;
        float highest = -3.4e38f;
        for (int i = start; i < end; i++) {
            if (!(ledge->heights[i] > 0.05f)) continue;
            float h = ledge->heights[i];
            area += h * share * ledge->columnWidth;
            float f = layer_floor_y(ledge, i) + h * (1 - share);
            float t = layer_floor_y(ledge, i) + h;
            if (f < lowest) lowest = f;
            if (t > highest) highest = t;
        }
        float cohesion = area > 14 ? rnd_range(&sim->rnd, 0.4f, 0.7f) : 0;
        for (int i = start; i < end; i++) {
            if (!(ledge->heights[i] > 0.05f)) continue;
            float h = ledge->heights[i];
            float amount = h * share;
            layer_set(ledge, i, h - amount);
            sim_release(sim, amount * ledge->columnWidth * (1 - cohesion),
                        v2make(layer_center_x(ledge, i), layer_floor_y(ledge, i) + h - amount),
                        velocity, v2make(ledge->columnWidth / 2, amount), 0.25f);
        }
        if (cohesion > 0) {
            float left = layer_center_x(ledge, start);
            float right = layer_center_x(ledge, end - 1);
            sim_throw_lump(sim, area * cohesion, v2make((left + right) / 2, (lowest + highest) / 2), velocity);
        }
        start = end;
    }
    sim->heightsChanged = 1;
}

static void sim_scrape(SnowSim *sim, SnowLayer *ledge, int from, int to, int edge, float inward)
{
    float area = 0;
    for (int i = from; i < to; i++) {
        float packed = fminf(ledge->packed[i], ledge->heights[i]);
        area += (packed + (ledge->heights[i] - packed) * PACKING) * ledge->columnWidth;
        layer_set(ledge, i, 0);
        layer_hold(ledge, i);
    }
    if (!(area > 0.01f)) return;
    sim->heightsChanged = 1;
    float speed = fmaxf(0, -inward * (ledge->trackVelocity.x + ledge->offsetVelocity.x));
    float crumbled = area * 0.4f * smoothstepf(200, 1200, speed);
    if (crumbled > 0.5f) {
        sim_release(sim, crumbled,
                    v2make(layer_center_x(ledge, edge), layer_surface_y(ledge, edge)),
                    v2make(inward * speed * 0.25f, 60 + speed * 0.12f),
                    v2make(ledge->columnWidth, 2), 0.6f);
    }
    layer_heap(ledge, area - crumbled, edge, (int)inward, REPOSE * ledge->columnWidth);
}

static void sim_confine(SnowSim *sim, SnowLayer *ledge)
{
    int count = ledge->count;
    v2 base = layer_base(ledge);
    int first = 0, last = count - 1;
    if (sim->wallLeft) {
        float v = ceilf(-base.x / ledge->columnWidth - 0.5f);
        int n = (int)v;
        if (n < 0) n = 0;
        if (n > count) n = count;
        first = n;
    }
    if (sim->wallRight) {
        float v = floorf((sim->width - base.x) / ledge->columnWidth - 0.5f);
        int n = (int)v;
        if (n > count - 1) n = count - 1;
        if (n < -1) n = -1;
        last = n;
    }
    if (!(first > 0 || last < count - 1)) return;
    if (first > last) {
        float x = first > 0 ? 1.0f : sim->width - 1.0f;
        for (int i = 0; i < count; i++) {
            if (!(ledge->heights[i] > 0.05f)) continue;
            float h = ledge->heights[i];
            layer_set(ledge, i, 0);
            sim_release(sim, h * ledge->columnWidth, v2make(x, layer_floor_y(ledge, i)), v2make(0, 0),
                        v2make(1, h), 0);
        }
        sim->heightsChanged = 1;
        return;
    }
    sim_hold_layer(sim, ledge);
    if (first > 0) sim_scrape(sim, ledge, 0, first, first, 1);
    if (last < count - 1) sim_scrape(sim, ledge, last + 1, count, last, -1);
}

/* ---- windows ---- */

static void sim_restore(SnowLayer *ledge, float left, float top, int columns)
{
    layer_resize(ledge, columns);
    ledge->origin = v2make(left, top);
    ledge->track = ledge->origin;
    ledge->trackVelocity = v2make(0, 0);
    ledge->offset = v2make(0, 0);
    ledge->offsetVelocity = v2make(0, 0);
    ledge->previousBaseY = top;
    ledge->movedAt = -1;
}

static void sim_expire_parking(SnowSim *sim)
{
    while (sim->parkingCount > 0 && sim->clock_ - sim->parking[0].since > 0.25) {
        SnowLayer *layer = sim->parking[0].layer;
        memmove(&sim->parking[0], &sim->parking[1], sizeof(Parked) * (size_t)(sim->parkingCount - 1));
        sim->parkingCount--;
        sim_shed(sim, layer, 1, v2make(0, 0));
        layer_free(layer);
        free(layer);
    }
}

static void sim_move(SnowSim *sim, SnowLayer *ledge, float left, float top, int columns, float interval)
{
    float oldLeft = ledge->origin.x;
    float newRight = left + (float)columns * ledge->columnWidth;
    float oldRight = oldLeft + (float)ledge->count * ledge->columnWidth;
    float shift = fabsf(left - oldLeft) <= fabsf(newRight - oldRight) ? left - oldLeft : newRight - oldRight;
    v2 displacement = v2make(shift, top - ledge->origin.y);
    int resized = columns != ledge->count || fabsf(left - oldLeft - shift) > 0.5f;
    if (displacement.x == 0 && displacement.y == 0 && !resized) return;

    float elapsed = fmaxf(interval, 1.0f / 120);
    int wasResting = sim->clock_ - ledge->movedAt > 0.3 && interval > 0.05f;
    ledge->movedAt = sim->clock_;
    if (ledge->top > 0.05f) {
        sim->lastWindowMotion = sim->clock_;
        sim->windowsChanged = 1;
    }
    if (v2len(displacement) / elapsed > JUMP_SPEED) {
        sim_shed(sim, ledge, 1, v2make(0, 0));
        ledge->offset = v2make(0, 0);
        ledge->offsetVelocity = v2make(0, 0);
        ledge->trackVelocity = v2make(0, 0);
    }
    if (resized) {
        int oldCount = ledge->count;
        float *fresh = calloc((size_t)columns, sizeof(float));
        for (int i = 0; i < oldCount; i++) {
            if (!(ledge->heights[i] > 0)) continue;
            float cx = layer_center_x(ledge, i);
            int j = (int)floorf((cx + shift - left - ledge->offset.x) / ledge->columnWidth);
            if (j >= 0 && j < columns) {
                fresh[j] += ledge->heights[i];
            } else {
                float h = ledge->heights[i];
                sim_release(sim, h * ledge->columnWidth, v2make(cx, layer_floor_y(ledge, i)),
                            ledge->trackVelocity, v2make(ledge->columnWidth / 2, h), 0);
            }
        }
        layer_replace(ledge, fresh, columns);
        free(fresh);
    }
    ledge->origin = v2make(left, top);
    if (v2len(displacement) / elapsed > JUMP_SPEED) {
        ledge->track = ledge->origin;
    } else if (wasResting) {
        ledge->trackVelocity = v2mul(displacement, 1.0f / elapsed);
        ledge->track = v2sub(ledge->origin, v2mul(ledge->trackVelocity, TRACK_TIME));
    }
}

/* Critically damped follow of `target`, stable at any time step. */
static void smooth_damp(v2 *current, v2 *velocity, v2 target, float time, float dt)
{
    float omega = 2 / time;
    float x = omega * dt;
    float decay = 1 / (1 + x + 0.48f * x * x + 0.235f * x * x * x);
    v2 change = v2sub(*current, target);
    v2 temp = v2mul(v2add(*velocity, v2mul(change, omega)), dt);
    *velocity = v2mul(v2sub(*velocity, v2mul(temp, omega)), decay);
    *current = v2add(target, v2mul(v2add(change, temp), decay));
}

static void sim_carry(SnowSim *sim, SnowLayer *ledge, float dt)
{
    v2 previousVelocity = ledge->trackVelocity;
    smooth_damp(&ledge->track, &ledge->trackVelocity, ledge->origin, TRACK_TIME, dt);
    v2 acceleration = v2mul(v2sub(ledge->trackVelocity, previousVelocity), 1.0f / dt);

    ledge->agitation *= expf(-dt / 0.3f);
    float magnitude = v2len(acceleration);
    if (magnitude > 1 && v2dot(acceleration, ledge->swingDirection) < 0) {
        float a = fminf(ledge->previousSwingPeak, ledge->swingPeak) / GRAVITY;
        if (a > ledge->agitation) ledge->agitation = a;
        ledge->previousSwingPeak = ledge->swingPeak;
        ledge->swingPeak = magnitude;
        ledge->swingDirection = v2mul(acceleration, 1 / magnitude);
    } else {
        if (magnitude > ledge->swingPeak) ledge->swingPeak = magnitude;
        if (magnitude > 1 && ledge->swingDirection.x == 0 && ledge->swingDirection.y == 0)
            ledge->swingDirection = v2mul(acceleration, 1 / magnitude);
    }

    if (!(ledge->top > 0.05f)) {
        ledge->offset = v2make(0, 0);
        ledge->offsetVelocity = v2make(0, 0);
        return;
    }

    float lift = ledge->offset.y;
    float liftVelocity = ledge->offsetVelocity.y;
    float fall = -GRAVITY - acceleration.y;
    if (lift > 0 || fall > ADHESION) {
        liftVelocity += fall * dt;
        lift += liftVelocity * dt;
        if (lift <= 0) {
            lift = 0;
            liftVelocity = 0;
        }
    }

    float pressure = lift > 0 ? 0 : fmaxf(0, GRAVITY + ADHESION + acceleration.y);
    float drive = -acceleration.x;
    float slideVelocity = ledge->offsetVelocity.x;
    if (!(slideVelocity == 0 && fabsf(drive) <= GRIP * pressure)) {
        float direction = slideVelocity != 0 ? (slideVelocity > 0 ? 1.0f : -1.0f) : (drive > 0 ? 1.0f : -1.0f);
        float next = slideVelocity + (drive - direction * SLIP * pressure) * dt;
        slideVelocity = pressure > 0 && next * direction <= 0 ? 0 : next;
    }

    float slide = ledge->offset.x + slideVelocity * dt;
    v2 carried = v2add(ledge->trackVelocity, v2make(slideVelocity, liftVelocity));
    while (fabsf(slide) >= ledge->columnWidth) {
        int direction = slide > 0 ? 1 : -1;
        float droppedHeight, dx, dy;
        layer_slide_column(ledge, direction, &droppedHeight, &dx, &dy);
        slide -= (float)direction * ledge->columnWidth;
        if (droppedHeight > 0.01f) {
            float x = dx + (float)direction * ledge->columnWidth;
            sim_release(sim, droppedHeight * ledge->columnWidth, v2make(x, dy), carried,
                        v2make(ledge->columnWidth / 2, droppedHeight), 0);
        }
    }

    int moved = lift != ledge->offset.y || slide != ledge->offset.x;
    ledge->offset = v2make(slide, lift);
    ledge->offsetVelocity = v2make(slideVelocity, liftVelocity);
    if (moved) sim->heightsChanged = 1;

    if (lift > BREAK_LIFT) {
        sim_shed(sim, ledge, 1, carried);
        ledge->offset = v2make(0, 0);
        ledge->offsetVelocity = v2make(0, 0);
    } else if (ledge->agitation > SHAKING) {
        float rate = fminf(0.8f, 0.06f * (ledge->agitation - SHAKING));
        int picks = (int)((float)ledge->count * rate * dt * 2 + rnd_next(&sim->rnd));
        for (int k = 0; k < picks; k++) {
            int i = (int)(rnd_next(&sim->rnd) * (float)ledge->count);
            if (i < 0) i = 0;
            if (i > ledge->count - 1) i = ledge->count - 1;
            float h = ledge->heights[i];
            if (!(h > 0.2f)) continue;
            layer_set(ledge, i, h * 0.5f);
            v2 jitter = v2make(rnd_range(&sim->rnd, -80, 80), rnd_range(&sim->rnd, 50, 200));
            sim_release(sim, h * 0.5f * ledge->columnWidth,
                        v2make(layer_center_x(ledge, i), layer_floor_y(ledge, i) + h * 0.5f),
                        v2add(carried, jitter), v2make(ledge->columnWidth / 2, h * 0.5f), 0.25f);
        }
        if (picks > 0) sim->heightsChanged = 1;
    }
}

void sim_update_windows(SnowSim *sim, const WinSurface *surfaces, int count,
                        float interval, const IdSet *parked, const IdSet *elsewhere,
                        bool snowOnWindows)
{
    if (!snowOnWindows) count = 0;
    uint64_t *seen = NULL;
    int seenCount = 0;
    if (count > 0) seen = malloc(sizeof(uint64_t) * (size_t)count);

    for (int s = 0; s < count; s++) {
        const WinSurface *surface = &surfaces[s];
        float left = surface->x;
        /* Surfaces arrive with `y` at the bottom edge in bottom-up coordinates;
         * snow sits on the top edge, which is a window height above that. */
        float top = surface->y + surface->h;
        int columns = (int)(surface->w / COLUMN_WIDTH);
        if (columns < 8) continue;
        seen[seenCount++] = surface->id;

        SnowLayer *ledge = sim_ledges_find(sim, surface->id);
        if (ledge) {
            sim_move(sim, ledge, left, top, columns, interval);
        } else {
            SnowLayer *returning = sim_dormant_take(sim, surface->id);
            if (!returning) returning = sim_parking_take(sim, surface->id);
            if (returning) {
                ledge = returning;
                sim_restore(ledge, left, top, columns);
                sim_ledges_add(sim, surface->id, ledge);
            } else {
                ledge = calloc(1, sizeof(SnowLayer));
                layer_init(ledge, v2make(left, top), columns, COLUMN_WIDTH, LEDGE_DEPTH, 1,
                           rnd_range(&sim->rnd, 0, 6.2831853f), rnd_range(&sim->rnd, 0, 6.2831853f));
                sim_ledges_add(sim, surface->id, ledge);
            }
        }
        if (surface->cornerRadius != ledge->cornerRadius || surface->cornerExponent != ledge->cornerExponent) {
            ledge->cornerRadius = surface->cornerRadius;
            ledge->cornerExponent = surface->cornerExponent;
            layer_update_bed(ledge);
        }
        ledge->occluderCount = 0;
        for (int k = 0; k < surface->cornerCount && k < MAX_OCCLUDERS; k++) {
            for (int c = 0; c < 4; c++) ledge->occluders[ledge->occluderCount][c] = surface->corners[k][c];
            ledge->occluderCount++;
        }
        ledge->isDock = surface->isDock;
        layer_update_visibility(ledge, sim->width, surface->corners, surface->cornerCount);
    }

    for (int i = sim->ledgeCount - 1; i >= 0; i--) {
        uint64_t id = sim->ledges[i].id;
        int found = 0;
        for (int k = 0; k < seenCount; k++)
            if (seen[k] == id) { found = 1; break; }
        if (found) continue;
        SnowLayer *ledge = sim->ledges[i].layer;
        sim_ledges_remove(sim, id);
        if (elsewhere && idset_has(elsewhere, id)) {
            sim_dormant_put(sim, id, ledge);
        } else if (parked && idset_has(parked, id)) {
            sim_parking_add(sim, id, ledge);
        } else {
            sim_shed(sim, ledge, 1, v2add(ledge->trackVelocity, ledge->offsetVelocity));
            layer_free(ledge);
            free(ledge);
        }
    }
    free(seen);
    sim_rebuild_layers(sim);
}

/* ---- cursor ---- */

static float sim_emit(SnowSim *sim, float area, v2 origin, v2 center, v2 velocity)
{
    if (!(area > 0.6f) || sim->grainCount >= MAX_GRAINS) return 0;
    int count = (int)(area / 1.6f) + 1;
    if (count > 4) count = 4;
    float share = area / (float)count;
    float speed = v2len(velocity);
    v2 offset = v2sub(origin, center);
    float length = v2len(offset);
    v2 outward = length > 0.001f ? v2mul(offset, 1 / length) : v2make(0, 1);
    sim_reserve_grains(sim, sim->grainCount + count);
    for (int k = 0; k < count; k++) {
        v2 v = v2mul(velocity, rnd_range(&sim->rnd, 0.55f, 1.25f));
        v = v2add(v, v2mul(outward, speed * rnd_range(&sim->rnd, 0.05f, 0.25f)));
        v.y += speed * rnd_range(&sim->rnd, 0.02f, 0.2f);
        v.x += rnd_range(&sim->rnd, -30, 30);
        v.y += rnd_range(&sim->rnd, -20, 40);
        Grain *g = &sim->grains[sim->grainCount++];
        g->position = v2add(origin, v2make(rnd_range(&sim->rnd, -1, 1), rnd_range(&sim->rnd, 0, 2)));
        g->velocity = v;
        g->area = share;
        float s = sqrtf(share) * 1.05f;
        g->size = fminf(4.2f, fmaxf(1.1f, s));
        g->lump = 0;
        g->blown = 0;
    }
    return area;
}

static void sim_carve(SnowSim *sim, SnowLayer *layer, v2 center, v2 velocity, float travel, int final)
{
    float radius = CURSOR_RADIUS;
    v2 base = layer_base(layer);
    int lower = (int)floorf((center.x - radius - base.x) / layer->columnWidth);
    int upper = (int)floorf((center.x + radius - base.x) / layer->columnWidth);
    if (lower < 0) lower = 0;
    if (upper > layer->count - 1) upper = layer->count - 1;
    if (lower > upper) return;

    float local = center.y - base.y;
    float sprayShare = smoothstepf(60, 420, v2len(velocity));
    float pushedLeft = 0, pushedRight = 0;

    for (int i = lower; i <= upper; i++) {
        if (!layer->visible[i]) continue;
        float x = layer_center_x(layer, i);
        float dx = x - center.x;
        float reach = radius * radius - dx * dx;
        if (!(reach > 0)) continue;
        float half = sqrtf(reach);
        float bedLevel = layer->bed[i];
        float bottom = fmaxf(0, local - half - bedLevel);
        float top = local + half - bedLevel;
        if (!(top > bottom)) continue;
        float h = layer->heights[i];
        int buried = h > top;
        if (final && !buried && h >= bottom - 1) {
            sim_hold_layer(sim, layer);
            layer_hold(layer, i);
        }
        if (!(h > bottom + 0.01f)) continue;

        float removed = buried ? (top - bottom) * fminf(1, travel / (2 * radius)) : h - bottom;
        if (!(removed > 0.001f)) continue;
        layer_set(layer, i, h - removed);
        sim->lastCursorPush = sim->clock_;
        sim->heightsChanged = 1;

        float area = removed * layer->columnWidth;
        float surface = layer_floor_y(layer, i) + (buried ? h - removed : h);
        float sprayed = sprayShare > 0 ? sim_emit(sim, area * sprayShare, v2make(x, surface), center, velocity) : 0;
        float side = fabsf(velocity.x) > 40 ? velocity.x : dx;
        if (side < 0) pushedLeft += area - sprayed;
        else pushedRight += area - sprayed;
    }

    struct { float area, x; } pushes[2] = {
        {pushedLeft, center.x - radius - layer->columnWidth * 3},
        {pushedRight, center.x + radius + layer->columnWidth * 3},
    };
    for (int k = 0; k < 2; k++) {
        if (!(pushes[k].area > 0)) continue;
        float missed = layer_deposit(layer, pushes[k].area, pushes[k].x, 1);
        sim_drop_past(sim, layer, missed, pushes[k].x, velocity.x * 0.5f);
    }
}

static void sim_track_cursor(SnowSim *sim, int hasCursor, v2 point, float dt)
{
    for (int i = 0; i < sim->heldCount; i++) layer_release_support(sim->held[i]);
    sim->heldCount = 0;
    if (!hasCursor) {
        sim->hasCursor = 0;
        return;
    }

    v2 center = v2make(point.x + CURSOR_OFFSET_X, point.y + CURSOR_OFFSET_Y);
    if (!sim->hasCursor) {
        sim->cursor = center;
        sim->hasCursor = 1;
        return;
    }
    v2 previous = sim->cursor;
    sim->cursor = center;

    v2 delta = v2sub(center, previous);
    v2 velocity = v2mul(delta, 1.0f / fmaxf(dt, 1.0f / 240));
    float speed = v2len(velocity);
    if (speed > 3000) velocity = v2mul(velocity, 3000 / speed);
    sim->cursorVelocity = v2add(sim->cursorVelocity, v2mul(v2sub(velocity, sim->cursorVelocity), 0.5f));

    float low = fminf(center.y, previous.y) - CURSOR_RADIUS;
    float high = fmaxf(center.y, previous.y) + CURSOR_RADIUS;
    float left = fminf(center.x, previous.x) - CURSOR_RADIUS;
    float right = fmaxf(center.x, previous.x) + CURSOR_RADIUS;

    SnowLayer *touched[256];
    int touchedCount = 0;
    for (int i = 0; i < sim->layerCount && touchedCount < 256; i++) {
        SnowLayer *layer = sim->layers[i];
        v2 base = layer_base(layer);
        if (low <= base.y + layer->top + 1 && high >= base.y - layer->bedDepth &&
            right >= base.x && left <= base.x + (float)layer->count * layer->columnWidth &&
            (layer == sim->ground || layer->top > 0.01f)) {
            touched[touchedCount++] = layer;
        }
    }
    if (touchedCount == 0) return;

    float distance = v2len(delta);
    if (distance > 400) {
        for (int i = 0; i < touchedCount; i++)
            sim_carve(sim, touched[i], center, v2make(0, 0), 0, 1);
        return;
    }
    int steps = (int)(distance / (CURSOR_RADIUS * 0.5f)) + 1;
    for (int step = 1; step <= steps; step++) {
        v2 position = v2add(previous, v2mul(delta, (float)step / (float)steps));
        for (int i = 0; i < touchedCount; i++)
            sim_carve(sim, touched[i], position, velocity, distance / (float)steps, step == steps);
    }
}

/* ---- falling snow ---- */

static int sim_landing(SnowSim *sim, float x, float from, float to, SnowLayer **outLayer, int *outIndex)
{
    SnowLayer *hitLayer = NULL;
    int hitIndex = -1;
    float highest = -3.4e38f;
    for (int k = 0; k < sim->ledgeCount; k++) {
        SnowLayer *ledge = sim->ledges[k].layer;
        int i = layer_index_at(ledge, x);
        if (i < 0 || !ledge->visible[i]) continue;
        float surface = layer_surface_y(ledge, i);
        if (from >= ledge->previousBaseY + ledge->bed[i] && to <= surface && surface > highest) {
            hitLayer = ledge;
            hitIndex = i;
            highest = surface;
        }
    }
    if (hitLayer) {
        *outLayer = hitLayer;
        *outIndex = hitIndex;
        return 1;
    }
    int i = layer_clamped_index(sim->ground, x);
    if (to <= layer_surface_y(sim->ground, i)) {
        *outLayer = sim->ground;
        *outIndex = i;
        return 1;
    }
    return 0;
}

static void sim_remove_flake(SnowSim *sim, int i)
{
    sim->flakes[i] = sim->flakes[sim->flakeCount - 1];
    sim->flakeCount--;
}

static void sim_remove_grain(SnowSim *sim, int i)
{
    sim->grains[i] = sim->grains[sim->grainCount - 1];
    sim->grainCount--;
}

static Flake sim_make_flake(SnowSim *sim, float y)
{
    int near = rnd_next(&sim->rnd) < 0.02f;
    float depth = near ? 1 : powf(rnd_next(&sim->rnd), 1.6f);
    float fallSpeed = near ? rnd_range(&sim->rnd, 75, 100) : 16 + depth * 50 + rnd_range(&sim->rnd, 0, 10);
    Flake f;
    memset(&f, 0, sizeof(f));
    f.position = v2make(rnd_range(&sim->rnd, -30, sim->width + 30), y);
    f.velocity = v2make(sim_wind_speed(sim) * (0.35f + 0.65f * depth), -fallSpeed);
    f.fallSpeed = fallSpeed;
    f.size = near ? rnd_range(&sim->rnd, 8, 13) : 1.4f + depth * 4.4f + rnd_range(&sim->rnd, 0, 1);
    f.depth = depth;
    f.alpha = near ? 0.28f : 0.4f + depth * 0.5f;
    f.soft = near ? 0.95f : 0.75f - depth * 0.45f;
    f.phase = rnd_range(&sim->rnd, 0, PHASE_WRAP);
    f.swayRate = rnd_range(&sim->rnd, 0.5f, 1.4f);
    f.swayAmount = near ? rnd_range(&sim->rnd, 10, 20) : rnd_range(&sim->rnd, 4, 9) + depth * 10;
    f.settles = !near;
    f.mass = 1;
    f.tint[0] = f.tint[1] = f.tint[2] = 1;
    f.tintAmount = 0;
    f.blown = 0;
    return f;
}

static void sim_recycle_flake(SnowSim *sim, int i)
{
    if (sim->flakeCount <= sim->targetFlakes && !sim_blowing(sim)) {
        sim->flakes[i] = sim_make_flake(sim, sim->height + rnd_range(&sim->rnd, 4, 40));
    } else {
        sim_remove_flake(sim, i);
    }
}

static void sim_advance_flakes(SnowSim *sim, float dt)
{
    float wind = sim_wind_speed(sim);
    float follow = fminf(1, dt * 1.2f);
    float settle = fminf(1, dt * 1.6f);
    float wakeRadius = 80;
    v2 wakeVelocity = sim->cursorVelocity;
    float wakeSpeed = v2len(wakeVelocity);
    if (wakeSpeed > 500) wakeVelocity = v2mul(wakeVelocity, 500 / wakeSpeed);
    v2 wakeCenter = sim->hasCursor ? sim->cursor : v2make(-10000, -10000);
    int wakeActive = wakeSpeed > 20;

    for (int i = sim->flakeCount - 1; i >= 0; i--) {
        Flake *fp = &sim->flakes[i];
        Flake f = *fp;
        if (f.blown != 0) {
            float target = f.blown * GUST_WIND * (1.3f + 1.3f * f.depth);
            f.velocity.x += (target - f.velocity.x) * fminf(1, dt * 2.5f);
        } else {
            float target = wind * (0.35f + 0.65f * f.depth);
            f.velocity.x += (target - f.velocity.x) * follow;
        }
        f.velocity.y += (-f.fallSpeed - f.velocity.y) * settle;

        if (wakeActive) {
            float distanceSquared = v2len2(v2sub(f.position, wakeCenter));
            if (distanceSquared < wakeRadius * wakeRadius) {
                float falloff = 1 - sqrtf(distanceSquared) / wakeRadius;
                f.velocity = v2add(f.velocity, v2mul(v2sub(wakeVelocity, f.velocity),
                                                     fminf(1, falloff * falloff * 4 * dt)));
            }
        }

        f.phase += f.swayRate * dt;
        if (f.phase > PHASE_WRAP) f.phase -= PHASE_WRAP;
        float sway = (cosf(f.phase) + 0.3f * cosf(f.phase * 2.3f)) * f.swayAmount;
        float flutter = 1 + 0.12f * sinf(f.phase * 1.3f);
        if (f.tintAmount > 0) f.tintAmount = fmaxf(0, f.tintAmount - dt * 1.3f);
        float previousY = f.position.y;
        f.position.x += (f.velocity.x + sway) * dt;
        f.position.y += f.velocity.y * flutter * dt;

        if (f.blown != 0 && (f.position.x < -30 || f.position.x > sim->width + 30)) {
            sim_remove_flake(sim, i);
            continue;
        }
        if (f.position.x < -30) f.position.x += sim->width + 60;
        else if (f.position.x > sim->width + 30) f.position.x -= sim->width + 60;

        if (f.settles && sim->accumulates && f.blown == 0) {
            SnowLayer *hitLayer;
            int hitIndex;
            if (sim_landing(sim, f.position.x, previousY, f.position.y, &hitLayer, &hitIndex)) {
                if (sim->snowfallHeld && f.mass < 1 && hitLayer == sim->ground) sim->snowfallHeld = 0;
                layer_deposit(hitLayer, f.size * f.size * DEPOSITION * f.mass * layer_fill(hitLayer, hitIndex),
                              f.position.x, 1);
                sim->heightsChanged = 1;
                sim_recycle_flake(sim, i);
                continue;
            }
        } else if (f.position.y < -f.size * 2) {
            sim_recycle_flake(sim, i);
            continue;
        }
        sim->flakes[i] = f;
    }
}

static void sim_advance_grains(SnowSim *sim, float dt)
{
    if (sim->grainCount == 0) return;
    float damping = expf(-GRAIN_DRAG * dt);
    for (int i = sim->grainCount - 1; i >= 0; i--) {
        Grain *gp = &sim->grains[i];
        Grain g = *gp;
        float previousY = g.position.y;
        if (g.blown) {
            float speed = g.velocity.x < 0 ? -GUST_WIND * 1.6f : GUST_WIND * 1.6f;
            g.velocity.x += (speed - g.velocity.x) * fminf(1, dt * 2);
            g.velocity.y -= GRAVITY * 0.08f * dt;
        } else {
            g.velocity.y -= GRAVITY * dt;
            g.velocity = v2mul(g.velocity, damping);
        }
        g.position = v2add(g.position, v2mul(g.velocity, dt));

        if (g.position.x < -10 || g.position.x > sim->width + 10 || g.position.y < -10) {
            sim_remove_grain(sim, i);
            continue;
        }
        SnowLayer *hitLayer;
        int hitIndex;
        if (!g.blown && g.velocity.y <= 0 &&
            sim_landing(sim, g.position.x, previousY, g.position.y, &hitLayer, &hitIndex)) {
            if (g.lump > 0) {
                sim_burst(sim, &g, hitLayer);
                sim->heightsChanged = 1;
                sim_remove_grain(sim, i);
                continue;
            }
            float missed = layer_deposit(hitLayer, g.area, g.position.x, 1);
            sim_drop_past(sim, hitLayer, missed, g.position.x, g.velocity.x * 0.5f);
            sim->heightsChanged = 1;
            sim_remove_grain(sim, i);
            continue;
        }
        sim->grains[i] = g;
    }
}

static void sim_advance_motes(SnowSim *sim, float dt)
{
    if (sim->moteCount == 0) return;
    float damping = expf(-3.2f * dt);
    float drift = sim_wind_speed(sim) * 0.4f;
    for (int i = sim->moteCount - 1; i >= 0; i--) {
        Mote *m = &sim->motes[i];
        m->age += dt;
        if (m->age >= m->life) {
            sim->motes[i] = sim->motes[sim->moteCount - 1];
            sim->moteCount--;
            continue;
        }
        m->velocity.y -= 35 * dt;
        m->velocity = v2mul(m->velocity, damping);
        m->position = v2add(m->position, v2mul(v2add(m->velocity, v2make(drift, 0)), dt));
    }
}

static void sim_spawn_flakes(SnowSim *sim)
{
    if (sim->flakeCount >= sim->targetFlakes || sim_blowing(sim)) return;
    int missing = sim->targetFlakes - sim->flakeCount;
    sim_reserve_flakes(sim, sim->flakeCount + missing);
    for (int i = 0; i < missing; i++)
        sim->flakes[sim->flakeCount++] = sim_make_flake(sim, sim->height + rnd_range(&sim->rnd, 0, sim->height * 0.8f));
}

static void sim_vary(SnowSim *sim, float dt)
{
    if (sim->snowfall == SNOWFALL_VARIABLE && sim->clock_ >= sim->nextShift) {
        float roll = rnd_next(&sim->rnd);
        if (roll < 0.25f) sim->strengthTarget = rnd_range(&sim->rnd, 0.25f, 0.5f);
        else if (roll < 0.75f) sim->strengthTarget = rnd_range(&sim->rnd, 0.7f, 1.3f);
        else sim->strengthTarget = rnd_range(&sim->rnd, 1.5f, 2.1f);
        sim->nextShift = sim->clock_ + rnd_range(&sim->rnd, 60, 240);
    }
    if (fabsf(sim->strengthTarget - sim->strength) <= 0.001f) return;
    sim->strength += (sim->strengthTarget - sim->strength) * (1 - expf(-dt / 25));
    sim_update_target(sim);
}

/* ---- gust ---- */

static float sim_gust_front(SnowSim *sim, float age, float direction, float margin)
{
    float t = clampf(age / GUST_SWEEP, 0, 1);
    float travel = (sim->width + 2 * margin) * (1 - (1 - t) * (1 - t));
    return direction > 0 ? -margin + travel : sim->width + margin - travel;
}

static void sim_lift(SnowSim *sim, v2 position, float direction)
{
    if (sim->grainCount >= MAX_GRAINS) return;
    sim_reserve_grains(sim, sim->grainCount + 1);
    Grain *g = &sim->grains[sim->grainCount++];
    g->position = v2add(position, v2make(rnd_range(&sim->rnd, -1, 1), rnd_range(&sim->rnd, -3, 0)));
    g->velocity = v2make(direction * rnd_range(&sim->rnd, 800, 1800), rnd_range(&sim->rnd, 60, 420));
    g->area = 0;
    g->size = rnd_range(&sim->rnd, 1.1f, 2.6f);
    g->lump = 0;
    g->blown = 1;
    if (rnd_next(&sim->rnd) >= 0.35f || sim->moteCount >= MAX_MOTES) return;
    sim_reserve_motes(sim, sim->moteCount + 1);
    Mote *m = &sim->motes[sim->moteCount++];
    m->position = position;
    m->velocity = v2make(direction * rnd_range(&sim->rnd, 200, 600), rnd_range(&sim->rnd, 20, 120));
    m->size = rnd_range(&sim->rnd, 6, 14);
    m->alpha = rnd_range(&sim->rnd, 0.06f, 0.14f);
    m->age = 0;
    m->life = rnd_range(&sim->rnd, 0.6f, 1.3f);
}

static void sim_blow(SnowSim *sim, float dt)
{
    if (!sim->gust.active) return;
    float age = (float)(sim->clock_ - sim->gust.start);
    int anyBlown = 0;
    for (int i = 0; i < sim->flakeCount; i++)
        if (sim->flakes[i].blown != 0) { anyBlown = 1; break; }
    if (!(age < GUST_LENGTH || anyBlown)) {
        sim->gust.active = 0;
        return;
    }
    float margin = 40;
    float direction = sim->gust.direction;
    float front = sim_gust_front(sim, age, direction, margin);
    int finish = age > GUST_SWEEP + (sim->gust.calm ? 0.6f : 0.8f);

    if (!sim->gust.calm) {
        float trailing = sim_gust_front(sim, age - GUST_LAG, direction, margin);
        for (int i = 0; i < sim->flakeCount; i++) {
            if (sim->flakes[i].blown != 0) continue;
            float stagger = sim->flakes[i].phase / PHASE_WRAP * 240;
            if (finish || (trailing - sim->flakes[i].position.x) * direction > stagger)
                sim->flakes[i].blown = direction;
        }
    }

    for (int k = 0; k < sim->layerCount; k++) {
        SnowLayer *layer = sim->layers[k];
        float dust = 0;
        for (int i = 0; i < layer->count; i++) {
            float height = layer->heights[i];
            if (!(height > 0)) continue;
            float x = layer_center_x(layer, i);
            if (!sim->gust.calm && !((front - x) * direction > 0)) continue;
            float surface = layer_surface_y(layer, i);
            float loss = finish ? height
                                : fminf(height, (sim->gust.calm ? height * 2.5f + 6 : height * 8 + 50) * dt);
            layer_set(layer, i, height - loss);
            sim->heightsChanged = 1;
            if (sim->gust.calm || !layer->visible[i]) continue;
            dust += loss * layer->columnWidth;
            while (dust >= 3.5f) {
                sim_lift(sim, v2make(x, surface), direction);
                dust -= 3.5f;
            }
        }
    }
}

void sim_blow_away(SnowSim *sim, float direction, bool calm)
{
    sim->gust.active = 1;
    sim->gust.start = sim->clock_;
    sim->gust.direction = direction < 0 ? -1 : 1;
    sim->gust.calm = calm;
    for (int i = 0; i < sim->dormantCount; i++) {
        layer_free(sim->dormant[i].layer);
        free(sim->dormant[i].layer);
    }
    sim->dormantCount = 0;
    for (int i = 0; i < sim->parkingCount; i++) {
        SnowLayer *layer = sim->parking[i].layer;
        for (int c = 0; c < layer->count; c++) layer_set(layer, c, 0);
    }
}

/* ---- settling ---- */

static void sim_settle(SnowSim *sim, float dt)
{
    float falling = snowfall_per_megapoint(sim->snowfall) * sim->strength * sim->density;
    float meltPace = falling > 0 ? powf(falling / THAW_BALANCE, THAW_EXPONENT) : 1;
    for (int k = 0; k < sim->layerCount; k++) {
        SnowLayer *layer = sim->layers[k];
        if (!sim->accumulates || sim->thaws) {
            float downTo = !sim->accumulates ? 0 : (layer == sim->ground ? THAW_DEPTH : LEDGE_THAW_DEPTH);
            if (layer_thaw(layer, dt * meltPace, downTo, THAW_RATE, THAW_TIME)) sim->heightsChanged = 1;
        }
        float repose = layer->agitation > SHAKING ? REPOSE * 0.35f : REPOSE;
        if (layer_relax(layer, repose, ROUNDING)) sim->heightsChanged = 1;
        if (layer->spillLeft > 1.5f) {
            sim_release(sim, layer->spillLeft, sim_edge_point(layer, 1),
                        v2add(layer->trackVelocity, v2make(-6, -15)), v2make(0.25f, 1), 0);
            layer->spillLeft = 0;
        }
        if (layer->spillRight > 1.5f) {
            sim_release(sim, layer->spillRight, sim_edge_point(layer, 0),
                        v2add(layer->trackVelocity, v2make(6, -15)), v2make(0.25f, 1), 0);
            layer->spillRight = 0;
        }
    }
}

bool sim_step(SnowSim *sim, float dt, bool hasCursor, float cursorX, float cursorY)
{
    sim->clock_ += dt;
    int wasMoving = sim->flakeCount || sim->grainCount || sim->moteCount;
    sim->heightsChanged = 0;

    sim_vary(sim, dt);
    sim_expire_parking(sim);
    for (int i = 0; i < sim->ledgeCount; i++) sim_carry(sim, sim->ledges[i].layer, dt);
    if (hasCursor) sim_track_cursor(sim, 1, v2make(cursorX, cursorY), dt);
    else sim_track_cursor(sim, 0, v2make(0, 0), dt);
    for (int i = 0; i < sim->ledgeCount; i++) sim_confine(sim, sim->ledges[i].layer);
    sim_blow(sim, dt);
    sim_advance_flakes(sim, dt);
    sim_advance_grains(sim, dt);
    sim_advance_motes(sim, dt);
    sim_settle(sim, dt);
    sim_spawn_flakes(sim);

    for (int i = 0; i < sim->layerCount; i++) {
        layer_refresh_top(sim->layers[i]);
        sim->layers[i]->previousBaseY = layer_base(sim->layers[i]).y;
    }

    int pushed = sim->clock_ - sim->lastWindowMotion < 0.6 || sim->clock_ - sim->lastCursorPush < 0.3 ||
                 sim->gust.active;
    int flying = sim->grainCount > 24 || sim->clock_ < sim->scatteredUntil;
    if (pushed) sim->pace = PACE_INTERACTIVE;
    else if (flying) sim->pace = PACE_LIVELY;
    else sim->pace = PACE_CALM;

    int changed = wasMoving || sim->flakeCount || sim->grainCount || sim->moteCount ||
                  sim->heightsChanged || sim->windowsChanged;
    sim->windowsChanged = 0;
    return changed != 0;
}

int sim_write_sprites(SnowSim *sim, Sprite *out, int capacity)
{
    int count = 0;
    for (int i = 0; i < sim->flakeCount && count < capacity; i++) {
        Flake *f = &sim->flakes[i];
        Sprite *s = &out[count++];
        s->px = f->position.x;
        s->py = f->position.y;
        s->vx = f->velocity.x;
        s->vy = f->velocity.y;
        s->size = f->size;
        s->alpha = f->alpha;
        s->soft = f->soft;
        s->tintAmount = f->tintAmount;
        s->tintR = f->tint[0];
        s->tintG = f->tint[1];
        s->tintB = f->tint[2];
        s->lump = 0;
    }
    for (int i = 0; i < sim->grainCount && count < capacity; i++) {
        Grain *g = &sim->grains[i];
        Sprite *s = &out[count++];
        s->px = g->position.x;
        s->py = g->position.y;
        s->vx = g->velocity.x;
        s->vy = g->velocity.y;
        s->size = g->size;
        s->alpha = g->lump > 0 ? 0.97f : (g->blown ? 0.8f : 0.95f);
        s->soft = g->lump > 0 ? 0.2f : 0.3f;
        s->tintAmount = 0;
        s->tintR = s->tintG = s->tintB = 1;
        s->lump = g->lump;
    }
    for (int i = 0; i < sim->moteCount && count < capacity; i++) {
        Mote *m = &sim->motes[i];
        float t = m->age / m->life;
        float fade = fminf(1, m->age / 0.06f) * powf(1 - t, 1.5f);
        Sprite *s = &out[count++];
        s->px = m->position.x;
        s->py = m->position.y;
        s->vx = 0;
        s->vy = 0;
        s->size = m->size * (1 + t);
        s->alpha = m->alpha * fade;
        s->soft = 1;
        s->tintAmount = 0;
        s->tintR = s->tintG = s->tintB = 1;
        s->lump = 0;
    }
    return count;
}

int sim_write_layers(SnowSim *sim, float *heights, int capacityFloats, PileLayer *out, int maxLayers)
{
    int count = 0;
    int offset = 0;
    for (int i = 0; i < sim->layerCount; i++) {
        SnowLayer *layer = sim->layers[i];
        if (!(layer->top > 0.05f)) continue;
        if (offset + 2 * layer->count > capacityFloats) continue;
        if (count >= maxLayers) break;
        layer_write_smoothed(layer, heights + offset);
        PileLayer *p = &out[count++];
        p->offset = offset;
        p->count = layer->count;
        p->originX = layer_base(layer).x;
        p->originY = layer_base(layer).y;
        p->top = layer->top;
        p->bottom = -layer->bedDepth;
        p->occluderCount = layer->occluderCount;
        for (int k = 0; k < layer->occluderCount; k++)
            for (int c = 0; c < 4; c++) p->occluders[k][c] = layer->occluders[k][c];
        p->isDock = layer->isDock;
        offset += 2 * layer->count;
    }
    return count;
}

void sim_debug_dump(const SnowSim *sim, char *buffer, int size)
{
    int used = 0;
    used += snprintf(buffer + used, (size_t)(size - used), "size %.0fx%.0f layers %d grains %d flakes %d\n",
                     sim->width, sim->height, sim->layerCount, sim->grainCount, sim->flakeCount);
    for (int i = 0; i < sim->layerCount && used < size - 160; i++) {
        const SnowLayer *layer = sim->layers[i];
        float total = 0;
        int covered = 0;
        for (int c = 0; c < layer->count; c++) {
            total += layer->heights[c] * layer->columnWidth;
            if (layer->heights[c] > 0.05f) covered++;
        }
        used += snprintf(buffer + used, (size_t)(size - used),
                         "  layer %d%s at %.0f,%.0f cols %d top %.1f bed %.1f area %.1f covered %d\n", i,
                         layer->isDock ? " (dock)" : "", layer_base(layer).x, layer_base(layer).y, layer->count,
                         layer->top, layer->bedDepth, total, covered);
    }
}
