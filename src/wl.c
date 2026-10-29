#include "wl.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "layer-shell-client-protocol.h"

static void output_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                             uint32_t width, uint32_t height);

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = output_configure,
    .closed = NULL,
};

static int ensure_egl(Snow *snow);

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t physicalWidth, int32_t physicalHeight, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    (void)output;
    (void)physicalWidth;
    (void)physicalHeight;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
    Output *o = data;
    o->positionX = x;
    o->positionY = y;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width,
                        int32_t height, int32_t refresh)
{
    (void)data;
    (void)output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

static void output_done(void *data, struct wl_output *output) { (void)data; (void)output; }
static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)data;
    (void)output;
    (void)factor;
}
static void output_name(void *data, struct wl_output *output, const char *name)
{
    (void)output;
    Output *o = data;
    snprintf(o->name, sizeof(o->name), "%s", name);
}
static void output_description(void *data, struct wl_output *output, const char *description)
{
    (void)data;
    (void)output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void create_surface(Snow *snow, Output *o)
{
    if (o->surface) return;
    o->surface = wl_compositor_create_surface(snow->compositor);
    if (!o->surface) return;

    struct wl_region *empty = wl_compositor_create_region(snow->compositor);
    wl_surface_set_input_region(o->surface, empty);
    wl_region_destroy(empty);
    wl_surface_set_buffer_scale(o->surface, 1);

    o->layer = zwlr_layer_shell_v1_get_layer_surface(snow->layerShell, o->surface, o->wl_output,
                                                     (uint32_t)snow->layerLevel, "snowhush");
    zwlr_layer_surface_v1_set_size(o->layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor(o->layer,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, 0);
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
    wl_surface_commit(o->surface);
}

static void destroy_surface(Output *o)
{
    if (o->eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(o->snow->eglDisplay, o->eglSurface);
        o->eglSurface = EGL_NO_SURFACE;
    }
    if (o->eglWindow) {
        wl_egl_window_destroy(o->eglWindow);
        o->eglWindow = NULL;
    }
    if (o->layer) {
        zwlr_layer_surface_v1_destroy(o->layer);
        o->layer = NULL;
    }
    if (o->surface) {
        wl_surface_destroy(o->surface);
        o->surface = NULL;
    }
    o->configured = 0;
}

static void output_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                             uint32_t width, uint32_t height)
{
    Output *o = data;
    Snow *snow = o->snow;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (width == 0 || height == 0) return;

    if (o->eglWindow && (o->width != (int)width || o->height != (int)height)) {
        wl_egl_window_resize(o->eglWindow, (int)width, (int)height, 0, 0);
    }
    o->width = (int)width;
    o->height = (int)height;

    if (!ensure_egl(snow)) return;
    if (!o->eglWindow) {
        o->eglWindow = wl_egl_window_create(o->surface, o->width, o->height);
        if (!o->eglWindow) return;
        o->eglSurface = eglCreatePlatformWindowSurface(snow->eglDisplay, snow->eglConfig, o->eglWindow, NULL);
        if (o->eglSurface == EGL_NO_SURFACE) {
            fprintf(stderr, "snowhush: eglCreatePlatformWindowSurface failed: 0x%x\n", eglGetError());
            return;
        }
    }
    int first = !o->configured;
    o->configured = 1;
    if (first && snow->on_output_ready) snow->on_output_ready(o);
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    Snow *snow = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        snow->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        snow->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        snow->layerShell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        Output *o = calloc(1, sizeof(Output));
        o->snow = snow;
        o->name_id = name;
        o->eglSurface = EGL_NO_SURFACE;
        o->wl_output = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
        uint32_t v = version < 4 ? version : 4;
        if (v >= 4) wl_output_add_listener(o->wl_output, &output_listener, o);
        if (snow->outputCount == snow->outputCapacity) {
            snow->outputCapacity = snow->outputCapacity ? snow->outputCapacity * 2 : 4;
            snow->outputs = realloc(snow->outputs, sizeof(Output *) * (size_t)snow->outputCapacity);
        }
        snow->outputs[snow->outputCount++] = o;
        if (snow->layerShell && snow->compositor) create_surface(snow, o);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    Snow *snow = data;
    for (int i = 0; i < snow->outputCount; i++) {
        Output *o = snow->outputs[i];
        if (o->name_id != name) continue;
        if (snow->on_output_gone) snow->on_output_gone(o);
        destroy_surface(o);
        wl_output_destroy(o->wl_output);
        if (o->sim) {
            sim_destroy(o->sim);
            o->sim = NULL;
        }
        free(o);
        snow->outputs[i] = snow->outputs[snow->outputCount - 1];
        snow->outputCount--;
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int ensure_egl(Snow *snow)
{
    if (snow->eglReady) return 1;
    snow->eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, snow->display, NULL);
    if (snow->eglDisplay == EGL_NO_DISPLAY) {
        fprintf(stderr, "snowhush: no EGL display\n");
        return 0;
    }
    if (!eglInitialize(snow->eglDisplay, NULL, NULL)) {
        fprintf(stderr, "snowhush: eglInitialize failed: 0x%x\n", eglGetError());
        return 0;
    }
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint configCount = 0;
    if (!eglChooseConfig(snow->eglDisplay, attributes, &snow->eglConfig, 1, &configCount) || configCount == 0) {
        fprintf(stderr, "snowhush: no matching EGL config\n");
        return 0;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    snow->eglContext = eglCreateContext(snow->eglDisplay, snow->eglConfig, EGL_NO_CONTEXT, contextAttributes);
    if (snow->eglContext == EGL_NO_CONTEXT) {
        fprintf(stderr, "snowhush: eglCreateContext failed: 0x%x\n", eglGetError());
        return 0;
    }
    snow->eglReady = 1;
    return 1;
}

int wl_start(Snow *snow, int behindWindows)
{
    snow->layerLevel = behindWindows ? ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND : ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
    snow->display = wl_display_connect(NULL);
    if (!snow->display) {
        fprintf(stderr, "snowhush: cannot connect to Wayland\n");
        return 0;
    }
    snow->registry = wl_display_get_registry(snow->display);
    wl_registry_add_listener(snow->registry, &registry_listener, snow);
    wl_display_roundtrip(snow->display);
    if (!snow->compositor || !snow->layerShell) {
        fprintf(stderr, "snowhush: the compositor has no wlr-layer-shell support\n");
        return 0;
    }
    for (int i = 0; i < snow->outputCount; i++)
        if (!snow->outputs[i]->surface) create_surface(snow, snow->outputs[i]);
    /* Second roundtrip: outputs get their names and the layers get configured. */
    wl_display_roundtrip(snow->display);
    wl_display_roundtrip(snow->display);
    snow->running = 1;
    return 1;
}

void wl_set_layer(Snow *snow, int layerLevel)
{
    snow->layerLevel = layerLevel;
    for (int i = 0; i < snow->outputCount; i++) {
        Output *o = snow->outputs[i];
        if (snow->on_output_gone) snow->on_output_gone(o);
        destroy_surface(o);
    }
    for (int i = 0; i < snow->outputCount; i++)
        if (!snow->outputs[i]->surface) create_surface(snow, snow->outputs[i]);
    wl_display_roundtrip(snow->display);
}

int wl_begin_frame(Snow *snow, Output *output, int width, int height)
{
    if (!output->eglSurface) return 0;
    if (!eglMakeCurrent(snow->eglDisplay, output->eglSurface, output->eglSurface, snow->eglContext)) return 0;
    return 1;
}

void wl_end_frame(Snow *snow, Output *output)
{
    eglSwapBuffers(snow->eglDisplay, output->eglSurface);
}

int wl_wait(Snow *snow, int extraFd, int timeoutMs)
{
    struct pollfd fds[2];
    int fdCount = 0;
    fds[fdCount].fd = wl_display_get_fd(snow->display);
    fds[fdCount].events = POLLIN;
    fds[fdCount].revents = 0;
    fdCount++;
    if (extraFd >= 0) {
        fds[fdCount].fd = extraFd;
        fds[fdCount].events = POLLIN;
        fds[fdCount].revents = 0;
        fdCount++;
    }

    int prepared = wl_display_prepare_read(snow->display) == 0;
    if (!prepared) {
        /* Events are already queued: drain them instead of sleeping. */
        if (wl_display_dispatch_pending(snow->display) < 0) return -1;
    } else {
        wl_display_flush(snow->display);
    }

    int result = 0;
    int ready = poll(fds, (nfds_t)fdCount, timeoutMs);
    if (ready < 0) {
        if (prepared) wl_display_cancel_read(snow->display);
        return (errno == EINTR) ? 0 : -1;
    }
    if (prepared) {
        if ((fds[0].revents & POLLIN) && ready > 0) {
            if (wl_display_read_events(snow->display) < 0) return -1;
            result |= 1;
        } else {
            wl_display_cancel_read(snow->display);
        }
        if (wl_display_dispatch_pending(snow->display) < 0) return -1;
    }
    if (extraFd >= 0 && (fds[fdCount - 1].revents & POLLIN)) result |= 2;
    return result;
}

int wl_flush(Snow *snow) { return wl_display_flush(snow->display); }

Output *wl_find_output(Snow *snow, const char *name)
{
    for (int i = 0; i < snow->outputCount; i++)
        if (strcmp(snow->outputs[i]->name, name) == 0) return snow->outputs[i];
    return NULL;
}

void wl_shutdown(Snow *snow)
{
    for (int i = 0; i < snow->outputCount; i++) {
        Output *o = snow->outputs[i];
        destroy_surface(o);
        if (o->wl_output) wl_output_destroy(o->wl_output);
        if (o->sim) sim_destroy(o->sim);
        free(o);
    }
    free(snow->outputs);
    snow->outputs = NULL;
    snow->outputCount = 0;
    if (snow->eglReady) {
        eglMakeCurrent(snow->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(snow->eglDisplay, snow->eglContext);
        eglTerminate(snow->eglDisplay);
    }
    if (snow->layerShell) zwlr_layer_shell_v1_destroy(snow->layerShell);
    if (snow->compositor) wl_compositor_destroy(snow->compositor);
    if (snow->registry) wl_registry_destroy(snow->registry);
    if (snow->display) wl_display_disconnect(snow->display);
}
