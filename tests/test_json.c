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

    // --- numbers ------------------------------------------------------------
    // strtod will take all of these; JSON does not. Letting them through means
    // a typo in a level file arrives as a plausible-looking coordinate.
    const char *notJson[] = {
        "{\"v\": +1}",          // leading plus
        "{\"v\": 0x10}",        // hex
        "{\"v\": 0x1p8}",       // hex float
        "{\"v\": 01}",          // leading zero
        "{\"v\": 1.}",          // no digit after the point
        "{\"v\": .5}",          // no digit before it
        "{\"v\": 1e}",          // no exponent digits
        "{\"v\": 1e+}",
        "{\"v\": -}",
        "{\"v\": 1e999}",       // well-formed, but infinity in a double
        "{\"v\": -1e999}",
        "{\"v\": 1E999}",
    };
    for (unsigned i = 0; i < sizeof notJson / sizeof notJson[0]; i++) {
        Arena n;
        ArenaInit(&n, 1 << 16, "num");
        JsonValue *r = P(&n, notJson[i], err, sizeof err);
        CHECK(r == NULL, "accepted a number JSON does not allow: %s", notJson[i]);
        ArenaFree(&n);
    }

    // Everything JSON's grammar does allow has to keep working.
    struct { const char *text; double want; } good[] = {
        { "{\"v\": 0}", 0.0 },
        { "{\"v\": -0}", 0.0 },
        { "{\"v\": 3}", 3.0 },
        { "{\"v\": -3}", -3.0 },
        { "{\"v\": 0.5}", 0.5 },
        { "{\"v\": -1.5e-3}", -0.0015 },
        { "{\"v\": 2E3}", 2000.0 },
        { "{\"v\": 2e+3}", 2000.0 },
        { "{\"v\": 12345.678}", 12345.678 },
        { "{\"v\": 1e-999}", 0.0 },          // underflow settles on zero
    };
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) {
        Arena n;
        ArenaInit(&n, 1 << 16, "num");
        JsonValue *r = P(&n, good[i].text, err, sizeof err);
        CHECK(r != NULL, "rejected valid JSON: %s (%s)", good[i].text, err);
        if (r) {
            double got = JsonNumberField(r, "v", -1.0);
            CHECK(got == good[i].want, "%s read as %g, expected %g",
                  good[i].text, got, good[i].want);
        }
        ArenaFree(&n);
    }

    // A number longer than the parser's stack buffer must convert exactly
    // rather than being truncated to whatever fitted.
    {
        char longNumber[512];
        int at = snprintf(longNumber, sizeof longNumber, "{\"v\": 1.");
        for (int i = 0; i < 400; i++) longNumber[at++] = '0';
        snprintf(longNumber + at, sizeof longNumber - (size_t)at, "5e2}");
        Arena n;
        ArenaInit(&n, 1 << 16, "long");
        JsonValue *r = P(&n, longNumber, err, sizeof err);
        CHECK(r != NULL, "rejected a very long but valid number (%s)", err);
        if (r) {
            CHECK(JsonNumberField(r, "v", -1.0) == 100.0,
                  "long number read as %g, expected 100", JsonNumberField(r, "v", -1.0));
        }
        ArenaFree(&n);
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
