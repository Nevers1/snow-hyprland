#include "tray.h"

#ifdef SNOW_NO_TRAY
/* Built without libsystemd: the app still runs, just without a tray icon. */
Tray *tray_start(const TrayCallbacks *callbacks)
{
    (void)callbacks;
    return NULL;
}
void tray_stop(Tray *tray) { (void)tray; }
void tray_poll(Tray *tray) { (void)tray; }
void tray_refresh(Tray *tray) { (void)tray; }
#else

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define SNI_PATH "/StatusNotifierItem"
#define SNI_IFACE "org.kde.StatusNotifierItem"
#define MENU_PATH "/MenuBar"
#define MENU_IFACE "com.canonical.dbusmenu"
#define WATCHER_SERVICE "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"

#define ICON_SIZES 5

struct Tray {
    sd_bus *bus;
    const TrayCallbacks *callbacks;
    char serviceName[128];
    uint32_t revision;
    int registered;
    double retryAt;

    int iconCount;
    struct {
        int size;
        unsigned char *pixels;
    } icons[ICON_SIZES];
};

/* ------------------------------------------------------------------ */
/* icon                                                                */

static double edge_distance(double px, double py, double ax, double ay, double bx, double by)
{
    double vx = bx - ax, vy = by - ay;
    double wx = px - ax, wy = py - ay;
    double lengthSquared = vx * vx + vy * vy;
    double t = lengthSquared > 0 ? (wx * vx + wy * vy) / lengthSquared : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double dx = px - (ax + vx * t), dy = py - (ay + vy * t);
    return sqrt(dx * dx + dy * dy);
}

/* A six-armed flake with feathery branches, drawn as a signed distance field
 * in [-1, 1] so one routine serves every icon size. */
static int flake_inside(double u, double v)
{
    double r = sqrt(u * u + v * v);
    if (r > 1.05) return 0;
    double best = r - 0.17; /* the hub */
    const double thickness = 0.125;
    for (int k = 0; k < 6; k++) {
        double angle = (double)k * M_PI / 3.0;
        double ca = cos(angle), sa = sin(angle);
        double arm = edge_distance(u, v, 0, 0, ca * 0.94, sa * 0.94) - thickness * 0.5;
        if (arm < best) best = arm;
        for (int b = 0; b < 2; b++) {
            double t = b == 0 ? 0.50 : 0.78;
            double length = b == 0 ? 0.32 : 0.21;
            double bx = ca * t, by = sa * t;
            for (int s = 0; s < 2; s++) {
                double branchAngle = angle + (s ? -0.62 : 0.62);
                double tip = edge_distance(u, v, bx, by, bx + cos(branchAngle) * length,
                                           by + sin(branchAngle) * length) -
                             thickness * 0.34;
                if (tip < best) best = tip;
            }
        }
    }
    return best <= 0;
}

static unsigned char *make_icon(int size)
{
    unsigned char *pixels = malloc((size_t)size * (size_t)size * 4);
    if (!pixels) return NULL;
    const int samples = 4;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int hits = 0;
            for (int sy = 0; sy < samples; sy++) {
                for (int sx = 0; sx < samples; sx++) {
                    double px = ((double)x + ((double)sx + 0.5) / samples) / size * 2.0 - 1.0;
                    double py = ((double)y + ((double)sy + 0.5) / samples) / size * 2.0 - 1.0;
                    if (flake_inside(px, py)) hits++;
                }
            }
            double coverage = (double)hits / (samples * samples);
            int alpha = (int)(coverage * 255.0 + 0.5);
            unsigned char *p = pixels + (((size_t)y * size + x) * 4);
            p[0] = (unsigned char)alpha; /* A */
            p[1] = 255;                  /* R */
            p[2] = 255;                  /* G */
            p[3] = 255;                  /* B */
        }
    }
    return pixels;
}

