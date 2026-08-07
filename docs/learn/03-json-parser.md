# 03 — Writing a JSON parser

> `engine/include/engine/json.h` · `engine/src/json.c` — 401 lines.
> Tests: `tests/test_json.c`.

---

## The problem

Levels are data. Data needs a file format. The format needs a reader.

The obvious move is to link a library — cJSON, jsmn, nlohmann. This project
writes its own, in 400 lines, for three reasons:

1. **Allocation control.** Every third-party parser calls `malloc`. This one
   allocates only from a caller-supplied `Arena`, which is what makes the level
   loader's two-arena pattern (Chapter 02) possible and the exhaustion test
   trivial.
2. **Comments.** Strict JSON forbids them. Hand-edited level files want them.
3. **It is a genuinely small problem.** JSON's grammar fits on a postcard. A
   recursive-descent parser for it is a canonical exercise, and writing one
   teaches you more about parsing than using one ever will.

The cost is that you now own the bugs. Chapter 14 covers how the tests
compensate.

---

## The JSON grammar

Here is the entire language, in about the form you would implement it:

```
value   := object | array | string | number | "true" | "false" | "null"
object  := '{' [ string ':' value { ',' string ':' value } ] '}'
array   := '[' [ value { ',' value } ] ']'
string  := '"' { char | escape } '"'
escape  := '\' ( '"' | '\' | '/' | 'b' | 'f' | 'n' | 'r' | 't' | 'u' hex hex hex hex )
number  := [ '-' ] int [ frac ] [ exp ]
```

Two properties make this pleasant to parse:

- **It is LL(1).** One character of lookahead tells you which production to
  take: `{` means object, `[` means array, `"` means string, `t`/`f`/`n` mean
  the literals, and a digit or `-` means number. No backtracking is ever needed.
- **It is recursive.** Values contain values. Which means the natural
  implementation is a set of mutually recursive functions — hence *recursive
  descent*.

---

## The value representation

```c
// engine/include/engine/json.h
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
```

A tagged union: one `type` field says which arm of `as` is live. This is how you
write a sum type in C.

Three deliberate choices here:

**Strings carry both a length and a NUL.** The length lets `JsonGet` compare
keys without `strlen`, and lets strings contain embedded NULs (which JSON
permits, spelled `\u0000`). The NUL lets `JsonString` hand a pointer straight to
`strcmp` and `snprintf`. Storing both costs one byte and removes a whole class
of caller annoyance.

**Arrays and objects are contiguous, not linked.** `items` is a flat array of
`JsonValue`, not a list. That makes `JsonAt(array, i)` an O(1) index and keeps
iteration cache-friendly. Getting there requires a trick, covered below.

**`JsonMember` embeds its `JsonValue` by value**, not by pointer. One fewer
indirection, one fewer allocation.

---

## The parser state

```c
// engine/src/json.c
typedef struct Parser {
    const char *start;   // for computing line/column on error
    const char *p;       // cursor
    const char *end;     // one past the last byte
    Arena *arena;
    char *err;
    int errSize;
    bool failed;
    int depth;
} Parser;
```

Note there is **no NUL-termination assumption anywhere**. The parser works on a
`(pointer, length)` range. Every loop tests `ps->p < ps->end` before
dereferencing. This is not fussiness: `LoadFileData` returns a buffer whose
length you are told, and assuming a terminator is how parsers read past the end
of heap blocks.

---

## Whitespace, and the comment extension

```c
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
```

The four JSON whitespace characters, plus both comment forms. Putting comments
in the whitespace skipper rather than in a separate tokeniser pass means they
are legal *anywhere* whitespace is — between a key and its colon, inside an
array, after the closing brace.

The bounds checks are the interesting part. `ps->p + 1 < ps->end` before reading
`ps->p[1]`; the block-comment loop stops with one byte to spare and then clamps.
A `/*` at the very end of the file with no terminator leaves `p == end` rather
than running off. This is the kind of thing the malformed-input test list exists
to catch.

---

## Strings: two passes and why

This is the most subtle function in the file.

```c
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
    /* ... second pass: decode escapes into buf ... */
}
```

**Why two passes?** Because the arena cannot grow an allocation. You must know
the size before you ask for it. So pass one measures a safe upper bound; pass
two fills.

**Why an upper bound rather than the exact size?** Computing the exact size
would mean decoding the escapes in pass one too — doing the work twice. The
bound is cheap: any escape sequence is at least two source bytes and produces at
most four output bytes (a `\uXXXX` outside the BMP, via a surrogate pair, is
twelve source bytes producing four output bytes, so the bound holds comfortably).
Over-allocating a few bytes in an arena is free.

**The `scan + 1 >= ps->end` check** handles a backslash as the final byte of the
file, which would otherwise skip `scan` past `end` and turn the loop's bounds
test into a comparison of unrelated pointers.

