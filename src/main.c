/* Snowhush for Hyprland.
 *
 * Soft snow on the desktop: it falls, settles along the bottom of the screen
 * and on the top edges of windows, piles against the edges of the layout,
 * parts under the pointer, slides off windows that are dragged or shaken, and
 * drops in lumps that burst into powder when a window closes.
 *
 * A port of mbezruchko/Snowhush (MIT) to Wayland/Hyprland: window tracking and
 * the pointer come from Hyprland's IPC, everything else is the original.
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "gl.h"
#include "hypr.h"
#include "image.h"
#include "json.h"
#include "sim.h"
#include "tray.h"
#include "wl.h"

#define MAX_SURFACES 128

static inline float main_smoothstep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return t * t * (3 - 2 * t);
}

static inline float main_sqrf(float v) { return v * v; }

typedef struct {
    Snow snow;
    HyprState state;
    Renderer *renderer;

    Snowfall snowfall;
    int accumulates;
    int thaws;
    int snowOnWindows;
    int behindWindows;
    int hideFullscreen;
    /* Outputs that are switched off, comma separated. Empty means every output. */
    char monitorsOff[512];
    int maxFps;

    double pollAt;
    double lastWindowChange;
    double lastPoll;
    uint64_t stateHash;

    int controlFd;
    char controlPath[256];

    Tray *tray;

    int running;
} App;

static App app;

/* Declared further down; the control socket answers with them. */
static float monitor_top_inset(const HyprState *state, const HyprMonitor *monitor);
static void build_surfaces(const HyprState *state, const HyprMonitor *monitor, int monitorIndex,
                           WinSurface *surfaces, int *outCount, IdSet *elsewhere, IdSet *parked);

/* ------------------------------------------------------------------ */
/* config and control                                                  */

static char *config_path(char *buffer, size_t size, const char *name)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (config && *config) snprintf(buffer, size, "%s/snowhush/%s", config, name);
    else if (home) snprintf(buffer, size, "%s/.config/snowhush/%s", home, name);
    else snprintf(buffer, size, "/tmp/snowhush/%s", name);
    return buffer;
}

static Snowfall parse_snowfall(const char *text)
{
    if (!text) return SNOWFALL_STEADY;
    if (strcmp(text, "off") == 0) return SNOWFALL_OFF;
    if (strcmp(text, "light") == 0) return SNOWFALL_LIGHT;
    if (strcmp(text, "heavy") == 0) return SNOWFALL_HEAVY;
    if (strcmp(text, "variable") == 0) return SNOWFALL_VARIABLE;
    return SNOWFALL_STEADY;
}

static const char *snowfall_name(Snowfall s)
{
    switch (s) {
    case SNOWFALL_OFF: return "off";
    case SNOWFALL_LIGHT: return "light";
    case SNOWFALL_HEAVY: return "heavy";
    case SNOWFALL_VARIABLE: return "variable";
    case SNOWFALL_STEADY:
    default: return "steady";
    }
}

static int parse_onoff(const char *text, int fallback)
{
    if (!text) return fallback;
    if (strcmp(text, "on") == 0 || strcmp(text, "true") == 0 || strcmp(text, "1") == 0) return 1;
    if (strcmp(text, "off") == 0 || strcmp(text, "false") == 0 || strcmp(text, "0") == 0) return 0;
    return fallback;
}

static void config_load(void)
{
    char path[512];
    config_path(path, sizeof(path), "config");
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals = 0;
        char *key = line;
        char *value = equals + 1;
        while (*key == ' ' || *key == '\t') key++;
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        while (*value == ' ' || *value == '\t') value++;
        end = value + strlen(value);
        while (end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (strcmp(key, "snowfall") == 0) app.snowfall = parse_snowfall(value);
        else if (strcmp(key, "accumulate") == 0) app.accumulates = parse_onoff(value, app.accumulates);
        else if (strcmp(key, "melt") == 0) app.thaws = parse_onoff(value, app.thaws);
        else if (strcmp(key, "snow_on_windows") == 0) app.snowOnWindows = parse_onoff(value, app.snowOnWindows);
        else if (strcmp(key, "behind_windows") == 0) app.behindWindows = parse_onoff(value, app.behindWindows);
        else if (strcmp(key, "hide_fullscreen") == 0) app.hideFullscreen = parse_onoff(value, app.hideFullscreen);
        else if (strcmp(key, "monitors_off") == 0) snprintf(app.monitorsOff, sizeof(app.monitorsOff), "%s", value);
        else if (strcmp(key, "fps") == 0) app.maxFps = atoi(value);
    }
    fclose(file);
}

static void config_save(void)
{
    char path[512], directory[512];
    config_path(path, sizeof(path), "config");
    snprintf(directory, sizeof(directory), "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = 0;
        mkdir(directory, 0755);
    }
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file,
            "# Snowhush for Hyprland\n"
            "snowfall = %s\n"
            "accumulate = %s\n"
            "melt = %s\n"
            "snow_on_windows = %s\n"
            "behind_windows = %s\n"
            "# Comma separated outputs without snow. Empty means every output has it.\n"
            "monitors_off = %s\n"
            "# No snow while a window is fullscreen.\n"
            "hide_fullscreen = %s\n"
            "fps = %d\n",
            snowfall_name(app.snowfall), app.accumulates ? "on" : "off", app.thaws ? "on" : "off",
            app.snowOnWindows ? "on" : "off", app.behindWindows ? "on" : "off", app.monitorsOff,
            app.hideFullscreen ? "on" : "off", app.maxFps);
    fclose(file);
}

