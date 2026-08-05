#include "engine/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct Parser {
    const char *start;
    const char *p;
    const char *end;
    Arena *arena;
    char *err;
    int errSize;
    bool failed;
    int depth;
} Parser;

// Linked-list scratch nodes: arrays and objects are gathered as a list and then
// flattened into a contiguous block, which avoids needing a growable allocator.
typedef struct ItemNode { JsonValue value; struct ItemNode *next; } ItemNode;
typedef struct MemberNode { JsonMember member; struct MemberNode *next; } MemberNode;

static bool ParseValue(Parser *ps, JsonValue *out);

// Records the first failure only; later errors are cascade noise.
static void Fail(Parser *ps, const char *what)
{
    if (ps->failed) return;
    ps->failed = true;
    if (ps->err && ps->errSize > 0) {
        long line = 1, col = 1;
        for (const char *c = ps->start; c < ps->p && c < ps->end; c++) {
            if (*c == '\n') { line++; col = 1; } else { col++; }
        }
        snprintf(ps->err, (size_t)ps->errSize, "%s (line %ld, column %ld)", what, line, col);
    }
}

static void SkipWhitespace(Parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ps->p++;
        } else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '/') {
            while (ps->p < ps->end && *ps->p != '\n') ps->p++;
        } else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '*') {
            ps->p += 2;
            while (ps->p + 1 < ps->end && !(ps->p[0] == '*' && ps->p[1] == '/')) ps->p++;
            ps->p = (ps->p + 2 < ps->end) ? ps->p + 2 : ps->end;
        } else {
            break;
        }
    }
}

static bool Match(Parser *ps, char c)
{
    SkipWhitespace(ps);
    if (ps->p < ps->end && *ps->p == c) { ps->p++; return true; }
    return false;
}

static void EncodeUtf8(unsigned int cp, char **dst)
{
    if (cp < 0x80) {
        *(*dst)++ = (char)cp;
    } else if (cp < 0x800) {
        *(*dst)++ = (char)(0xC0 | (cp >> 6));
        *(*dst)++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *(*dst)++ = (char)(0xE0 | (cp >> 12));
        *(*dst)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*dst)++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *(*dst)++ = (char)(0xF0 | (cp >> 18));
        *(*dst)++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *(*dst)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*dst)++ = (char)(0x80 | (cp & 0x3F));
    }
}

static int HexQuad(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v * 16 + d;
    }
    return v;
}

// Assumes the opening quote has been consumed.
static bool ParseStringBody(Parser *ps, const char **outChars, int *outLength)
{
    const char *start = ps->p;
    size_t worst = 0;
    // First scan to the closing quote so the exact buffer size is known.
    const char *scan = start;
    while (scan < ps->end && *scan != '"') {
        if (*scan == '\\') {
            if (scan + 1 >= ps->end) break;
            scan += 2;
            worst += 4;   // a \uXXXX escape expands to at most 4 UTF-8 bytes
        } else {
            scan++;
            worst++;
        }
    }
    if (scan >= ps->end || *scan != '"') { Fail(ps, "unterminated string"); return false; }

    char *buf = (char *)ArenaAlloc(ps->arena, worst + 1);
    if (!buf) { Fail(ps, "out of arena memory"); return false; }
    char *dst = buf;

    while (ps->p < scan) {
        char c = *ps->p;
        if (c != '\\') { *dst++ = c; ps->p++; continue; }
        ps->p++;
        if (ps->p >= scan) break;
        char e = *ps->p++;
        switch (e) {
            case '"':  *dst++ = '"';  break;
            case '\\': *dst++ = '\\'; break;
            case '/':  *dst++ = '/';  break;
            case 'b':  *dst++ = '\b'; break;
            case 'f':  *dst++ = '\f'; break;
            case 'n':  *dst++ = '\n'; break;
            case 'r':  *dst++ = '\r'; break;
            case 't':  *dst++ = '\t'; break;
            case 'u': {
                if (ps->p + 4 > scan) { Fail(ps, "truncated \\u escape"); return false; }
                int hi = HexQuad(ps->p);
                if (hi < 0) { Fail(ps, "bad \\u escape"); return false; }
                ps->p += 4;
                unsigned int cp = (unsigned int)hi;
                // Combine a surrogate pair when the low half follows.
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->p + 6 <= scan &&
                    ps->p[0] == '\\' && ps->p[1] == 'u') {
                    int lo = HexQuad(ps->p + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned int)lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                EncodeUtf8(cp, &dst);
                break;
            }
            default: Fail(ps, "unknown escape"); return false;
        }
    }
    *dst = '\0';
    ps->p = scan + 1;   // step past the closing quote
    *outChars = buf;
    *outLength = (int)(dst - buf);
    return true;
}

