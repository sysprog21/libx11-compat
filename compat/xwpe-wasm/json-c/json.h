#pragma once

/* Compile-only json-c stub for the wasm xwpe build. The LSP and DAP sources
 * that use json-c spawn a language/debug server with fork+execvp, which does
 * not exist under emscripten, so those features never run in the browser. These
 * functions exist solely to let that code compile; they intentionally store and
 * expose nothing. Do not mistake this for a working json-c.
 */

#include <stddef.h>

#define JSON_C_TO_STRING_PLAIN 0

enum json_type {
    json_type_null,
    json_type_boolean,
    json_type_double,
    json_type_int,
    json_type_object,
    json_type_array,
    json_type_string
};

enum json_tokener_error { json_tokener_success = 0 };

struct json_object {
    int unused;
};

struct json_tokener {
    int unused;
};

static struct json_object json_stub_object;

static inline struct json_object *json_object_get(struct json_object *obj)
{
    return obj;
}
static inline void json_object_put(struct json_object *obj)
{
    (void) obj;
}
static inline struct json_object *json_object_new_object(void)
{
    return &json_stub_object;
}
static inline struct json_object *json_object_new_array(void)
{
    return &json_stub_object;
}
static inline struct json_object *json_object_new_string(const char *s)
{
    (void) s;
    return &json_stub_object;
}
static inline struct json_object *json_object_new_int(int i)
{
    (void) i;
    return &json_stub_object;
}
static inline struct json_object *json_object_new_boolean(int b)
{
    (void) b;
    return &json_stub_object;
}
static inline void json_object_object_add(struct json_object *obj,
                                          const char *key,
                                          struct json_object *val)
{
    (void) obj;
    (void) key;
    (void) val;
}
static inline int json_object_object_get_ex(const struct json_object *obj,
                                            const char *key,
                                            struct json_object **val)
{
    (void) obj;
    (void) key;
    if (val)
        *val = NULL;
    return 0;
}
static inline int json_object_is_type(const struct json_object *obj,
                                      enum json_type type)
{
    (void) obj;
    (void) type;
    return 0;
}
static inline int json_object_get_int(const struct json_object *obj)
{
    (void) obj;
    return 0;
}
static inline int json_object_get_boolean(const struct json_object *obj)
{
    (void) obj;
    return 0;
}
static inline const char *json_object_get_string(const struct json_object *obj)
{
    (void) obj;
    return "";
}
static inline const char *json_object_to_json_string(
    const struct json_object *obj)
{
    (void) obj;
    return "{}";
}
static inline const char *json_object_to_json_string_ext(
    const struct json_object *obj,
    int flags)
{
    (void) obj;
    (void) flags;
    return "{}";
}
static inline int json_object_array_add(struct json_object *obj,
                                        struct json_object *val)
{
    (void) obj;
    (void) val;
    return 0;
}
static inline size_t json_object_array_length(const struct json_object *obj)
{
    (void) obj;
    return 0;
}
static inline struct json_object *json_object_array_get_idx(
    const struct json_object *obj,
    size_t idx)
{
    (void) obj;
    (void) idx;
    return NULL;
}
static inline struct json_object *json_object_from_file(const char *path)
{
    (void) path;
    return NULL;
}
static inline struct json_tokener *json_tokener_new(void)
{
    static struct json_tokener tok;
    return &tok;
}
static inline void json_tokener_free(struct json_tokener *tok)
{
    (void) tok;
}
static inline struct json_object *
json_tokener_parse_ex(struct json_tokener *tok, const char *str, int len)
{
    (void) tok;
    (void) str;
    (void) len;
    return &json_stub_object;
}
static inline enum json_tokener_error json_tokener_get_error(
    struct json_tokener *tok)
{
    (void) tok;
    return json_tokener_success;
}

/* Stub objects carry no fields, so iteration is always empty. The (void)(obj)
 * keeps the container referenced; key and val are declared for the loop body.
 */
#define json_object_object_foreach(obj, key, val)      \
    for (char *key = ((void) (obj), (char *) 0); key;) \
        for (struct json_object *val = NULL; key;)