/* ------------------------------------------------------------------ */
/* which outputs get snow                                              */

/* The config stores the outputs that are switched *off*, so an empty list means
 * "every output" without losing the case where all of them were turned off. */
static int name_in_list(const char *list, const char *name)
{
    if (!list[0]) return 0;
    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t length = comma ? (size_t)(comma - p) : strlen(p);
        if (length == strlen(name) && strncmp(p, name, length) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

static int monitor_enabled(const char *name) { return !name_in_list(app.monitorsOff, name); }

static void monitor_set_enabled(const char *name, int enabled)
{
    char list[512] = "";
    size_t used = 0;
    for (int i = 0; i < app.state.monitorCount; i++) {
        const char *monitor = app.state.monitors[i].name;
        int on = strcmp(monitor, name) == 0 ? enabled : monitor_enabled(monitor);
        if (on) continue; /* only the switched off ones are kept */
        size_t length = strlen(monitor);
        if (used + length + 2 >= sizeof(list)) break;
        if (used > 0) list[used++] = ',';
        memcpy(list + used, monitor, length);
        used += length;
        list[used] = 0;
    }
    snprintf(app.monitorsOff, sizeof(app.monitorsOff), "%s", list);
}

static void apply_settings(void)
{
    for (int i = 0; i < app.snow.outputCount; i++) {
        Output *o = app.snow.outputs[i];
        if (!o->sim) continue;
        sim_set_snowfall(o->sim, app.snowfall);
        sim_set_accumulates(o->sim, app.accumulates);
        sim_set_thaws(o->sim, app.thaws);
    }
}

/* ------------------------------------------------------------------ */
/* tray menu                                                           */

enum {
    MENU_SNOWFALL = 1,
    MENU_OFF = 11,
    MENU_LIGHT,
    MENU_STEADY,
    MENU_HEAVY,
    MENU_VARIABLE,
    MENU_MONITORS,
    MENU_WINDOWS = 21,
    MENU_ACCUMULATE,
    MENU_MELT,
    MENU_FULLSCREEN,
    MENU_SEPARATOR = 25,
    MENU_CLEAR,
    MENU_QUIT,
    MENU_MONITOR_BASE = 100,
};

static void tray_menu_event(int id);
static int tray_build(TrayItem *items, int capacity);

static const TrayCallbacks trayCallbacks = {
    .title = "Snowhush",
    .build = tray_build,
    .event = tray_menu_event,
};

/* Names and labels have to outlive the D-Bus reply, so they live here. */
static char trayMonitorNames[16][64];
static char trayMonitorLabels[16][96];
static int trayMonitorCount;

/* A StatusNotifierHost *may* draw a checkbox for a toggle item, but Waybar's
 * menu does not, and a setting you cannot see is a setting you cannot trust.
 * So the state is written into the label itself: a filled or hollow dot for
 * the exclusive choices, a tick for the switches. */
static char trayStateLabels[TRAY_MAX_ITEMS][112];
static int trayStateLabelAt;

static const char *state_label(TrayKind kind, int state, const char *text)
{
    const char *mark;
    if (kind == TRAY_RADIO)
        mark = state ? "\u25cf  " : "\u25cb  "; /* ● / ○ */
    else
        mark = state ? "\u2713  " : "   "; /* ✓ / blank */

    char *out = trayStateLabels[trayStateLabelAt++ % TRAY_MAX_ITEMS];
    snprintf(out, sizeof(trayStateLabels[0]), "%s%s", mark, text);
    return out;
}

static void menu_add(TrayItem *items, int *count, int capacity, int id, int parent, TrayKind kind,
                     int state, const char *label)
{
    if (*count >= capacity) return;
    TrayItem *item = &items[(*count)++];
    item->id = id;
    item->parent = parent;
    item->kind = kind;
    item->state = state;
    item->enabled = 1;
    item->label = (kind == TRAY_CHECK || kind == TRAY_RADIO) ? state_label(kind, state, label) : label;
}

static int tray_build(TrayItem *items, int capacity)
{
    int count = 0;
    menu_add(items, &count, capacity, MENU_SNOWFALL, 0, TRAY_SUBMENU, 0, "Snowfall");
    menu_add(items, &count, capacity, MENU_OFF, MENU_SNOWFALL, TRAY_RADIO, app.snowfall == SNOWFALL_OFF, "Off");
    menu_add(items, &count, capacity, MENU_LIGHT, MENU_SNOWFALL, TRAY_RADIO, app.snowfall == SNOWFALL_LIGHT, "Light");
    menu_add(items, &count, capacity, MENU_STEADY, MENU_SNOWFALL, TRAY_RADIO, app.snowfall == SNOWFALL_STEADY, "Steady");
    menu_add(items, &count, capacity, MENU_HEAVY, MENU_SNOWFALL, TRAY_RADIO, app.snowfall == SNOWFALL_HEAVY, "Heavy");
    menu_add(items, &count, capacity, MENU_VARIABLE, MENU_SNOWFALL, TRAY_RADIO, app.snowfall == SNOWFALL_VARIABLE,
             "Variable");

    menu_add(items, &count, capacity, MENU_MONITORS, 0, TRAY_SUBMENU, 0, "Screens");
    trayMonitorCount = 0;
    for (int i = 0; i < app.state.monitorCount && i < 16 && count < capacity - 1; i++) {
        const HyprMonitor *monitor = &app.state.monitors[i];
        snprintf(trayMonitorNames[i], sizeof(trayMonitorNames[i]), "%s", monitor->name);
        snprintf(trayMonitorLabels[i], sizeof(trayMonitorLabels[i]), "%s (%dx%d)", monitor->name, monitor->w,
                 monitor->h);
        trayMonitorCount = i + 1;
        menu_add(items, &count, capacity, MENU_MONITOR_BASE + i, MENU_MONITORS, TRAY_CHECK,
                 monitor_enabled(monitor->name), trayMonitorLabels[i]);
    }
    if (trayMonitorCount == 0)
        menu_add(items, &count, capacity, MENU_MONITOR_BASE, MENU_MONITORS, TRAY_PLAIN, 0, "No outputs found");

    menu_add(items, &count, capacity, MENU_WINDOWS, 0, TRAY_CHECK, app.snowOnWindows, "Snow on windows");
    menu_add(items, &count, capacity, MENU_ACCUMULATE, 0, TRAY_CHECK, app.accumulates, "Accumulate");
    menu_add(items, &count, capacity, MENU_MELT, 0, TRAY_CHECK, app.thaws, "Melt");
    menu_add(items, &count, capacity, MENU_FULLSCREEN, 0, TRAY_CHECK, app.hideFullscreen,
             "No snow over fullscreen");
    menu_add(items, &count, capacity, MENU_SEPARATOR, 0, TRAY_SEPARATOR, 0, "");
    menu_add(items, &count, capacity, MENU_CLEAR, 0, TRAY_PLAIN, 0, "Clear snow");
    menu_add(items, &count, capacity, MENU_QUIT, 0, TRAY_PLAIN, 0, "Quit Snowhush");
    return count;
}

static void clear_snow(void)
{
    for (int i = 0; i < app.snow.outputCount; i++) {
        Output *o = app.snow.outputs[i];
        if (!o->sim) continue;
        sim_blow_away(o->sim, sim_breeze_direction(o->sim), 0);
    }
}

static void tray_menu_event(int id)
{
    if (id == -1) {
        /* A click on the icon: pause the snowfall, or bring it back. */
        app.snowfall = app.snowfall == SNOWFALL_OFF ? SNOWFALL_STEADY : SNOWFALL_OFF;
        apply_settings();
    } else if (id >= MENU_OFF && id <= MENU_VARIABLE) {
        static const Snowfall values[] = {SNOWFALL_OFF, SNOWFALL_LIGHT, SNOWFALL_STEADY, SNOWFALL_HEAVY,
                                          SNOWFALL_VARIABLE};
        app.snowfall = values[id - MENU_OFF];
        apply_settings();
    } else if (id >= MENU_MONITOR_BASE && id < MENU_MONITOR_BASE + 16) {
        int index = id - MENU_MONITOR_BASE;
        if (index < trayMonitorCount && trayMonitorNames[index][0])
            monitor_set_enabled(trayMonitorNames[index], !monitor_enabled(trayMonitorNames[index]));
    } else if (id == MENU_WINDOWS) {
        app.snowOnWindows = !app.snowOnWindows;
    } else if (id == MENU_ACCUMULATE) {
        app.accumulates = !app.accumulates;
        apply_settings();
    } else if (id == MENU_MELT) {
        app.thaws = !app.thaws;
        apply_settings();
    } else if (id == MENU_FULLSCREEN) {
        app.hideFullscreen = !app.hideFullscreen;
    } else if (id == MENU_CLEAR) {
        clear_snow();
    } else if (id == MENU_QUIT) {
        app.running = 0;
    } else {
        return;
    }
    config_save();
    tray_refresh(app.tray);
}

static void control_open(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) runtime = "/tmp";
    snprintf(app.controlPath, sizeof(app.controlPath), "%s/snowhush-%d.sock", runtime, (int)getpid());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", app.controlPath);
    unlink(app.controlPath);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    listen(fd, 8);
    app.controlFd = fd;

    char link[512], target[512];
    config_path(link, sizeof(link), "last-socket");
    char directory[512];
    snprintf(directory, sizeof(directory), "%s", link);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = 0;
        mkdir(directory, 0755);
    }
    snprintf(target, sizeof(target), "%s\n", app.controlPath);
    FILE *file = fopen(link, "w");
    if (file) {
        fputs(target, file);
        fclose(file);
    }
}

