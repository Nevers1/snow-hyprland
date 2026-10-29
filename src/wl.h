/* Wayland plumbing: one layer-shell surface per output, drawn with EGL.
 *
 * The surface sits on the overlay layer (in front of windows, like the macOS
 * build) or the background layer, spans the whole output, and takes no input:
 * an empty input region makes every click fall through to whatever is beneath.
 */
#ifndef SNOW_WL_H
#define SNOW_WL_H

#include <EGL/egl.h>
#include <stdint.h>

#include "sim.h"

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_shm;
struct wl_output;
struct wl_surface;
struct wl_egl_window;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

typedef struct Snow Snow;

typedef struct Output {
    Snow *snow;
    struct wl_output *wl_output;
    uint32_t name_id;
    char name[64];
    int positionX, positionY;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_egl_window *eglWindow;
    EGLSurface eglSurface;
    int configured;
    int width, height;   /* logical size, from the layer-shell configure */
    EGLContext context;

    SnowSim *sim;
    void *renderer;
    int drawn;
    int muted; /* switched off, or covered by a fullscreen window */
} Output;

struct Snow {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layerShell;

    EGLDisplay eglDisplay;
    EGLConfig eglConfig;
    EGLContext eglContext;
    int eglReady;

    Output **outputs;
    int outputCount;
    int outputCapacity;

    int layerLevel; /* 0 background, 3 overlay */
    int running;

    /* set by the app */
    void (*on_output_ready)(Output *output);
    void (*on_output_gone)(Output *output);
    void (*on_before_draw)(void);
};

int wl_start(Snow *snow, int behindWindows);
/* Rebuilds every surface on another layer, e.g. when "behind windows" is toggled. */
void wl_set_layer(Snow *snow, int layerLevel);
void wl_shutdown(Snow *snow);
/* Waits for events on the display (and `extraFd`, if >= 0).
 * Returns a mask: 1 = display events read, 2 = extra fd readable; -1 on error. */
int wl_wait(Snow *snow, int extraFd, int timeoutMs);
int wl_flush(Snow *snow);
/* Binds the shared context to this output's surface. */
int wl_begin_frame(Snow *snow, Output *output, int width, int height);
void wl_end_frame(Snow *snow, Output *output);
Output *wl_find_output(Snow *snow, const char *name);

#endif
