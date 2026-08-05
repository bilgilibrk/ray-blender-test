// Minimal recursive-descent JSON reader used by the level loader.
//
// Everything is allocated from a caller-supplied Arena, so a parsed document is
// released by resetting that arena; individual values are never freed. As a
// convenience for hand-edited levels the lexer also skips `//` and `/* */`
// comments, which strict JSON does not allow.
#ifndef ENGINE_JSON_H
#define ENGINE_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "engine/arena.h"

typedef enum JsonType {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct JsonMember JsonMember;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        struct { const char *chars; int length; } string;   // NUL-terminated as well
        struct { JsonValue *items; int count; } array;
        struct { JsonMember *members; int count; } object;
    } as;
};

struct JsonMember {
    const char *key;
    int keyLength;
    JsonValue value;
};

// Returns NULL and fills `err` (may be NULL) when the text is malformed.
JsonValue *JsonParse(Arena *arena, const char *text, size_t length, char *err, int errSize);

// Object/array access. All accessors tolerate NULL and type mismatches.
const JsonValue *JsonGet(const JsonValue *object, const char *key);
int JsonCount(const JsonValue *array);
const JsonValue *JsonAt(const JsonValue *array, int index);

double JsonNumber(const JsonValue *value, double fallback);
bool JsonBool(const JsonValue *value, bool fallback);
const char *JsonString(const JsonValue *value, const char *fallback);

// Reads `count` numbers from an array value into `out`; false if unavailable.
bool JsonFloats(const JsonValue *array, float *out, int count);

// Shorthand for JsonFloats(JsonGet(object, key), ...), leaving `out` untouched on miss.
bool JsonFloatsField(const JsonValue *object, const char *key, float *out, int count);
double JsonNumberField(const JsonValue *object, const char *key, double fallback);
const char *JsonStringField(const JsonValue *object, const char *key, const char *fallback);

#endif // ENGINE_JSON_H