static void handle_control(void)
{
    if (app.controlFd < 0) return;
    int client = accept(app.controlFd, NULL, NULL);
    if (client < 0) return;
    char request[512];
    ssize_t n = read(client, request, sizeof(request) - 1);
    if (n <= 0) {
        close(client);
        return;
    }
    request[n] = 0;
    char *newline = strchr(request, '\n');
    if (newline) *newline = 0;

    char reply[512] = "";
    char *command = strtok(request, " ");
    if (!command) {
        snprintf(reply, sizeof(reply), "error: empty command\n");
    } else if (strcmp(command, "snowfall") == 0) {
        char *value = strtok(NULL, " ");
        if (value) {
            app.snowfall = parse_snowfall(value);
            apply_settings();
            config_save();
        }
        snprintf(reply, sizeof(reply), "snowfall %s\n", snowfall_name(app.snowfall));
    } else if (strcmp(command, "accumulate") == 0) {
        char *value = strtok(NULL, " ");
        app.accumulates = parse_onoff(value, !app.accumulates);
        apply_settings();
        config_save();
        snprintf(reply, sizeof(reply), "accumulate %s\n", app.accumulates ? "on" : "off");
    } else if (strcmp(command, "melt") == 0) {
        char *value = strtok(NULL, " ");
        app.thaws = parse_onoff(value, !app.thaws);
        apply_settings();
        config_save();
        snprintf(reply, sizeof(reply), "melt %s\n", app.thaws ? "on" : "off");
    } else if (strcmp(command, "windows") == 0 || strcmp(command, "snow_on_windows") == 0) {
        char *value = strtok(NULL, " ");
        app.snowOnWindows = parse_onoff(value, !app.snowOnWindows);
        config_save();
        snprintf(reply, sizeof(reply), "snow_on_windows %s\n", app.snowOnWindows ? "on" : "off");
    } else if (strcmp(command, "behind") == 0) {
        char *value = strtok(NULL, " ");
        app.behindWindows = parse_onoff(value, !app.behindWindows);
        config_save();
        wl_set_layer(&app.snow, app.behindWindows ? 0 : 3);
        snprintf(reply, sizeof(reply), "behind_windows %s\n", app.behindWindows ? "on" : "off");
    } else if (strcmp(command, "fullscreen") == 0) {
        char *value = strtok(NULL, " ");
        app.hideFullscreen = parse_onoff(value, !app.hideFullscreen);
        config_save();
        tray_refresh(app.tray);
        snprintf(reply, sizeof(reply), "hide_fullscreen %s\n", app.hideFullscreen ? "on" : "off");
    } else if (strcmp(command, "monitors") == 0) {
        char *value = strtok(NULL, " "); /* "all" clears the list */
        if (value && strcmp(value, "all") == 0) {
            app.monitorsOff[0] = 0;
            config_save();
            tray_refresh(app.tray);
        }
        int used = 0;
        for (int i = 0; i < app.state.monitorCount && used < (int)sizeof(reply) - 80; i++) {
            const HyprMonitor *monitor = &app.state.monitors[i];
            used += snprintf(reply + used, sizeof(reply) - (size_t)used, "%s %s\n", monitor->name,
                             monitor_enabled(monitor->name) ? "on" : "off");
        }
        if (!used) snprintf(reply, sizeof(reply), "no outputs\n");
    } else if (strcmp(command, "monitor") == 0) {
        char *name = strtok(NULL, " ");
        char *value = strtok(NULL, " ");
        if (!name) {
            snprintf(reply, sizeof(reply), "error: which output?\n");
        } else if (!value) {
            snprintf(reply, sizeof(reply), "%s %s\n", name, monitor_enabled(name) ? "on" : "off");
        } else {
            monitor_set_enabled(name, parse_onoff(value, !monitor_enabled(name)));
            config_save();
            tray_refresh(app.tray);
            snprintf(reply, sizeof(reply), "%s %s\n", name, monitor_enabled(name) ? "on" : "off");
        }
    } else if (strcmp(command, "clear") == 0) {
        int calm = 0;
        for (int i = 0; i < app.snow.outputCount; i++) {
            Output *o = app.snow.outputs[i];
            if (!o->sim) continue;
            sim_blow_away(o->sim, sim_breeze_direction(o->sim), calm);
        }
        snprintf(reply, sizeof(reply), "cleared\n");
    } else if (strcmp(command, "quit") == 0) {
        snprintf(reply, sizeof(reply), "bye\n");
        app.running = 0;
    } else if (strcmp(command, "status") == 0) {
        snprintf(reply, sizeof(reply),
                 "snowfall %s\naccumulate %s\nmelt %s\nsnow_on_windows %s\nbehind_windows %s\n"
                 "hide_fullscreen %s\nmonitors_off %s\nfps %d\n",
                 snowfall_name(app.snowfall), app.accumulates ? "on" : "off", app.thaws ? "on" : "off",
                 app.snowOnWindows ? "on" : "off", app.behindWindows ? "on" : "off",
                 app.hideFullscreen ? "on" : "off", app.monitorsOff[0] ? app.monitorsOff : "-", app.maxFps);
    } else if (strcmp(command, "surfaces") == 0) {
        static char dump[8192];
        dump[0] = 0;
        int used = 0;
        for (int i = 0; i < app.snow.outputCount && used < (int)sizeof(dump) - 1200; i++) {
            Output *o = app.snow.outputs[i];
            HyprMonitor *monitor = NULL;
            int monitorIndex = -1;
            for (int k = 0; k < app.state.monitorCount; k++) {
                if (strcmp(app.state.monitors[k].name, o->name) == 0) {
                    monitor = &app.state.monitors[k];
                    monitorIndex = k;
                }
            }
            if (!monitor) {
                used += snprintf(dump + used, sizeof(dump) - (size_t)used, "%s: no monitor\n", o->name);
                continue;
            }
            static WinSurface surfaces[MAX_SURFACES];
            IdSet elsewhere = {0}, parked = {0};
            int surfaceCount = 0;
            build_surfaces(&app.state, monitor, monitorIndex, surfaces, &surfaceCount, &elsewhere, &parked);
            used += snprintf(dump + used, sizeof(dump) - (size_t)used, "%s (%dx%d) inset %.0f surfaces %d\n",
                             o->name, monitor->w, monitor->h, monitor_top_inset(&app.state, monitor), surfaceCount);
            for (int s = 0; s < surfaceCount && used < (int)sizeof(dump) - 200; s++)
                used += snprintf(dump + used, sizeof(dump) - (size_t)used,
                                 "  window %llx at %.0f,%.0f %.0fx%.0f radius %.0f occluders %d\n",
                                 (unsigned long long)surfaces[s].id, surfaces[s].x, surfaces[s].y, surfaces[s].w,
                                 surfaces[s].h, surfaces[s].cornerRadius, surfaces[s].cornerCount);
            idset_free(&elsewhere);
            idset_free(&parked);
            if (o->sim && used < (int)sizeof(dump) - 1200) {
                sim_debug_dump(o->sim, dump + used, (int)sizeof(dump) - used);
                used += (int)strlen(dump + used);
            }
        }
        snprintf(reply, sizeof(reply), "see surfaces\n");
        (void)write(client, reply, strlen(reply));
        (void)write(client, dump, strlen(dump));
        close(client);
        return;
    } else {
        snprintf(reply, sizeof(reply), "error: unknown command '%s'\n", command);
    }
    (void)write(client, reply, strlen(reply));
    close(client);
}