### UTF-8 encoding

```c
static void EncodeUtf8(unsigned int cp, char **dst)
{
    if (cp < 0x80) {
        *(*dst)++ = (char)cp;
    } else if (cp < 0x800) {
        *(*dst)++ = (char)(0xC0 | (cp >> 6));
        *(*dst)++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *(*dst)++ = (char)(0xE0 | (cp >> 12));
        /* ... */
    } else {
        *(*dst)++ = (char)(0xF0 | (cp >> 18));
        /* ... */
    }
}
```

UTF-8's design in one function. A code point becomes 1–4 bytes:

| Code point | Bytes | Pattern |
|---|---|---|
| `U+0000`–`U+007F` | 1 | `0xxxxxxx` |
| `U+0080`–`U+07FF` | 2 | `110xxxxx 10xxxxxx` |
| `U+0800`–`U+FFFF` | 3 | `1110xxxx 10xxxxxx 10xxxxxx` |
| `U+10000`–`U+10FFFF` | 4 | `11110xxx 10xxxxxx 10xxxxxx 10xxxxxx` |

The leading byte's high bits encode the length; continuation bytes always start
`10`. That is what makes UTF-8 self-synchronising: from any byte you can find
the start of the character by scanning backwards past anything matching `10xxxxxx`.

`0xC0 | (cp >> 6)` sets the two-byte marker and the top five payload bits;
`0x80 | (cp & 0x3F)` sets the continuation marker and the low six. Every branch
is the same shape.

### Surrogate pairs

```c
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
```

Historical baggage worth understanding, because it appears everywhere.

JSON's `\u` escape is defined in terms of UTF-16 code *units*, not Unicode code
points. UTF-16 represents code points above `U+FFFF` as a pair of 16-bit units
drawn from reserved ranges: a **high surrogate** in `D800`–`DBFF` and a **low
surrogate** in `DC00`–`DFFF`. Neither half is a valid character on its own.

So `"😀"` is not two characters; it is one — 😀, `U+1F600`. The
reconstruction is:

```
cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00)
```

Ten bits from each half, offset by `0x10000`, giving exactly the `U+10000`–`U+10FFFF`
range.

The code is careful to only consume the second escape if it really is a low
surrogate. An unpaired high surrogate is encoded on its own — technically
invalid UTF-8, but a lenient choice that avoids rejecting a level file over a
character in a track name.

---

## Numbers

```c
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
```