static bool ParseNumber(Parser *ps, JsonValue *out)
{
    char *endp = NULL;
    // strtod needs a NUL-terminated buffer; JSON numbers are short so copy locally.
    char tmp[64];
    size_t n = (size_t)(ps->end - ps->p);
    if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    memcpy(tmp, ps->p, n);
    tmp[n] = '\0';

    double v = strtod(tmp, &endp);
    if (endp == tmp) { Fail(ps, "invalid number"); return false; }
    ps->p += (endp - tmp);
    out->type = JSON_NUMBER;
    out->as.number = v;
    return true;
}

static bool ParseArray(Parser *ps, JsonValue *out)
{
    ItemNode *head = NULL, *tail = NULL;
    int count = 0;

    SkipWhitespace(ps);
    if (!Match(ps, ']')) {
        for (;;) {
            ItemNode *node = (ItemNode *)ArenaAlloc(ps->arena, sizeof(ItemNode));
            if (!node) { Fail(ps, "out of arena memory"); return false; }
            if (!ParseValue(ps, &node->value)) return false;
            node->next = NULL;
            if (tail) tail->next = node; else head = node;
            tail = node;
            count++;

            SkipWhitespace(ps);
            if (Match(ps, ',')) continue;
            if (Match(ps, ']')) break;
            Fail(ps, "expected ',' or ']'");
            return false;
        }
    }

    JsonValue *items = NULL;
    if (count > 0) {
        items = (JsonValue *)ArenaAlloc(ps->arena, sizeof(JsonValue) * (size_t)count);
        if (!items) { Fail(ps, "out of arena memory"); return false; }
        int i = 0;
        for (ItemNode *n = head; n; n = n->next) items[i++] = n->value;
    }
    out->type = JSON_ARRAY;
    out->as.array.items = items;
    out->as.array.count = count;
    return true;
}

static bool ParseObject(Parser *ps, JsonValue *out)
{
    MemberNode *head = NULL, *tail = NULL;
    int count = 0;

    SkipWhitespace(ps);
    if (!Match(ps, '}')) {
        for (;;) {
            SkipWhitespace(ps);
            if (!Match(ps, '"')) { Fail(ps, "expected object key"); return false; }

            MemberNode *node = (MemberNode *)ArenaAlloc(ps->arena, sizeof(MemberNode));
            if (!node) { Fail(ps, "out of arena memory"); return false; }
            if (!ParseStringBody(ps, &node->member.key, &node->member.keyLength)) return false;
            if (!Match(ps, ':')) { Fail(ps, "expected ':'"); return false; }
            if (!ParseValue(ps, &node->member.value)) return false;
            node->next = NULL;
            if (tail) tail->next = node; else head = node;
            tail = node;
            count++;

            SkipWhitespace(ps);
            if (Match(ps, ',')) continue;
            if (Match(ps, '}')) break;
            Fail(ps, "expected ',' or '}'");
            return false;
        }
    }

    JsonMember *members = NULL;
    if (count > 0) {
        members = (JsonMember *)ArenaAlloc(ps->arena, sizeof(JsonMember) * (size_t)count);
        if (!members) { Fail(ps, "out of arena memory"); return false; }
        int i = 0;
        for (MemberNode *n = head; n; n = n->next) members[i++] = n->member;
    }
    out->type = JSON_OBJECT;
    out->as.object.members = members;
    out->as.object.count = count;
    return true;
}