/* ------------------------------------------------------------------ */
/* scene light                                                         */

static double lightUpdatedAt = -1e9;
static float sceneLight[3] = {1, 1, 1};
static float sceneShadow[4] = {0, 0, 0, 0};

static void scene_daylight(float out[3])
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    float hour = (float)local.tm_hour + (float)local.tm_min / 60.0f;
    float day = main_smoothstep(6, 8.5f, hour) * (1 - main_smoothstep(17.5f, 20.5f, hour));
    float golden = fminf(1.0f, expf(-main_sqrf((hour - 7.6f) / 1.1f)) + expf(-main_sqrf((hour - 18.6f) / 1.2f)));
    const float night[3] = {0.86f, 0.89f, 0.97f};
    const float warm[3] = {1.0f, 0.965f, 0.915f};
    for (int k = 0; k < 3; k++) {
        float base = night[k] + (1 - night[k]) * day;
        out[k] = base + (warm[k] - base) * golden * 0.55f;
    }
}

/* The light the snow is seen in leans toward the desktop picture along the
 * bottom of the screen, as in the original. */
static void wallpaper_tint(float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    static time_t cachedStamp = 0;
    static float cached[4] = {0, 0, 0, 0};
    static char cachedPath[512] = "";
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/hypr/wallpaper_effects/.wallpaper_current", home);
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (st.st_mtime == cachedStamp && strcmp(path, cachedPath) == 0) {
        memcpy(out, cached, sizeof(cached));
        return;
    }
    if (!sample_image_tint(path, cached)) return;
    cachedStamp = st.st_mtime;
    snprintf(cachedPath, sizeof(cachedPath), "%s", path);
    memcpy(out, cached, sizeof(cached));
}

