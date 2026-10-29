/* A small JSON reader: enough for the Hyprland IPC output. */
#ifndef SNOW_JSON_H
#define SNOW_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

typedef struct Json Json;

struct Json {
    JsonType type;
    double number;
    int boolean;
    char *string;
    char *key;
    Json *child;
    Json *next;
};

Json *json_parse(const char *text);
void json_free(Json *value);

Json *json_get(const Json *object, const char *key);
const char *json_string(const Json *object, const char *key, const char *fallback);
double json_number(const Json *object, const char *key, double fallback);
int json_bool(const Json *object, const char *key, int fallback);

#endif
