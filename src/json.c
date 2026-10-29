#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    int error;
} Parser;

static void skip_space(Parser *s)
{
    while (*s->p && (unsigned char)*s->p <= ' ') s->p++;
}

static void put_utf8(char **out, unsigned cp)
{
    if (cp < 0x80) {
        *(*out)++ = (char)cp;
    } else if (cp < 0x800) {
        *(*out)++ = (char)(0xC0 | (cp >> 6));
        *(*out)++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *(*out)++ = (char)(0xE0 | (cp >> 12));
        *(*out)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*out)++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *(*out)++ = (char)(0xF0 | (cp >> 18));
        *(*out)++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *(*out)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*out)++ = (char)(0x80 | (cp & 0x3F));
    }
}

static char *parse_string_raw(Parser *s)
{
    if (*s->p != '"') {
        s->error = 1;
        return NULL;
    }
    s->p++;
    const char *start = s->p;
    size_t len = 0;
    for (const char *q = start; *q && *q != '"'; q++) {
        if (*q == '\\' && q[1]) q++;
        len++;
    }
    char *out = malloc(len * 4 + 1);
    char *w = out;
    while (*s->p && *s->p != '"') {
        unsigned char c = (unsigned char)*s->p++;
        if (c != '\\') {
            *w++ = (char)c;
            continue;
        }
        char esc = *s->p++;
        switch (esc) {
        case 'n': *w++ = '\n'; break;
        case 't': *w++ = '\t'; break;
        case 'r': *w++ = '\r'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 0; k < 4 && isxdigit((unsigned char)*s->p); k++) {
                char h = *s->p++;
                cp = cp * 16 + (unsigned)(h <= '9' ? h - '0' : (tolower(h) - 'a' + 10));
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && s->p[0] == '\\' && s->p[1] == 'u') {
                s->p += 2;
                unsigned low = 0;
                for (int k = 0; k < 4 && isxdigit((unsigned char)*s->p); k++) {
                    char h = *s->p++;
                    low = low * 16 + (unsigned)(h <= '9' ? h - '0' : (tolower(h) - 'a' + 10));
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            put_utf8(&w, cp);
            break;
        }
        default: *w++ = esc; break;
        }
    }
    if (*s->p == '"') s->p++;
    *w = 0;
    return out;
}

static Json *parse_value(Parser *s);

static Json *new_node(JsonType type)
{
    Json *n = calloc(1, sizeof(Json));
    n->type = type;
    return n;
}

static Json *parse_array(Parser *s)
{
    Json *arr = new_node(JSON_ARRAY);
    s->p++; /* [ */
    Json **tail = &arr->child;
    skip_space(s);
    if (*s->p == ']') {
        s->p++;
        return arr;
    }
    for (;;) {
        Json *v = parse_value(s);
        if (!v) {
            s->error = 1;
            json_free(arr);
            return NULL;
        }
        *tail = v;
        tail = &v->next;
        skip_space(s);
        if (*s->p == ',') {
            s->p++;
            continue;
        }
        if (*s->p == ']') {
            s->p++;
            return arr;
        }
        s->error = 1;
        json_free(arr);
        return NULL;
    }
}

static Json *parse_object(Parser *s)
{
    Json *obj = new_node(JSON_OBJECT);
    s->p++; /* { */
    Json **tail = &obj->child;
    skip_space(s);
    if (*s->p == '}') {
        s->p++;
        return obj;
    }
    for (;;) {
        skip_space(s);
        char *key = parse_string_raw(s);
        if (!key) {
            s->error = 1;
            json_free(obj);
            return NULL;
        }
        skip_space(s);
        if (*s->p != ':') {
            free(key);
            s->error = 1;
            json_free(obj);
            return NULL;
        }
        s->p++;
        Json *v = parse_value(s);
        if (!v) {
            free(key);
            s->error = 1;
            json_free(obj);
            return NULL;
        }
        v->key = key;
        *tail = v;
        tail = &v->next;
        skip_space(s);
        if (*s->p == ',') {
            s->p++;
            continue;
        }
        if (*s->p == '}') {
            s->p++;
            return obj;
        }
        s->error = 1;
        json_free(obj);
        return NULL;
    }
}

static Json *parse_value(Parser *s)
{
    skip_space(s);
    switch (*s->p) {
    case '{':
        return parse_object(s);
    case '[':
        return parse_array(s);
    case '"': {
        Json *n = new_node(JSON_STRING);
        n->string = parse_string_raw(s);
        if (!n->string) {
            free(n);
            s->error = 1;
            return NULL;
        }
        return n;
    }
    case 't':
        if (strncmp(s->p, "true", 4) == 0) {
            s->p += 4;
            Json *n = new_node(JSON_BOOL);
            n->boolean = 1;
            return n;
        }
        s->error = 1;
        return NULL;
    case 'f':
        if (strncmp(s->p, "false", 5) == 0) {
            s->p += 5;
            return new_node(JSON_BOOL);
        }
        s->error = 1;
        return NULL;
    case 'n':
        if (strncmp(s->p, "null", 4) == 0) {
            s->p += 4;
            return new_node(JSON_NULL);
        }
        s->error = 1;
        return NULL;
    default: {
        char *end = NULL;
        double v = strtod(s->p, &end);
        if (end == s->p) {
            s->error = 1;
            return NULL;
        }
        s->p = end;
        Json *n = new_node(JSON_NUMBER);
        n->number = v;
        return n;
    }
    }
}

Json *json_parse(const char *text)
{
    Parser s = {text, 0};
    Json *v = parse_value(&s);
    if (s.error) {
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(Json *value)
{
    while (value) {
        Json *next = value->next;
        free(value->string);
        free(value->key);
        json_free(value->child);
        free(value);
        value = next;
    }
}

Json *json_get(const Json *object, const char *key)
{
    if (!object || object->type != JSON_OBJECT) return NULL;
    for (Json *c = object->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0) return c;
    return NULL;
}

const char *json_string(const Json *object, const char *key, const char *fallback)
{
    Json *v = json_get(object, key);
    if (!v || v->type != JSON_STRING) return fallback;
    return v->string;
}

double json_number(const Json *object, const char *key, double fallback)
{
    Json *v = json_get(object, key);
    if (!v || v->type != JSON_NUMBER) return fallback;
    return v->number;
}

int json_bool(const Json *object, const char *key, int fallback)
{
    Json *v = json_get(object, key);
    if (!v || v->type != JSON_BOOL) return fallback;
    return v->boolean;
}