static void update_light(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    if (now - lightUpdatedAt < 60) return;
    lightUpdatedAt = now;
    scene_daylight(sceneLight);
    wallpaper_tint(sceneShadow);
}

/* ------------------------------------------------------------------ */
/* window tracking                                                     */

static float monitor_top_inset(const HyprState *state, const HyprMonitor *monitor)
{
    float inset = 0;
    for (int i = 0; i < state->layerCount; i++) {
        const HyprLayer *layer = &state->layers[i];
        if (layer->w < monitor->w / 2) continue;
        if (layer->y > monitor->y + 2) continue;
        float bottom = (float)(layer->y + layer->h - monitor->y);
        if (bottom > inset && bottom < monitor->h / 3.0f) inset = bottom;
    }
    return inset;
}

static int client_visible_on(const HyprClient *client, const HyprMonitor *monitor)
{
    if (!client->mapped || client->hidden || !client->visible) return 0;
    if (client->monitor < 0) return 0;
    if (client->workspaceId != monitor->activeWorkspaceId && client->workspaceId != monitor->specialWorkspaceId)
        return 0;
    return 1;
}

/* Hyprland reports 0 none, 1 maximised, 2 fullscreen. Only the last one takes
 * the whole screen, so only that one hides the snow. */
static int fullscreen_on(const HyprState *state, const HyprMonitor *monitor)
{
    for (int i = 0; i < state->clientCount; i++) {
        const HyprClient *client = &state->clients[i];
        if (client->fullscreen < 2) continue;
        if (!client_visible_on(client, monitor)) continue;
        if (client->monitor >= state->monitorCount) continue;
        if (strcmp(state->monitors[client->monitor].name, monitor->name) == 0) return 1;
    }
    return 0;
}

/* Hyprland paints windows in two passes: every tiled window in client-list
 * order, then every floating one on top, also in client-list order (see
 * IHyprRenderer::renderWorkspaceWindows). `hyprctl clients` returns the raw
 * z-list, so a tiled window can appear after a floating one without being in
 * front of it. Occlusion has to follow what is actually painted, otherwise the
 * snow on a floating window is clipped away by a tiled window underneath it. */