Delegating to `strtod` is the right call. Correctly-rounded decimal-to-binary
conversion is genuinely hard (see: the `0.1` problem, Steele & White's dragon4,
Gay's `strtod`), and the standard library has a tested implementation.

The awkwardness is that `strtod` requires a NUL-terminated string and the parser
has a range. Copying up to 63 bytes into a stack buffer solves it. 63 is
comfortably more than any real JSON number: the longest sensible one,
`-1.7976931348623157e+308`, is 24 characters.

`endp == tmp` means `strtod` consumed nothing, i.e. the text was not a number at
all. That is the only failure mode worth reporting; overflow yields `HUGE_VAL`
and underflow yields zero, which are acceptable for level data.

Note this accepts a leading `+`, which strict JSON forbids. `ParseValue`'s
dispatch includes `c == '+'`. A deliberate leniency, consistent with allowing
comments.

---

## Arrays and objects: the linked-list-then-flatten trick

Here is the problem. To allocate a contiguous array of `n` elements you must
know `n`. To know `n` you must parse the array. To parse the array you must
store the elements somewhere. But you cannot grow an arena allocation.

The solution:

```c
// Linked-list scratch nodes: arrays and objects are gathered as a list and then
// flattened into a contiguous block, which avoids needing a growable allocator.
typedef struct ItemNode { JsonValue value; struct ItemNode *next; } ItemNode;
```

Parse into a singly-linked list, counting as you go. Then allocate the exact
array and copy across:

```c
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
```

The list nodes are never freed — they are arena garbage, dead the moment the
copy loop finishes, and reclaimed when the whole scratch arena goes. That is why
the level loader budgets `fileSize * 12`: this trick roughly doubles peak usage
for arrays.

Trading memory for simplicity is the correct call here, because the memory is
scratch and bounded by the file size, while the alternative (a growable vector
with realloc semantics inside an arena) is a whole extra data structure.

**The `if (tail) tail->next = node; else head = node;` idiom** is worth
recognising: it appends to a singly-linked list in O(1) without a dummy head
node. You will write it a hundred times.

**Empty containers are handled by the early `Match(ps, ']')`**, before the loop.
Without it, `[]` would enter the loop and try to parse a value where `]` is,
producing a spurious error. `tests/test_json.c` checks `"empty_arr": []` and
`"empty_obj": {}` for exactly this reason.

`ParseObject` is the same shape with a key-parse and a colon-match in front.

---

## Dispatch and the depth guard

```c
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
    else if (c == '"') { ps->p++; out->type = JSON_STRING;
                         ok = ParseStringBody(ps, &out->as.string.chars, &out->as.string.length); }
    else if (remaining >= 4 && memcmp(ps->p, "true", 4) == 0) { /* ... */ }
    else if (remaining >= 5 && memcmp(ps->p, "false", 5) == 0) { /* ... */ }
    else if (remaining >= 4 && memcmp(ps->p, "null", 4) == 0) { /* ... */ }
    else if (c == '-' || c == '+' || (c >= '0' && c <= '9')) { ok = ParseNumber(ps, out); }
    else { Fail(ps, "unexpected character"); }

    ps->depth--;
    return ok;
}
```

The LL(1) dispatch, one branch per production.

**The depth guard is a security control, not a nicety.** Recursive descent uses
the C stack for nesting. A file containing 100,000 `[` characters would recurse
100,000 deep and smash the stack — a segfault at best, a stack-clash exploit
primitive at worst. `JSON_MAX_DEPTH` is 64, far more than any real level file
needs (the deepest path here is root → `props` → element → `pos` → number, which
is 4) and far less than the stack can take.

`tests/test_json.c` fires 4,095 open brackets at it:

```c
char deep[4096];
memset(deep, '[', sizeof deep - 1);
deep[sizeof deep - 1] = '\0';
CHECK(P(&tmp, deep, err, sizeof err) == NULL, "deep nesting rejected");
```

**The `remaining >= 4` guards before `memcmp`** stop a file ending in `tru` from
reading past the buffer. `tests/test_json.c` includes `"tru"` in its list of
inputs that must be rejected.

**`if (ps->failed) return false;` at the top** short-circuits the whole
recursion once anything has failed, so a deeply nested error unwinds without
each level trying to parse more garbage.

---

## Error reporting

```c
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
```

Two ideas here.

**First error wins.** Once a parse has gone wrong, every subsequent error is a
consequence, not a cause. Reporting the last one you hit points the user at the
wrong place. Compilers that report "47 errors" from one missing brace are making
this mistake.

**Line and column are computed lazily**, by rescanning from the start, only when
an error actually occurs. The alternative — tracking line and column on every
character — costs two increments and a branch in the hottest loop in the parser,
forever, to serve a case that happens at most once per file. Rescanning is O(n)
but happens zero times in the common case.

That is a general principle: **push cost onto the rare path.**

---

## The tolerant accessor layer

The parser produces a tree. Reading it is a second API, and its design is what
makes the level loader readable:

```c
const JsonValue *JsonGet(const JsonValue *object, const char *key);
int JsonCount(const JsonValue *array);
const JsonValue *JsonAt(const JsonValue *array, int index);

double JsonNumber(const JsonValue *value, double fallback);
bool JsonBool(const JsonValue *value, bool fallback);
const char *JsonString(const JsonValue *value, const char *fallback);
bool JsonFloats(const JsonValue *array, float *out, int count);
```

**Every accessor tolerates `NULL` and type mismatches.** Look:

```c
double JsonNumber(const JsonValue *value, double fallback)
{
    if (!value) return fallback;
    if (value->type == JSON_NUMBER) return value->as.number;
    if (value->type == JSON_BOOL) return value->as.boolean ? 1.0 : 0.0;
    return fallback;
}
```

`JsonGet` on a missing key returns `NULL`; `JsonNumber(NULL, 3)` returns 3. So
this composes:

```c
level->laps = (int)JsonNumberField(settings, "laps", 3);
```

If `settings` is missing entirely, if it is present but has no `laps`, or if
`laps` is a string — the result is 3, with no branch at the call site. Compare
what that line would look like with a strict API:

```c
int laps = 3;
const JsonValue *s = JsonGet(root, "settings");
if (s && s->type == JSON_OBJECT) {
    const JsonValue *l = JsonGet(s, "laps");
    if (l && l->type == JSON_NUMBER) laps = (int)l->as.number;
}
```

Six lines instead of one, times forty fields. The tolerant design is why
`level.c` reads as a flat list of assignments.

**The tradeoff, stated honestly:** a typo in a key name becomes a silent default
rather than an error. That is the right trade for a level format where every
field is optional by design and the exporter is the primary author — but it
would be the wrong trade for, say, a configuration file where a misspelled key
should be loud. The mitigation here is `Validate Level` in the Blender add-on
and the invariant tests in `tests/test_race.c`, which check the *result* rather
than the syntax.

`JsonFloats` is the vector reader, and note it leaves `out` untouched on a miss:

```c
bool JsonFloats(const JsonValue *array, float *out, int count)
{
    if (!array || array->type != JSON_ARRAY) return false;
    if (array->as.array.count < count) return false;
    for (int i = 0; i < count; i++) {
        out[i] = (float)JsonNumber(&array->as.array.items[i], out[i]);
    }
    return true;
}
```

The caller pre-fills `out` with defaults, so `ReadVec3` is three lines:

```c
// engine/src/level.c
static Vector3 ReadVec3(const JsonValue *obj, const char *key, Vector3 fallback)
{
    float v[3] = { fallback.x, fallback.y, fallback.z };
    JsonFloatsField(obj, key, v, 3);
    return (Vector3){ v[0], v[1], v[2] };
}
```

And `ReadColor` uses the same property to accept both 3- and 4-element colours:

```c
const JsonValue *arr = JsonGet(obj, key);
// Alpha is optional, so accept both 3- and 4-element colours.
if (!JsonFloats(arr, v, 4)) JsonFloats(arr, v, 3);
```

Try the four-element read; if the array is too short, fall back to three, leaving
`v[3]` at the default alpha.

---

## Trailing data

```c
JsonValue *JsonParse(Arena *arena, const char *text, size_t length, char *err, int errSize)
{
    /* ... parse the root value ... */
    SkipWhitespace(&ps);
    if (ps.p != ps.end) {
        if (err && errSize > 0) {
            snprintf(err, (size_t)errSize, "trailing data at offset %ld", (long)(ps.p - text));
        }
        return NULL;
    }
    return root;
}
```

Easy to forget. Without this check, `{"a":1} garbage` parses "successfully",
because `ParseValue` is perfectly happy having consumed `{"a":1}` and stops
there. `tests/test_json.c` has `"{\"a\":1} trailing"` in its rejection list.

---

## The malformed-input corpus

The single highest-value part of the test file:

```c
// tests/test_json.c
const char *bad[] = {
    "{", "[", "{\"a\":}", "{\"a\" 1}", "[1,]", "{,}", "\"unterminated",
    "{\"a\":1} trailing", "tru", "[1 2]", "{\"a\":1,}", "",
};
for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    Arena tmp;
    ArenaInit(&tmp, 1 << 16, "bad");
    JsonValue *r = P(&tmp, bad[i], err, sizeof err);
    if (r != NULL) { /* fail */ }
    ArenaFree(&tmp);
}
```

Twelve inputs, each targeting a specific way a parser goes wrong:

| Input | The bug it catches |
|---|---|
| `{` | reading past end when an object is unterminated |
| `[` | same for arrays |
| `{"a":}` | accepting an empty value position |
| `{"a" 1}` | missing colon |
| `[1,]` | trailing comma in an array |
| `{,}` | comma where a key belongs |
| `"unterminated` | string running to EOF |
| `{"a":1} trailing` | not checking for trailing data |
| `tru` | `memcmp` past the end of the buffer |
| `[1 2]` | missing comma silently accepted |
| `{"a":1,}` | trailing comma in an object |
| `""` (empty) | empty input |

Note what these tests assert: **not a crash, and not accepted.** They do not
check the error message text, which would make them brittle. A parser that
rejects everything would pass this list — which is why the happy-path assertions
above it exist as the counterweight.

This is the cheapest security testing you will ever do. If you write a parser
for anything, write this list for it.

---

## Exercises

1. **Add trailing-comma tolerance.** Level files are hand-edited; a trailing
   comma is a natural mistake. Make `[1,2,3,]` parse. Then explain why the
   corresponding test in `bad[]` must be removed, and why you should think twice
   before doing any of this.

2. **Report the path.** Change `Fail` to also record the object keys and array
   indices on the way down, so an error reads
   `expected ',' or ']' at props[47].pos (line 812, column 30)`. What has to
   change in `Parser`, and what does it cost on the happy path?

3. **Measure the flatten cost.** Instrument `ArenaAlloc` to total bytes and
   parse `levels/circuit02.level.json`. How much of the peak is `ItemNode` and
   `MemberNode` scratch? Is the `fileSize * 12` budget in `level.c` generous or
   tight?

4. **Fuzz it.** Write a loop that generates 100,000 random byte strings of
   length 0–200 and feeds each to `JsonParse` with a 64 KB arena. Nothing should
   crash. Then mutate a real level file — flip one random byte per iteration —
   and do the same. This is the poor man's AFL and it finds real bugs.

5. **Reject invalid UTF-8 in surrogates.** Currently an unpaired high surrogate
   is encoded as-is, producing technically invalid UTF-8. Change it to emit
   `U+FFFD` (the replacement character) instead. Which byte sequence is that?

6. **A serialiser.** Write `JsonWrite(const JsonValue *, FILE *)` that prints a
   tree back out as valid JSON with correct escaping. Round-trip
   `circuit01.level.json` through parse → write → parse and check the two trees
   are structurally equal. What does "equal" mean for a `double`?

---

Next: [04 — Level format and the Blender pipeline](04-level-pipeline.md)
