/* Hyprland IPC: windows, monitors, layer surfaces and the pointer.
 *
 * Hyprland has no public global-cursor request besides `cursorpos`, and no
 * window list besides `clients`, so this is where the Linux port gets the
 * things the macOS original read from CoreGraphics.
 */
#ifndef SNOW_HYPR_H
#define SNOW_HYPR_H

#include <stdint.h>

#define HYPR_MAX_LAYERS 128

typedef struct {
    uint64_t id;
    int x, y, w, h;      /* global layout coordinates, y down from the layout top */
    int monitor;
    int workspaceId;
    int fullscreen;
    int floating;
    int mapped;
    int hidden;
    int visible;
    int pinned;
    char klass[128];
} HyprClient;

typedef struct {
    char name[64];
    int x, y, w, h;
    float scale;
    int focused;
    int activeWorkspaceId;
    int specialWorkspaceId;
} HyprMonitor;

typedef struct {
    int x, y, w, h;
    int level;
    char name[64];
} HyprLayer;

typedef struct {
    HyprClient *clients;
    int clientCount;
    HyprMonitor *monitors;
    int monitorCount;
    HyprLayer layers[HYPR_MAX_LAYERS];
    int layerCount;
    int rounding;
    int ok;
} HyprState;

int hypr_init(void);
void hypr_state_free(HyprState *state);
/* Polls monitors, windows and layer surfaces. Returns 1 on success. */
int hypr_refresh(HyprState *state);
/* The pointer in global layout coordinates. Returns 1 when available. */
int hypr_cursor(double *x, double *y);

#endif