static int layer_order(const HyprState *state, int monitorIndex, const HyprMonitor *monitor, int *order,
                       int capacity)
{
    int count = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < state->clientCount && count < capacity; i++) {
            const HyprClient *client = &state->clients[i];
            if (client->monitor != monitorIndex) continue;
            if (client->floating != pass) continue;
            if (!client_visible_on(client, monitor)) continue;
            if (client->fullscreen != 0) continue;
            order[count++] = i;
        }
    }
    return count;
}

static void build_surfaces(const HyprState *state, const HyprMonitor *monitor, int monitorIndex,
                           WinSurface *surfaces, int *outCount, IdSet *elsewhere, IdSet *parked)
{
    int count = 0;
    float topInset = monitor_top_inset(state, monitor);
    /* Mirrors the reference: a window that fills the working area carries no snow,
     * because it leaves no sky above it and its top edge sits at the screen edge. */
    double monitorArea = (double)monitor->w * ((double)monitor->h - (double)topInset);

    static int order[512];
    int orderCount = layer_order(state, monitorIndex, monitor, order, 512);

    for (int i = 0; i < state->clientCount; i++) {
        const HyprClient *client = &state->clients[i];
        if (client->monitor != monitorIndex) continue;
        if (!client_visible_on(client, monitor)) {
            /* Out of sight on purpose: on another workspace, kept for later. */
            if (client->mapped && !client->hidden && client->fullscreen == 0) idset_add(elsewhere, client->id);
            continue;
        }
        if (client->fullscreen != 0) {
            idset_add(elsewhere, client->id);
        }
    }

    for (int p = 0; p < orderCount && count < MAX_SURFACES; p++) {
        const HyprClient *client = &state->clients[order[p]];
        if (client->w < 80 || client->h < 40) continue;
        if ((double)client->w * (double)client->h >= monitorArea * 0.9) continue;

        float localX = (float)(client->x - monitor->x);
        float top = (float)(client->y - monitor->y);
        if (top < topInset + 6) continue;
        if (top < 6) continue;
        if (localX + client->w <= 0 || localX >= monitor->w) continue;

        float overlap = (float)client->w;
        if (localX < 0) overlap += localX;
        if (localX + client->w > monitor->w) overlap -= localX + client->w - monitor->w;
        if (overlap < 80) continue;

        WinSurface *surface = &surfaces[count++];
        memset(surface, 0, sizeof(*surface));
        surface->id = client->id;
        surface->x = localX;
        surface->y = (float)(monitor->h - (top + client->h));
        surface->w = (float)client->w;
        surface->h = (float)client->h;
        /* Search bars and other wide, shallow windows are capsules: their ends are
         * fully rounded, so the snow has to follow a half circle rather than a corner. */
        int capsule = client->h <= 90 && client->w >= client->h * 3;
        surface->cornerRadius = capsule ? (float)client->h / 2
                                        : (float)(state->rounding > 0 ? state->rounding : 0);
        if (surface->cornerRadius > surface->w / 2) surface->cornerRadius = surface->w / 2;
        if (surface->cornerRadius > surface->h / 2) surface->cornerRadius = surface->h / 2;
        surface->cornerExponent = 2;
        surface->isDock = 0;

        /* Windows painted after this one hide the snow behind them. */
        float regionTop = surface->y + surface->h;
        float bandLow = regionTop - surface->cornerRadius - 2;
        float bandHigh = regionTop + 40;
        for (int q = p + 1; q < orderCount && surface->cornerCount < MAX_OCCLUDERS; q++) {
            const HyprClient *front = &state->clients[order[q]];
            float fx = (float)(front->x - monitor->x);
            float fy = (float)(monitor->h - (front->y - monitor->y + front->h));
            float fw = (float)front->w;
            float fh = (float)front->h;
            if (fy > bandHigh || fy + fh < bandLow) continue;
            if (fx + fw <= localX || fx >= localX + surface->w) continue;
            surface->corners[surface->cornerCount][0] = fx;
            surface->corners[surface->cornerCount][1] = fy;
            surface->corners[surface->cornerCount][2] = fx + fw;
            surface->corners[surface->cornerCount][3] = fy + fh;
            surface->cornerCount++;
        }
    }

    /* Windows that are hidden (minimised, on a hidden special workspace) lose their snow. */
    for (int i = 0; i < state->clientCount; i++) {
        const HyprClient *client = &state->clients[i];
        if (client->monitor != monitorIndex) continue;
        if (client->mapped && (client->hidden || !client->visible) && client->fullscreen == 0)
            idset_add(parked, client->id);
    }

    *outCount = count;
}