static bool ParseValue(Parser *ps, JsonValue *out)
{
    if (ps->failed) return false;
    if (++ps->depth > JSON_MAX_DEPTH) { Fail(ps, "nesting too deep"); return false; }

    SkipWhitespace(ps);
    if (ps->p >= ps->end) { Fail(ps, "unexpected end of input"); ps->depth--; return false; }

    bool ok = false;
    char c = *ps->p;
    size_t remaining = (size_t)(ps->end - ps->p);

    if (c == '{') { ps->p++; ok = ParseObject(ps, out); }
    else if (c == '[') { ps->p++; ok = ParseArray(ps, out); }
    else if (c == '"') {
        ps->p++;
        out->type = JSON_STRING;
        ok = ParseStringBody(ps, &out->as.string.chars, &out->as.string.length);
    }
    else if (remaining >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4; out->type = JSON_BOOL; out->as.boolean = true; ok = true;
    }
    else if (remaining >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5; out->type = JSON_BOOL; out->as.boolean = false; ok = true;
    }
    else if (remaining >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4; out->type = JSON_NULL; ok = true;
    }
    else if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
        ok = ParseNumber(ps, out);
    }
    else {
        Fail(ps, "unexpected character");
    }

    ps->depth--;
    return ok;
}

JsonValue *JsonParse(Arena *arena, const char *text, size_t length, char *err, int errSize)
{
    if (err && errSize > 0) err[0] = '\0';

    Parser ps = {
        .start = text, .p = text, .end = text + length, .arena = arena,
        .err = err, .errSize = errSize, .failed = false, .depth = 0,
    };

    JsonValue *root = (JsonValue *)ArenaAlloc(arena, sizeof(JsonValue));
    if (!root) {
        if (err && errSize > 0) snprintf(err, (size_t)errSize, "out of arena memory");
        return NULL;
    }
    if (!ParseValue(&ps, root)) {
        if (err && errSize > 0 && err[0] == '\0') snprintf(err, (size_t)errSize, "parse failed");
        return NULL;
    }
    SkipWhitespace(&ps);
    if (ps.p != ps.end) {
        if (err && errSize > 0) {
            snprintf(err, (size_t)errSize, "trailing data at offset %ld", (long)(ps.p - text));
        }
        return NULL;
    }
    return root;
}

const JsonValue *JsonGet(const JsonValue *object, const char *key)
{
    if (!object || object->type != JSON_OBJECT || !key) return NULL;
    size_t len = strlen(key);
    for (int i = 0; i < object->as.object.count; i++) {
        const JsonMember *m = &object->as.object.members[i];
        if ((size_t)m->keyLength == len && memcmp(m->key, key, len) == 0) return &m->value;
    }
    return NULL;
}

int JsonCount(const JsonValue *array)
{
    if (!array) return 0;
    if (array->type == JSON_ARRAY) return array->as.array.count;
    if (array->type == JSON_OBJECT) return array->as.object.count;
    return 0;
}

const JsonValue *JsonAt(const JsonValue *array, int index)
{
    if (!array || array->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= array->as.array.count) return NULL;
    return &array->as.array.items[index];
}

double JsonNumber(const JsonValue *value, double fallback)
{
    if (!value) return fallback;
    if (value->type == JSON_NUMBER) return value->as.number;
    if (value->type == JSON_BOOL) return value->as.boolean ? 1.0 : 0.0;
    return fallback;
}

bool JsonBool(const JsonValue *value, bool fallback)
{
    if (!value) return fallback;
    if (value->type == JSON_BOOL) return value->as.boolean;
    if (value->type == JSON_NUMBER) return value->as.number != 0.0;
    return fallback;
}

const char *JsonString(const JsonValue *value, const char *fallback)
{
    if (!value || value->type != JSON_STRING) return fallback;
    return value->as.string.chars;
}

bool JsonFloats(const JsonValue *array, float *out, int count)
{
    if (!array || array->type != JSON_ARRAY) return false;
    if (array->as.array.count < count) return false;
    for (int i = 0; i < count; i++) {
        out[i] = (float)JsonNumber(&array->as.array.items[i], out[i]);
    }
    return true;
}

bool JsonFloatsField(const JsonValue *object, const char *key, float *out, int count)
{
    return JsonFloats(JsonGet(object, key), out, count);
}

double JsonNumberField(const JsonValue *object, const char *key, double fallback)
{
    return JsonNumber(JsonGet(object, key), fallback);
}

const char *JsonStringField(const JsonValue *object, const char *key, const char *fallback)
{
    return JsonString(JsonGet(object, key), fallback);
}
