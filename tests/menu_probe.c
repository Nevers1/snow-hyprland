/* Checks that a running snowhush serves a menu that libdbusmenu (what Waybar
 * uses for tray menus) can actually parse. Build and run:
 *
 *   cc menu_probe.c -o menu_probe $(pkg-config --cflags --libs libdbusmenu-glib-0.4 glib-2.0) \
 *      $(pkg-config --cflags --libs gtk+-3.0)
 *   ./menu_probe org.kde.StatusNotifierItem-<pid>-1 /MenuBar
 */
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <libdbusmenu-glib/client.h>
#include <libdbusmenu-glib/menuitem.h>
#include <stdio.h>

static void dump(DbusmenuMenuitem *item, int depth)
{
    const gchar *label = dbusmenu_menuitem_property_get(item, DBUSMENU_MENUITEM_PROP_LABEL);
    const gchar *type = dbusmenu_menuitem_property_get(item, DBUSMENU_MENUITEM_PROP_TYPE);
    gint toggle = dbusmenu_menuitem_property_get_int(item, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE);
    printf("%*s[%d] %s%s%s\n", depth * 2, "", dbusmenu_menuitem_get_id(item), label ? label : "",
           type ? " (separator)" : "", toggle >= 0 ? (toggle ? " [x]" : " [ ]") : "");
    GList *children = dbusmenu_menuitem_get_children(item);
    for (GList *l = children; l; l = l->next) dump(DBUSMENU_MENUITEM(l->data), depth + 1);
}

static gboolean quit(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bus-name> <menu-path>\n", argv[0]);
        return 2;
    }
    DbusmenuClient *client = dbusmenu_client_new(argv[1], argv[2]);
    g_signal_connect(client, "layout-updated", G_CALLBACK(quit), NULL);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(3, (GSourceFunc)quit, loop);
    g_main_loop_run(loop);

    DbusmenuMenuitem *root = dbusmenu_client_get_root(client);
    if (!root) {
        printf("no layout root: libdbusmenu could not read the menu\n");
        return 1;
    }
    dump(root, 0);
    return 0;
}