static uint64_t hash_state(const HyprState *state)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < state->clientCount; i++) {
        const HyprClient *c = &state->clients[i];
        h = (h ^ c->id) * 1099511628211ull;
        h = (h ^ (uint64_t)(c->x * 31 + c->y)) * 1099511628211ull;
        h = (h ^ (uint64_t)(c->w * 17 + c->h)) * 1099511628211ull;
        h = (h ^ (uint64_t)(c->hidden | (c->visible << 1) | (c->fullscreen << 2))) * 1099511628211ull;
    }
    for (int i = 0; i < state->monitorCount; i++) {
        h = (h ^ (uint64_t)(state->monitors[i].activeWorkspaceId * 7 + state->monitors[i].x + 3 * state->monitors[i].y)) *
            1099511628211ull;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* outputs                                                             */

static void output_ready(Output *o)
{
    fprintf(stderr, "snowhush: output %s %dx%d\n", o->name, o->width, o->height);
    if (!o->sim) {
        o->sim = sim_create((float)o->width, (float)o->height, app.snowfall);
        sim_set_accumulates(o->sim, app.accumulates);
        sim_set_thaws(o->sim, app.thaws);
        sim_set_walls(o->sim, 1, 1);
    } else {
        sim_resize(o->sim, (float)o->width, (float)o->height);
    }
}

static void output_gone(Output *o)
{
    (void)o;
}

static void update_walls(Output *o)
{
    HyprMonitor *monitor = NULL;
    for (int i = 0; i < app.state.monitorCount; i++)
        if (strcmp(app.state.monitors[i].name, o->name) == 0) monitor = &app.state.monitors[i];
    if (!monitor) return;
    int left = 1, right = 1;
    for (int i = 0; i < app.state.monitorCount; i++) {
        HyprMonitor *other = &app.state.monitors[i];
        if (other == monitor) continue;
        if (abs(other->x + other->w - monitor->x) < 1) left = 0;
        if (abs(other->x - (monitor->x + monitor->w)) < 1) right = 0;
        if (abs(other->y - monitor->y) >= monitor->h) continue;
    }
    sim_set_walls(o->sim, left, right);
}

static int draw_output(Output *o, double now, float dt, int changedAny)
{
    (void)changedAny;
    if (!o->configured || !o->width || !o->height) return 0;

    HyprMonitor *monitor = NULL;
    int monitorIndex = -1;
    for (int i = 0; i < app.state.monitorCount; i++) {
        if (strcmp(app.state.monitors[i].name, o->name) == 0) {
            monitor = &app.state.monitors[i];
            monitorIndex = i;
        }
    }

    int surfaceCount = 0;
    IdSet elsewhere = {0}, parked = {0};
    static WinSurface surfaces[MAX_SURFACES];
    if (monitor) {
        build_surfaces(&app.state, monitor, monitorIndex, surfaces, &surfaceCount, &elsewhere, &parked);
        update_walls(o);
    }

    if (!o->sim) {
        idset_free(&elsewhere);
        idset_free(&parked);
        return 0;
    }

    /* Snow is hidden on outputs the user switched off, and while a window there
     * is fullscreen: an app that wants the whole screen should get it. */
    int muted = 0;
    if (monitor) {
        if (!monitor_enabled(o->name)) muted = 1;
        else if (app.hideFullscreen && fullscreen_on(&app.state, monitor)) muted = 1;
    }
    if (muted) {
        if (!o->muted) {
            o->muted = 1;
            /* One empty frame, so the compositor stops showing the last one. */
            if (o->drawn && wl_begin_frame(&app.snow, o, o->width, o->height)) {
                if (!app.renderer) app.renderer = renderer_new();
                if (app.renderer)
                    renderer_draw(app.renderer, o->width, o->height, 1.0f, 0.0f, NULL, 0, NULL, 0, NULL, 0,
                                  sceneLight, sceneShadow);
                wl_end_frame(&app.snow, o);
            }
            o->drawn = 0;
        }
        idset_free(&elsewhere);
        idset_free(&parked);
        return 0;
    }
    o->muted = 0;

    float interval = (float)fmax(now - app.lastPoll, 1.0 / 240);
    if (interval > 0.25f) interval = 0.25f;
    sim_update_windows(o->sim, surfaces, surfaceCount, interval, &parked, &elsewhere, app.snowOnWindows);

    int hasCursor = 0;
    float cx = 0, cy = 0;
    double gx, gy;
    if (monitor && hypr_cursor(&gx, &gy)) {
        hasCursor = 1;
        cx = (float)(gx - monitor->x);
        cy = (float)(o->height - (gy - monitor->y));
    }
    int changed = sim_step(o->sim, dt, hasCursor, cx, cy);

    static Sprite sprites[20000];
    int spriteCount = sim_sprite_count(o->sim);
    if (spriteCount > (int)(sizeof(sprites) / sizeof(sprites[0])))
        spriteCount = (int)(sizeof(sprites) / sizeof(sprites[0]));
    spriteCount = sim_write_sprites(o->sim, sprites, spriteCount);

    int columnCount = sim_column_count(o->sim);
    static float *heights = NULL;
    static int heightsCapacity = 0;
    if (columnCount > heightsCapacity) {
        heightsCapacity = columnCount * 2 + 256;
        heights = realloc(heights, sizeof(float) * (size_t)heightsCapacity);
    }
    static PileLayer piles[64];
    int pileCount = sim_write_layers(o->sim, heights, heightsCapacity, piles, 64);

    int idle = !changed && spriteCount == 0 && !sim_has_snow(o->sim);
    if (!idle || !o->drawn) {
        if (wl_begin_frame(&app.snow, o, o->width, o->height)) {
            if (!app.renderer) app.renderer = renderer_new();
            if (!app.renderer) return 0;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            float time = (float)((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
            renderer_draw(app.renderer, o->width, o->height, 1.0f, fmodf(time, 1000.0f),
                          sprites, spriteCount, heights, columnCount, piles, pileCount,
                          sceneLight, sceneShadow);
            wl_end_frame(&app.snow, o);
        }
    }
    o->drawn = 1;

    idset_free(&elsewhere);
    idset_free(&parked);
    return changed;
}

/* ------------------------------------------------------------------ */
/* loop                                                                */

static double monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int next_interval(void)
{
    Pace pace = PACE_CALM;
    for (int i = 0; i < app.snow.outputCount; i++) {
        Output *o = app.snow.outputs[i];
        if (!o->sim) continue;
        Pace p = sim_pace(o->sim);
        if (p > pace) pace = p;
    }
    int fps = pace == PACE_CALM ? 30 : (pace == PACE_LIVELY ? 60 : app.maxFps);
    if (fps > app.maxFps) fps = app.maxFps;
    if (fps < 10) fps = 10;
    return fps;
}

static void run_loop(void)
{
    double last = monotonic();
    double deadline = last + 1.0 / 60.0;

    while (app.running) {
        double now = monotonic();
        int timeout = (int)((deadline - now) * 1000.0);
        if (timeout < 0) timeout = 0;
        int ready = wl_wait(&app.snow, app.controlFd, timeout);
        if (ready & 2) handle_control();
        tray_poll(app.tray);

        now = monotonic();
        if (now < deadline) continue;

        float dt = (float)(now - last);
        if (dt > 0.05f) dt = 0.05f;
        if (dt <= 0) dt = 1.0f / 60;

        /* Windows and the pointer change less often than the snow needs drawing. */
        if (now - app.pollAt >= (now - app.lastWindowChange < 0.6 ? 1.0 / 62.0 : 0.2)) {
            hypr_state_free(&app.state);
            if (hypr_refresh(&app.state)) {
                uint64_t hash = hash_state(&app.state);
                if (hash != app.stateHash) {
                    app.stateHash = hash;
                    app.lastWindowChange = now;
                }
            }
            app.pollAt = now;
        }
        update_light();

        for (int i = 0; i < app.snow.outputCount; i++) draw_output(app.snow.outputs[i], now, dt, 0);

        app.lastPoll = now;
        last = now;
        deadline = now + 1.0 / (double)next_interval();
        if (wl_flush(&app.snow) < 0) break;
    }
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */

static int control_client(const char *command)
{
    char path[512], socketBuffer[512];
    const char *home = getenv("HOME");
    const char *config = getenv("XDG_CONFIG_HOME");
    if (config && *config) snprintf(path, sizeof(path), "%s/snowhush/last-socket", config);
    else snprintf(path, sizeof(path), "%s/.config/snowhush/last-socket", home ? home : "");
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "snowhush: not running\n");
        return 1;
    }
    if (!fgets(socketBuffer, sizeof(socketBuffer), file)) {
        fclose(file);
        fprintf(stderr, "snowhush: not running\n");
        return 1;
    }
    fclose(file);
    char *newline = strchr(socketBuffer, '\n');
    if (newline) *newline = 0;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socketBuffer);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "snowhush: not running\n");
        close(fd);
        return 1;
    }
    (void)write(fd, command, strlen(command));
    char reply[1024];
    ssize_t n;
    while ((n = read(fd, reply, sizeof(reply) - 1)) > 0) {
        reply[n] = 0;
        fputs(reply, stdout);
    }
    close(fd);
    return 0;
}