static void icons_create(Tray *tray)
{
    static const int sizes[ICON_SIZES] = {16, 22, 24, 32, 48};
    for (int i = 0; i < ICON_SIZES; i++) {
        unsigned char *pixels = make_icon(sizes[i]);
        if (!pixels) break;
        tray->icons[tray->iconCount].size = sizes[i];
        tray->icons[tray->iconCount].pixels = pixels;
        tray->iconCount++;
    }
}

/* ------------------------------------------------------------------ */
/* StatusNotifierItem                                                  */

static int sni_get_category(sd_bus *bus, const char *path, const char *iface, const char *property,
                            sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)iface; (void)property; (void)userdata; (void)error;
    return sd_bus_message_append(reply, "s", "ApplicationStatus");
}

static int sni_get_id(sd_bus *bus, const char *path, const char *iface, const char *property,
                      sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)iface; (void)property; (void)userdata; (void)error;
    return sd_bus_message_append(reply, "s", "snowhush");
}

static int sni_get_title(sd_bus *bus, const char *path, const char *iface, const char *property,
                         sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)bus; (void)path; (void)iface; (void)property; (void)error;
    return sd_bus_message_append(reply, "s", tray->callbacks->title ? tray->callbacks->title : "Snowhush");
}

static int sni_get_status(sd_bus *bus, const char *path, const char *iface, const char *property,
                          sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)iface; (void)property; (void)userdata; (void)error;
    return sd_bus_message_append(reply, "s", "Active");
}

static int sni_get_menu(sd_bus *bus, const char *path, const char *iface, const char *property,
                        sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)iface; (void)property; (void)userdata; (void)error;
    return sd_bus_message_append(reply, "o", MENU_PATH);
}

static int sni_get_icon_name(sd_bus *bus, const char *path, const char *iface, const char *property,
                             sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)iface; (void)property; (void)userdata; (void)error;
    return sd_bus_message_append(reply, "s", "snowhush");
}

static int sni_get_item_is_menu(sd_bus *bus, const char *path, const char *iface, const char *property,
                                sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)iface; (void)property; (void)userdata; (void)error;
    /* The host draws the menu itself, from the dbusmenu service at /MenuBar. */
    return sd_bus_message_append(reply, "b", 1);
}

static int sni_get_icon_pixmap(sd_bus *bus, const char *path, const char *iface, const char *property,
                               sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)bus; (void)path; (void)iface; (void)property; (void)error;
    int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
    if (r < 0) return r;
    for (int i = 0; i < tray->iconCount; i++) {
        int size = tray->icons[i].size;
        r = sd_bus_message_open_container(reply, 'r', "iiay");
        if (r < 0) return r;
        r = sd_bus_message_append(reply, "ii", size, size);
        if (r < 0) return r;
        r = sd_bus_message_append_array(reply, 'y', tray->icons[i].pixels, (size_t)size * (size_t)size * 4);
        if (r < 0) return r;
        r = sd_bus_message_close_container(reply);
        if (r < 0) return r;
    }
    return sd_bus_message_close_container(reply);
}

static int sni_activate(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)error;
    sd_bus_message_skip(message, "ii");
    if (tray->callbacks->event) tray->callbacks->event(-1);
    return sd_bus_reply_method_return(message, NULL);
}

static int sni_context_menu(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    sd_bus_message_skip(message, "ii");
    /* The host owns the menu window; nothing to do but acknowledge. */
    return sd_bus_reply_method_return(message, NULL);
}

static int sni_scroll(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    sd_bus_message_skip(message, "is");
    return sd_bus_reply_method_return(message, NULL);
}

static const sd_bus_vtable sni_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Category", "s", sni_get_category, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Id", "s", sni_get_id, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Title", "s", sni_get_title, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", sni_get_status, 0, 0),
    SD_BUS_PROPERTY("Menu", "o", sni_get_menu, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ItemIsMenu", "b", sni_get_item_is_menu, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconName", "s", sni_get_icon_name, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconPixmap", "a(iiay)", sni_get_icon_pixmap, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AttentionIconName", "s", sni_get_icon_name, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Activate", "ii", "", sni_activate, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SecondaryActivate", "ii", "", sni_activate, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ContextMenu", "ii", "", sni_context_menu, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Scroll", "is", "", sni_scroll, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

/* ------------------------------------------------------------------ */
/* menu                                                                */

static int put_string(sd_bus_message *m, const char *key, const char *value)
{
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) return r;
    if ((r = sd_bus_message_append(m, "s", key)) < 0) return r;
    if ((r = sd_bus_message_open_container(m, 'v', "s")) < 0) return r;
    if ((r = sd_bus_message_append(m, "s", value)) < 0) return r;
    if ((r = sd_bus_message_close_container(m)) < 0) return r;
    return sd_bus_message_close_container(m);
}

