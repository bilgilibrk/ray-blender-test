#include <stdio.h>
#include <string.h>

#include "engine/json.h"

#include "tests.h"

static JsonValue *P(Arena *a, const char *text, char *err, int errSize)
{
    return JsonParse(a, text, strlen(text), err, errSize);
}

void RunJsonTests(void)
{
    Arena a;
    ArenaInit(&a, 1 << 20, "test");
    char err[256];

    // --- happy path -------------------------------------------------------
    const char *doc =
        "{\n"
        "  // a comment\n"
        "  \"name\": \"circuit \\\"one\\\"\\n\",\n"
        "  \"laps\": 3, \"scale\": -2.5e1, \"loop\": true, \"nothing\": null,\n"
        "  \"pos\": [1, 2.5, -3],\n"
        "  \"props\": [ {\"model\":\"roadStraight\"}, {\"model\":\"treeLarge\"} ],\n"
        "  \"empty_arr\": [], \"empty_obj\": {},\n"
        "  \"unicode\": \"\\u00e9\\u0041\"\n"
        "}";
    JsonValue *root = P(&a, doc, err, sizeof err);
    CHECK(root != NULL, "root parsed");
    if (!root) { printf("  err=%s\n", err); return; }

    CHECK(root->type == JSON_OBJECT, "root is object");
    CHECK(strcmp(JsonStringField(root, "name", ""), "circuit \"one\"\n") == 0, "escaped string");
    CHECK(JsonNumberField(root, "laps", 0) == 3, "integer field");
    CHECK(JsonNumberField(root, "scale", 0) == -25.0, "exponent field");
    CHECK(JsonBool(JsonGet(root, "loop"), false) == true, "bool field");
    CHECK(JsonGet(root, "nothing")->type == JSON_NULL, "null field");
    CHECK(JsonGet(root, "missing") == NULL, "missing field is NULL");

    float v[3] = {0, 0, 0};
    CHECK(JsonFloatsField(root, "pos", v, 3), "vec3 read");
    CHECK(v[0] == 1.0f && v[1] == 2.5f && v[2] == -3.0f, "vec3 values");

    CHECK(JsonCount(JsonGet(root, "props")) == 2, "array count");
    CHECK(strcmp(JsonStringField(JsonAt(JsonGet(root, "props"), 1), "model", ""), "treeLarge") == 0,
          "nested object in array");
    CHECK(JsonCount(JsonGet(root, "empty_arr")) == 0, "empty array");
    CHECK(JsonCount(JsonGet(root, "empty_obj")) == 0, "empty object");
    CHECK(JsonAt(JsonGet(root, "props"), 9) == NULL, "out-of-range index");
    CHECK(strcmp(JsonStringField(root, "unicode", ""), "\xc3\xa9" "A") == 0, "\\u escape to utf8");

    // Accessors must tolerate wrong types and NULL.
    CHECK(JsonNumber(JsonGet(root, "name"), 42.0) == 42.0, "number of string -> fallback");
    CHECK(strcmp(JsonString(JsonGet(root, "laps"), "fb"), "fb") == 0, "string of number -> fallback");
    CHECK(JsonCount(NULL) == 0, "count of NULL");
    CHECK(JsonGet(NULL, "x") == NULL, "get from NULL");

    // --- malformed input must be rejected, not crash ----------------------
    const char *bad[] = {
        "{", "[", "{\"a\":}", "{\"a\" 1}", "[1,]", "{,}", "\"unterminated",
        "{\"a\":1} trailing", "tru", "[1 2]", "{\"a\":1,}", "",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Arena tmp;
        ArenaInit(&tmp, 1 << 16, "bad");
        JsonValue *r = P(&tmp, bad[i], err, sizeof err);
        if (r != NULL) { printf("  FAIL: accepted malformed input #%u: <%s>\n", i, bad[i]); g_failures++; g_checks++; }
        ArenaFree(&tmp);
    }

    // Deep nesting must hit the depth guard rather than smash the stack.
    char deep[4096];
    memset(deep, '[', sizeof deep - 1);
    deep[sizeof deep - 1] = '\0';
    Arena tmp;
    ArenaInit(&tmp, 1 << 20, "deep");
    CHECK(P(&tmp, deep, err, sizeof err) == NULL, "deep nesting rejected");
    ArenaFree(&tmp);

    // Exhausting the arena must fail cleanly.
    Arena tiny;
    ArenaInit(&tiny, 64, "tiny");
    CHECK(P(&tiny, doc, err, sizeof err) == NULL, "arena exhaustion handled");
    ArenaFree(&tiny);

    ArenaFree(&a);
}