static void on_signal(int signalNumber)
{
    (void)signalNumber;
    app.running = 0;
}

int main(int argc, char *argv[])
{
    app.snowfall = SNOWFALL_STEADY;
    app.accumulates = 1;
    app.thaws = 1;
    app.snowOnWindows = 1;
    app.behindWindows = 0;
    app.hideFullscreen = 1;
    app.monitorsOff[0] = 0;
    app.maxFps = 60;
    app.controlFd = -1;
    app.running = 1;
    config_load();

    if (argc > 1 && strcmp(argv[1], "ctl") == 0) {
        char command[512] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(command, " ", sizeof(command) - strlen(command) - 1);
            strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
        }
        return control_client(command);
    }
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("snowhush for Hyprland\n\n"
               "  snowhush                 run\n"
               "  snowhush ctl <command>   talk to a running instance\n\n"
               "commands:\n"
               "  snowfall off|light|steady|heavy|variable\n"
               "  accumulate on|off\n"
               "  melt on|off\n"
               "  windows on|off        snow on windows\n"
               "  behind on|off         behind windows instead of in front\n"
               "  monitors [all]       which outputs get snow\n"
               "  monitor <name> on|off\n"
               "  fullscreen on|off    no snow over a fullscreen window\n"
               "  clear                 blow the snow away\n"
               "  status                show settings\n"
               "  quit                  stop\n");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--behind") == 0) app.behindWindows = 1;
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) app.maxFps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--snowfall") == 0 && i + 1 < argc) app.snowfall = parse_snowfall(argv[++i]);
    }

    if (!hypr_init()) return 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    app.snow.on_output_ready = output_ready;
    app.snow.on_output_gone = output_gone;
    app.snow.layerLevel = 3;
    if (!wl_start(&app.snow, app.behindWindows)) return 1;

    control_open();
    app.tray = tray_start(&trayCallbacks);
    app.pollAt = 0;
    app.lastWindowChange = 0;
    run_loop();

    tray_stop(app.tray);
    if (app.controlFd >= 0) {
        unlink(app.controlPath);
        close(app.controlFd);
    }
    renderer_free(app.renderer);
    hypr_state_free(&app.state);
    wl_shutdown(&app.snow);
    return 0;
}