static int put_bool(sd_bus_message *m, const char *key, int value)
{
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) return r;
    if ((r = sd_bus_message_append(m, "s", key)) < 0) return r;
    if ((r = sd_bus_message_open_container(m, 'v', "b")) < 0) return r;
    if ((r = sd_bus_message_append(m, "b", value ? 1 : 0)) < 0) return r;
    if ((r = sd_bus_message_close_container(m)) < 0) return r;
    return sd_bus_message_close_container(m);
}

static const TrayItem *item_find(const TrayItem *items, int count, int id)
{
    for (int i = 0; i < count; i++)
        if (items[i].id == id) return &items[i];
    return NULL;
}

static int put_item_props(sd_bus_message *m, const TrayItem *item)
{
    int r;
    if ((r = put_string(m, "label", item->label ? item->label : "")) < 0) return r;
    if ((r = put_bool(m, "enabled", item->enabled)) < 0) return r;
    if ((r = put_bool(m, "visible", 1)) < 0) return r;
    switch (item->kind) {
    case TRAY_SEPARATOR:
        return put_string(m, "type", "separator");
    case TRAY_SUBMENU:
        if ((r = put_string(m, "children-display", "submenu")) < 0) return r;
        break;
    default:
        /* Toggles carry their state in the label (see main.c): hosts such as
         * Waybar draw no check mark of their own, and a hidden checkbox is
         * worse than no checkbox. */
        break;
    }
    return 0;
}

static int append_node(sd_bus_message *m, Tray *tray, const TrayItem *items, int count, int id, int depth)
{
    int r;
    if ((r = sd_bus_message_open_container(m, 'r', "ia{sv}av")) < 0) return r;
    if ((r = sd_bus_message_append(m, "i", id)) < 0) return r;
    if ((r = sd_bus_message_open_container(m, 'a', "{sv}")) < 0) return r;
    if (id == 0) {
        if ((r = put_string(m, "children-display", "submenu")) < 0) return r;
        if ((r = put_string(m, "label", tray->callbacks->title ? tray->callbacks->title : "Snowhush")) < 0) return r;
    } else {
        const TrayItem *item = item_find(items, count, id);
        if (item && (r = put_item_props(m, item)) < 0) return r;
    }
    if ((r = sd_bus_message_close_container(m)) < 0) return r;
    if ((r = sd_bus_message_open_container(m, 'a', "v")) < 0) return r;
    if (depth != 0) {
        for (int i = 0; i < count; i++) {
            if (items[i].parent != id) continue;
            if ((r = sd_bus_message_open_container(m, 'v', "(ia{sv}av)")) < 0) return r;
            if ((r = append_node(m, tray, items, count, items[i].id, depth - 1)) < 0) return r;
            if ((r = sd_bus_message_close_container(m)) < 0) return r;
        }
    }
    if ((r = sd_bus_message_close_container(m)) < 0) return r;
    return sd_bus_message_close_container(m);
}

static int build_items(Tray *tray, TrayItem *items)
{
    if (!tray->callbacks->build) return 0;
    return tray->callbacks->build(items, TRAY_MAX_ITEMS);
}

