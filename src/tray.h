/* The tray icon: a StatusNotifierItem with a D-Bus menu.
 *
 * Waybar (and any other StatusNotifierHost) shows it next to the other tray
 * icons, and the menu is served over com.canonical.dbusmenu, so the settings
 * are reachable without a terminal.
 *
 * The menu itself lives in main.c: this module only knows how to publish a
 * flat list of items and how to report clicks back.
 */
#ifndef SNOW_TRAY_H
#define SNOW_TRAY_H

#define TRAY_MAX_ITEMS 64

typedef enum {
    TRAY_PLAIN = 0,
    TRAY_CHECK,   /* state is drawn into the label by the menu builder */
    TRAY_RADIO,
    TRAY_SEPARATOR,
    TRAY_SUBMENU,
} TrayKind;

typedef struct {
    int id;
    int parent; /* 0 for a top level entry */
    TrayKind kind;
    int state; /* checked for TRAY_CHECK and TRAY_RADIO */
    int enabled;
    const char *label;
} TrayItem;

typedef struct {
    const char *title;
    /* Fills `items` (parents before their children) and returns the count. */
    int (*build)(TrayItem *items, int capacity);
    /* A menu entry was activated. */
    void (*event)(int id);
} TrayCallbacks;

typedef struct Tray Tray;

Tray *tray_start(const TrayCallbacks *callbacks);
void tray_stop(Tray *tray);
/* Reads pending bus traffic and re-registers with the watcher if needed. */
void tray_poll(Tray *tray);
/* Tells the host the menu changed, so it asks for the layout again. */
void tray_refresh(Tray *tray);

#endif
