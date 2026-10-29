#include "hypr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

static char socket_path[512];

int hypr_init(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig || !*sig) {
        fprintf(stderr, "snowhush: HYPRLAND_INSTANCE_SIGNATURE is not set; run inside Hyprland\n");
        return 0;
    }
    if (runtime && *runtime)
        snprintf(socket_path, sizeof(socket_path), "%s/hypr/%s/.socket.sock", runtime, sig);
    else
        snprintf(socket_path, sizeof(socket_path), "/tmp/hypr/%s/.socket.sock", sig);
    if (access(socket_path, F_OK) != 0) {
        fprintf(stderr, "snowhush: Hyprland socket not found at %s\n", socket_path);
        return 0;
    }
    return 1;
}

/* One request per connection: Hyprland closes the socket after answering. */
static char *hypr_request(const char *command, size_t *outLength)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    size_t len = strlen(command);
    if (write(fd, command, len) != (ssize_t)len) {
        close(fd);
        return NULL;
    }
    shutdown(fd, SHUT_WR);

    size_t capacity = 1 << 16, length = 0;
    char *buf = malloc(capacity);
    for (;;) {
        if (length + 4096 + 1 > capacity) {
            capacity *= 2;
            buf = realloc(buf, capacity);
        }
        ssize_t n = read(fd, buf + length, capacity - length - 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return NULL;
        }
        length += (size_t)n;
    }
    close(fd);
    buf[length] = 0;
    if (outLength) *outLength = length;
    return buf;
}

int hypr_cursor(double *x, double *y)
{
    char *reply = hypr_request("cursorpos", NULL);
    if (!reply) return 0;
    int ok = 0;
    double px, py;
    if (sscanf(reply, "%lf, %lf", &px, &py) == 2) {
        *x = px;
        *y = py;
        ok = 1;
    }
    free(reply);
    return ok;
}

static void parse_monitors(HyprState *state, const char *text)
{
    Json *root = json_parse(text);
    if (!root || root->type != JSON_ARRAY) {
        json_free(root);
        return;
    }
    int count = 0;
    for (Json *c = root->child; c; c = c->next) count++;
    state->monitors = calloc((size_t)(count ? count : 1), sizeof(HyprMonitor));
    state->monitorCount = 0;
    int index = 0;
    for (Json *c = root->child; c; c = c->next, index++) {
        HyprMonitor *m = &state->monitors[state->monitorCount++];
        const char *name = json_string(c, "name", "");
        snprintf(m->name, sizeof(m->name), "%s", name);
        Json *at = json_get(c, "at");
        m->x = at && at->child ? (int)at->child->number : (int)json_number(c, "x", 0);
        m->y = at && at->child && at->child->next ? (int)at->child->next->number : (int)json_number(c, "y", 0);
        m->w = (int)json_number(c, "width", 0);
        m->h = (int)json_number(c, "height", 0);
        m->scale = (float)json_number(c, "scale", 1);
        m->focused = json_bool(c, "focused", 0);
        Json *active = json_get(c, "activeWorkspace");
        m->activeWorkspaceId = active ? (int)json_number(active, "id", 0) : 0;
        Json *special = json_get(c, "specialWorkspace");
        m->specialWorkspaceId = special ? (int)json_number(special, "id", 0) : 0;
        (void)index;
    }
    json_free(root);
}

static void parse_clients(HyprState *state, const char *text)
{
    Json *root = json_parse(text);
    if (!root || root->type != JSON_ARRAY) {
        json_free(root);
        return;
    }
    int count = 0;
    for (Json *c = root->child; c; c = c->next) count++;
    state->clients = calloc((size_t)(count ? count : 1), sizeof(HyprClient));
    state->clientCount = 0;
    for (Json *c = root->child; c; c = c->next) {
        HyprClient *client = &state->clients[state->clientCount++];
        const char *address = json_string(c, "address", "0x0");
        client->id = strtoull(address, NULL, 16);
        Json *at = json_get(c, "at");
        Json *size = json_get(c, "size");
        client->x = at && at->child ? (int)at->child->number : 0;
        client->y = at && at->child && at->child->next ? (int)at->child->next->number : 0;
        client->w = size && size->child ? (int)size->child->number : 0;
        client->h = size && size->child && size->child->next ? (int)size->child->next->number : 0;
        client->monitor = (int)json_number(c, "monitor", -1);
        client->fullscreen = (int)json_number(c, "fullscreen", 0);
        client->floating = json_bool(c, "floating", 0);
        client->mapped = json_bool(c, "mapped", 1);
        client->hidden = json_bool(c, "hidden", 0);
        client->visible = json_bool(c, "visible", 1);
        client->pinned = json_bool(c, "pinned", 0);
        Json *workspace = json_get(c, "workspace");
        client->workspaceId = workspace ? (int)json_number(workspace, "id", 0) : 0;
        snprintf(client->klass, sizeof(client->klass), "%s", json_string(c, "class", ""));
    }
    json_free(root);
}

static void parse_layers(HyprState *state, const char *text)
{
    Json *root = json_parse(text);
    if (!root || root->type != JSON_OBJECT) {
        json_free(root);
        return;
    }
    state->layerCount = 0;
    for (Json *mon = root->child; mon; mon = mon->next) {
        Json *levels = json_get(mon, "levels");
        if (!levels) continue;
        for (Json *level = levels->child; level; level = level->next) {
            int levelNumber = level->key ? atoi(level->key) : 0;
            if (levelNumber < 2) continue; /* only top and overlay layers sit above windows */
            for (Json *entry = level->child; entry; entry = entry->next) {
                if (state->layerCount >= HYPR_MAX_LAYERS) break;
                HyprLayer *layer = &state->layers[state->layerCount++];
                layer->x = (int)json_number(entry, "x", 0);
                layer->y = (int)json_number(entry, "y", 0);
                layer->w = (int)json_number(entry, "w", 0);
                layer->h = (int)json_number(entry, "h", 0);
                layer->level = levelNumber;
                snprintf(layer->name, sizeof(layer->name), "%s", json_string(entry, "namespace", ""));
            }
        }
    }
    json_free(root);
}

static int parse_rounding(const char *text)
{
    Json *root = json_parse(text);
    if (!root) return -1;
    int value = (int)json_number(root, "int", 0);
    json_free(root);
    return value;
}

/* The rounding option rarely changes; asking for it every poll would be rude. */
static int cachedRounding = 10;
static double roundingCheckedAt = -1e9;

int hypr_refresh(HyprState *state)
{
    memset(state, 0, sizeof(*state));
    char *monitors = hypr_request("j/monitors", NULL);
    char *clients = hypr_request("j/clients", NULL);
    char *layers = hypr_request("j/layers", NULL);
    int ok = monitors && clients;
    if (ok) {
        parse_monitors(state, monitors);
        parse_clients(state, clients);
        if (layers) parse_layers(state, layers);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        if (now - roundingCheckedAt > 30) {
            roundingCheckedAt = now;
            char *reply = hypr_request("j/getoption decoration:rounding", NULL);
            if (reply) {
                int value = parse_rounding(reply);
                if (value >= 0) cachedRounding = value;
                free(reply);
            }
        }
        state->rounding = cachedRounding;
        state->ok = 1;
    }
    free(monitors);
    free(clients);
    free(layers);
    return ok;
}

void hypr_state_free(HyprState *state)
{
    free(state->clients);
    free(state->monitors);
    state->clients = NULL;
    state->monitors = NULL;
    state->clientCount = 0;
    state->monitorCount = 0;
    state->layerCount = 0;
    state->ok = 0;
}