static int menu_get_layout(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)error;
    int32_t parent = 0, depth = -1;
    int r = sd_bus_message_read(message, "ii", &parent, &depth);
    if (r < 0) return r;
    sd_bus_message_skip(message, "as");

    TrayItem items[TRAY_MAX_ITEMS];
    int count = build_items(tray, items);

    sd_bus_message *reply = NULL;
    if ((r = sd_bus_message_new_method_return(message, &reply)) < 0) return r;
    if ((r = sd_bus_message_append(reply, "u", tray->revision)) < 0) goto fail;
    if ((r = append_node(reply, tray, items, count, parent, depth)) < 0) goto fail;
    return sd_bus_send(NULL, reply, NULL);
fail:
    sd_bus_message_unref(reply);
    return r;
}

static int append_properties(sd_bus_message *m, Tray *tray, int id, const char *const *names, int nameCount)
{
    TrayItem items[TRAY_MAX_ITEMS];
    int count = build_items(tray, items);
    const TrayItem *item = id == 0 ? NULL : item_find(items, count, id);

    int r = sd_bus_message_open_container(m, 'r', "ia{sv}");
    if (r < 0) return r;
    if ((r = sd_bus_message_append(m, "i", id)) < 0) return r;
    if ((r = sd_bus_message_open_container(m, 'a', "{sv}")) < 0) return r;
    if (id == 0) {
        if ((r = put_string(m, "children-display", "submenu")) < 0) return r;
        if ((r = put_string(m, "label", tray->callbacks->title ? tray->callbacks->title : "Snowhush")) < 0) return r;
    } else if (item) {
        if (nameCount == 0) {
            if ((r = put_item_props(m, item)) < 0) return r;
        } else {
            for (int i = 0; i < nameCount; i++) {
                const char *name = names[i];
                if (strcmp(name, "label") == 0) {
                    if ((r = put_string(m, "label", item->label ? item->label : "")) < 0) return r;
                } else if (strcmp(name, "enabled") == 0) {
                    if ((r = put_bool(m, "enabled", item->enabled)) < 0) return r;
                } else if (strcmp(name, "visible") == 0) {
                    if ((r = put_bool(m, "visible", 1)) < 0) return r;
                } else if (strcmp(name, "type") == 0) {
                    if ((r = put_string(m, "type", item->kind == TRAY_SEPARATOR ? "separator" : "standard")) < 0) return r;
                }
            }
        }
    }
    if ((r = sd_bus_message_close_container(m)) < 0) return r;
    return sd_bus_message_close_container(m);
}

static int menu_get_group_properties(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)error;
    int r;
    if ((r = sd_bus_message_enter_container(message, 'a', "i")) < 0) return r;
    int ids[TRAY_MAX_ITEMS];
    int count = 0;
    int32_t id;
    while (sd_bus_message_read(message, "i", &id) > 0) {
        if (count < TRAY_MAX_ITEMS) ids[count++] = id;
    }
    sd_bus_message_exit_container(message);

    const char *names[16];
    int nameCount = 0;
    if ((r = sd_bus_message_enter_container(message, 'a', "s")) < 0) return r;
    const char *name;
    while (sd_bus_message_read(message, "s", &name) > 0) {
        if (nameCount < 16) names[nameCount++] = name;
    }
    sd_bus_message_exit_container(message);

    sd_bus_message *reply = NULL;
    if ((r = sd_bus_message_new_method_return(message, &reply)) < 0) return r;
    if ((r = sd_bus_message_open_container(reply, 'a', "(ia{sv})")) < 0) goto fail;
    if (count == 0) {
        if ((r = append_properties(reply, tray, 0, names, nameCount)) < 0) goto fail;
    } else {
        for (int i = 0; i < count; i++)
            if ((r = append_properties(reply, tray, ids[i], names, nameCount)) < 0) goto fail;
    }
    if ((r = sd_bus_message_close_container(reply)) < 0) goto fail;
    return sd_bus_send(NULL, reply, NULL);
fail:
    sd_bus_message_unref(reply);
    return r;
}

