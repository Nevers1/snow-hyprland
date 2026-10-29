/* Snowhush for Hyprland: the snow simulation.
 *
 * A faithful port of the SnowSimulation / SnowLayer pair from the original
 * macOS app, kept line-for-line close on purpose: the behaviour (drift, settle,
 * slumping angle of repose, window adhesion, cursor carving, gust clearing) is
 * what makes it look right.
 *
 * Coordinates are overlay coordinates: origin at the bottom left of the output,
 * +x right, +y up, in logical pixels.
 */
#ifndef SNOW_SIM_H
#define SNOW_SIM_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_OCCLUDERS 16

typedef enum {
    SNOWFALL_OFF = 0,
    SNOWFALL_LIGHT,
    SNOWFALL_STEADY,
    SNOWFALL_HEAVY,
    SNOWFALL_VARIABLE,
} Snowfall;

typedef enum {
    PACE_CALM = 0,
    PACE_LIVELY,
    PACE_INTERACTIVE,
} Pace;

typedef struct {
    float x, y;
} v2;

/* One drawn flake, grain or mote. Layout matches the vertex attributes. */
typedef struct {
    float px, py;
    float vx, vy;
    float size;
    float alpha;
    float soft;
    float tintAmount;
    float tintR, tintG, tintB;
    float lump;
} Sprite;

typedef struct {
    int offset;
    int count;
    float originX, originY;
    float top;
    float bottom;
    float occluders[MAX_OCCLUDERS][4];
    int occluderCount;
    int isDock;
} PileLayer;

/* A window that can carry snow, in overlay coordinates. */
typedef struct {
    uint64_t id;
    float x, y, w, h;
    int cornerCount;
    float corners[MAX_OCCLUDERS][4];
    float cornerRadius;
    float cornerExponent;
    int isDock;
} WinSurface;

/* Sets of window ids, used to tell "closed" from "moved to another workspace". */
typedef struct {
    uint64_t *ids;
    int count;
    int cap;
} IdSet;

void idset_clear(IdSet *s);
void idset_add(IdSet *s, uint64_t id);
bool idset_has(const IdSet *s, uint64_t id);
void idset_free(IdSet *s);

typedef struct SnowSim SnowSim;

SnowSim *sim_create(float width, float height, Snowfall snowfall);
void sim_destroy(SnowSim *sim);

void sim_resize(SnowSim *sim, float width, float height);
bool sim_step(SnowSim *sim, float dt, bool hasCursor, float cursorX, float cursorY);

int sim_sprite_count(const SnowSim *sim);
int sim_write_sprites(SnowSim *sim, Sprite *out, int capacity);
int sim_column_count(const SnowSim *sim);
/* Writes smoothed surfaces then beds into `heights`, returns the number of piles. */
int sim_write_layers(SnowSim *sim, float *heights, int capacityFloats, PileLayer *out, int maxLayers);

/* Preferences. */
void sim_set_snowfall(SnowSim *sim, Snowfall snowfall);
void sim_set_density(SnowSim *sim, float density);
void sim_set_thaws(SnowSim *sim, bool thaws);
void sim_set_accumulates(SnowSim *sim, bool accumulates);
void sim_set_walls(SnowSim *sim, bool left, bool right);

Pace sim_pace(const SnowSim *sim);
bool sim_has_snow(const SnowSim *sim);
float sim_breeze_direction(const SnowSim *sim);
void sim_blow_away(SnowSim *sim, float direction, bool calm);

/* Windows on this output. `parked` are windows that vanished without notice,
 * `elsewhere` are windows that still exist but are out of sight (another
 * workspace): their snow waits for them. */
void sim_update_windows(SnowSim *sim, const WinSurface *surfaces, int count,
                        float interval, const IdSet *parked, const IdSet *elsewhere,
                        bool snowOnWindows);

/* Diagnostic dump of every snow carrier: used by `snowhush ctl surfaces`. */
void sim_debug_dump(const SnowSim *sim, char *buffer, int size);

#endif