static int menu_get_property(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)error;
    int32_t id = 0;
    const char *name = "";
    int r = sd_bus_message_read(message, "is", &id, &name);
    if (r < 0) return r;

    TrayItem items[TRAY_MAX_ITEMS];
    int count = build_items(tray, items);
    const TrayItem *item = id == 0 ? NULL : item_find(items, count, id);

    sd_bus_message *reply = NULL;
    if ((r = sd_bus_message_new_method_return(message, &reply)) < 0) return r;

    if (id == 0 && strcmp(name, "children-display") == 0) {
        if ((r = sd_bus_message_append(reply, "v", "s", "submenu")) < 0) goto fail;
    } else if (item && strcmp(name, "label") == 0) {
        if ((r = sd_bus_message_append(reply, "v", "s", item->label ? item->label : "")) < 0) goto fail;
    } else if (item && strcmp(name, "enabled") == 0) {
        if ((r = sd_bus_message_append(reply, "v", "b", item->enabled ? 1 : 0)) < 0) goto fail;
    } else if (item && strcmp(name, "visible") == 0) {
        if ((r = sd_bus_message_append(reply, "v", "b", 1)) < 0) goto fail;
    } else if (item && strcmp(name, "type") == 0) {
        if ((r = sd_bus_message_append(reply, "v", "s", item->kind == TRAY_SEPARATOR ? "separator" : "standard")) < 0) goto fail;
    } else if (item && strcmp(name, "children-display") == 0) {
        if ((r = sd_bus_message_append(reply, "v", "s", "submenu")) < 0) goto fail;
    } else {
        if ((r = sd_bus_message_append(reply, "v", "s", "")) < 0) goto fail;
    }
    return sd_bus_send(NULL, reply, NULL);
fail:
    sd_bus_message_unref(reply);
    return r;
}

static int menu_event(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)error;
    int32_t id = 0;
    const char *event = "";
    int r = sd_bus_message_read(message, "is", &id, &event);
    if (r < 0) return r;
    sd_bus_message_skip(message, "vu");
    if (strcmp(event, "clicked") == 0 && tray->callbacks->event) tray->callbacks->event(id);
    return sd_bus_reply_method_return(message, NULL);
}

static int menu_about_to_show(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    sd_bus_message_skip(message, "i");
    return sd_bus_reply_method_return(message, "b", 0);
}

static int menu_about_to_show_group(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    int r;
    if ((r = sd_bus_message_enter_container(message, 'a', "i")) < 0) return r;
    int ids[TRAY_MAX_ITEMS];
    int count = 0;
    int32_t id;
    while (sd_bus_message_read(message, "i", &id) > 0) {
        if (count < TRAY_MAX_ITEMS) ids[count++] = id;
    }
    sd_bus_message_exit_container(message);

    sd_bus_message *reply = NULL;
    if ((r = sd_bus_message_new_method_return(message, &reply)) < 0) return r;
    if ((r = sd_bus_message_open_container(reply, 'a', "i")) < 0) goto fail;
    for (int i = 0; i < count; i++)
        if ((r = sd_bus_message_append(reply, "i", ids[i])) < 0) goto fail;
    if ((r = sd_bus_message_close_container(reply)) < 0) goto fail;
    if ((r = sd_bus_message_open_container(reply, 'a', "i")) < 0) goto fail;
    if ((r = sd_bus_message_close_container(reply)) < 0) goto fail;
    return sd_bus_send(NULL, reply, NULL);
fail:
    sd_bus_message_unref(reply);
    return r;
}

static int menu_event_group(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)error;
    int r;
    if ((r = sd_bus_message_enter_container(message, 'a', "(isvu)")) < 0) return r;
    int32_t id = 0;
    const char *event = "";
    while (sd_bus_message_enter_container(message, 'r', "isvu") > 0) {
        sd_bus_message_read(message, "is", &id, &event);
        sd_bus_message_skip(message, "vu");
        sd_bus_message_exit_container(message);
        if (strcmp(event, "clicked") == 0 && tray->callbacks->event) tray->callbacks->event(id);
    }
    sd_bus_message_exit_container(message);
    return sd_bus_reply_method_return(message, "ai", 0, NULL);
}

static const sd_bus_vtable menu_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", menu_get_layout, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})", menu_get_group_properties, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetProperty", "is", "v", menu_get_property, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Event", "isvu", "", menu_event, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("EventGroup", "a(isvu)", "ai", menu_event_group, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AboutToShow", "i", "b", menu_about_to_show, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AboutToShowGroup", "ai", "aiai", menu_about_to_show_group, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("LayoutUpdated", "ui", 0),
    SD_BUS_SIGNAL("ItemsPropertiesUpdated", "a(ia{sv})a(ia{sv})", 0),
    SD_BUS_VTABLE_END,
};

/* ------------------------------------------------------------------ */
/* bus                                                                 */

static double tray_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int register_item(Tray *tray)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(tray->bus, WATCHER_SERVICE, WATCHER_PATH, WATCHER_IFACE,
                               "RegisterStatusNotifierItem", &error, NULL, "s", tray->serviceName);
    if (r < 0) {
        tray->registered = 0;
    } else {
        tray->registered = 1;
    }
    sd_bus_error_free(&error);
    return r;
}

static int on_name_owner_changed(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Tray *tray = userdata;
    (void)error;
    const char *name = NULL, *oldOwner = NULL, *newOwner = NULL;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0) return 0;
    if (name && strcmp(name, WATCHER_SERVICE) == 0) {
        tray->registered = 0;
        tray->retryAt = 0;
    }
    return 0;
}

static int tray_connect(Tray *tray)
{
    if (sd_bus_default_user(&tray->bus) < 0) {
        tray->bus = NULL;
        return -1;
    }
    char name[128];
    snprintf(name, sizeof(name), "org.kde.StatusNotifierItem-%d-1", (int)getpid());
    snprintf(tray->serviceName, sizeof(tray->serviceName), "%s", name);
    sd_bus_request_name(tray->bus, name, 0);

    if (sd_bus_add_object_vtable(tray->bus, NULL, SNI_PATH, SNI_IFACE, sni_vtable, tray) < 0 ||
        sd_bus_add_object_vtable(tray->bus, NULL, MENU_PATH, MENU_IFACE, menu_vtable, tray) < 0) {
        sd_bus_flush_close_unref(tray->bus);
        tray->bus = NULL;
        return -1;
    }
    sd_bus_match_signal(tray->bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                        "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner_changed, tray);
    tray->registered = 0;
    tray->retryAt = 0;
    return 0;
}

Tray *tray_start(const TrayCallbacks *callbacks)
{
    Tray *tray = calloc(1, sizeof(Tray));
    if (!tray) return NULL;
    tray->callbacks = callbacks;
    tray->revision = 1;
    icons_create(tray);
    if (tray_connect(tray) < 0) {
        /* No session bus is not fatal: the app still runs, just without a tray. */
        fprintf(stderr, "snowhush: tray unavailable (no session bus)\n");
    }
    return tray;
}

void tray_stop(Tray *tray)
{
    if (!tray) return;
    if (tray->bus) sd_bus_flush_close_unref(tray->bus);
    for (int i = 0; i < tray->iconCount; i++) free(tray->icons[i].pixels);
    free(tray);
}

void tray_poll(Tray *tray)
{
    if (!tray) return;
    double now = tray_now();
    if (!tray->bus) {
        if (now < tray->retryAt) return;
        tray->retryAt = now + 5.0;
        tray_connect(tray);
        return;
    }
    for (int i = 0; i < 16; i++) {
        int r = sd_bus_process(tray->bus, NULL);
        if (r < 0) {
            sd_bus_flush_close_unref(tray->bus);
            tray->bus = NULL;
            tray->retryAt = now + 5.0;
            return;
        }
        if (r == 0) break;
    }
    if (!tray->registered && now >= tray->retryAt) {
        tray->retryAt = now + 2.0;
        register_item(tray);
    }
}

void tray_refresh(Tray *tray)
{
    if (!tray || !tray->bus) return;
    tray->revision++;
    sd_bus_emit_signal(tray->bus, MENU_PATH, MENU_IFACE, "LayoutUpdated", "ui", tray->revision, 0);
}

#endif /* SNOW_NO_TRAY */
