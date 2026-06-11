#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xresource.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "util.h"

/* X resource manager. Each entry stores its pattern as parallel quark and
 * binding arrays so query-time matching is integer compare instead of
 * strcmp+malloc. Entries are linked into two structures: an ordered head/tail
 * list for enumeration / file output / O(N) destroy, and a hash bucket keyed by
 * the leaf quark for O(1)-average lookup.
 *
 * Motif issues hundreds of resource lookups per widget at startup (XmRendition
 * lists, XmFontList chains, XmNbaseTranslations). The flat linked-list +
 * strcmp-per-component variant this replaces turned a one-shot widget realize
 * into seconds of CPU; bucketing by leaf gives Motif's "lookup by leaf, score
 * by cascade" pattern a near-direct path.
 */

#define XRM_BUCKETS 256

typedef struct XrmEntry {
    XrmQuark *quarks; /* complen entries (no terminator) */
    /* complen entries; bindings[i] is the binding BEFORE quarks[i]. */
    XrmBinding *bindings;
    int complen;
    XrmQuark leaf; /* quarks[complen - 1], cached for bucketing. */
    char *type;
    char *value;
    unsigned int value_size; /* includes trailing NUL when value is text */
    struct XrmEntry *bnext;  /* bucket chain */
    struct XrmEntry *next;   /* ordered list link */
} XrmEntry;

struct _XrmHashBucketRec {
    XrmEntry *buckets[XRM_BUCKETS];
    XrmEntry *head;
    XrmEntry *tail;
};

/* Xlib documents resource paths up to 100 components; rounding to 128 accepts
 * the entire spec range without forcing libXt's _XtDisplayInitialize doubling
 * loop to give up on a legitimate deep widget tree.
 */
#define XRM_PREFIX_MAX 128

static unsigned int quarkBucket(XrmQuark q)
{
    /* Quarks are dense small integers; mix the high bits in so consecutive
     * quark IDs do not all land in the same bucket.
     */
    unsigned int u = (unsigned int) q;
    u ^= u >> 8;
    return u & (XRM_BUCKETS - 1);
}

static XrmQuark wildcardComponentQuark(void)
{
    static XrmQuark wildcard = NULLQUARK;
    if (wildcard == NULLQUARK)
        wildcard = XrmStringToQuark("?");
    return wildcard;
}

static Bool patternQuarkMatches(XrmQuark pattern,
                                XrmQuark name,
                                XrmQuark class,
                                Bool *nameMatch,
                                Bool *classMatch)
{
    XrmQuark wildcard = wildcardComponentQuark();
    *nameMatch = name != NULLQUARK && pattern == name;
    *classMatch = class != NULLQUARK && pattern == class;
    return *nameMatch || *classMatch || pattern == wildcard;
}

/* Parse a pattern string into a (binding, quark) sequence. Mirrors
 * XrmStringToBindingQuarkList's semantics: leading "*" flips binding to loose,
 * "." is tight, the binding of component i is determined by the separator that
 * preceded it.
 *
 * Returns the component count, or -1 on OOM.
 */
static int parsePattern(const char *pattern,
                        XrmQuark **quarks_out,
                        XrmBinding **bindings_out)
{
    *quarks_out = NULL;
    *bindings_out = NULL;
    if (!pattern)
        return 0;
    size_t cap = 8;
    XrmQuark *quarks = malloc(sizeof(XrmQuark) * cap);
    XrmBinding *bindings = malloc(sizeof(XrmBinding) * cap);
    if (!quarks || !bindings) {
        free(quarks);
        free(bindings);
        return -1;
    }
    int count = 0;
    XrmBinding binding = XrmBindTightly;
    const char *segment = pattern;
    const char *cursor = pattern;
    while (*cursor != '\0') {
        if (*cursor == '.' || *cursor == '*') {
            if (cursor != segment) {
                if ((size_t) count == cap) {
                    cap *= 2;
                    XrmQuark *nq = realloc(quarks, sizeof(XrmQuark) * cap);
                    XrmBinding *nb =
                        realloc(bindings, sizeof(XrmBinding) * cap);
                    if (!nq || !nb) {
                        free(nq ? nq : quarks);
                        free(nb ? nb : bindings);
                        return -1;
                    }
                    quarks = nq;
                    bindings = nb;
                }
                size_t len = (size_t) (cursor - segment);
                char *seg = malloc(len + 1);
                if (!seg) {
                    free(quarks);
                    free(bindings);
                    return -1;
                }
                memcpy(seg, segment, len);
                seg[len] = '\0';
                bindings[count] = binding;
                quarks[count] = XrmStringToQuark(seg);
                free(seg);
                count++;
                binding = XrmBindTightly;
            }
            if (*cursor == '*')
                binding = XrmBindLoosely;
            segment = cursor + 1;
        }
        cursor++;
    }
    if (cursor != segment) {
        if ((size_t) count == cap) {
            cap++;
            XrmQuark *nq = realloc(quarks, sizeof(XrmQuark) * cap);
            XrmBinding *nb = realloc(bindings, sizeof(XrmBinding) * cap);
            if (!nq || !nb) {
                free(nq ? nq : quarks);
                free(nb ? nb : bindings);
                return -1;
            }
            quarks = nq;
            bindings = nb;
        }
        size_t len = (size_t) (cursor - segment);
        char *seg = malloc(len + 1);
        if (!seg) {
            free(quarks);
            free(bindings);
            return -1;
        }
        memcpy(seg, segment, len);
        seg[len] = '\0';
        bindings[count] = binding;
        quarks[count] = XrmStringToQuark(seg);
        free(seg);
        count++;
    }
    *quarks_out = quarks;
    *bindings_out = bindings;
    return count;
}

static XrmEntry *xrmAllocEntry(const char *pattern,
                               const char *type,
                               const char *value,
                               unsigned int value_size)
{
    XrmQuark *quarks = NULL;
    XrmBinding *bindings = NULL;
    int complen = parsePattern(pattern, &quarks, &bindings);
    if (complen <= 0) {
        free(quarks);
        free(bindings);
        return NULL;
    }
    XrmEntry *e = calloc(1, sizeof(*e));
    if (!e) {
        free(quarks);
        free(bindings);
        return NULL;
    }
    e->quarks = quarks;
    e->bindings = bindings;
    e->complen = complen;
    e->leaf = quarks[complen - 1];
    e->type = strdup(type ? type : "String");
    e->value = malloc(value_size);
    if (!e->type || !e->value) {
        free(e->quarks);
        free(e->bindings);
        free(e->type);
        free(e->value);
        free(e);
        return NULL;
    }
    memcpy(e->value, value, value_size);
    e->value_size = value_size;
    return e;
}

static void xrmFreeEntry(XrmEntry *e)
{
    if (!e)
        return;
    free(e->quarks);
    free(e->bindings);
    free(e->type);
    free(e->value);
    free(e);
}

static Bool entryPatternEquals(const XrmEntry *e,
                               const XrmQuark *q,
                               const XrmBinding *b,
                               int n)
{
    if (e->complen != n)
        return False;
    for (int i = 0; i < n; i++) {
        if (e->quarks[i] != q[i] || e->bindings[i] != b[i])
            return False;
    }
    return True;
}

static XrmEntry *xrmFindEntryByPattern(XrmDatabase db,
                                       const XrmQuark *q,
                                       const XrmBinding *b,
                                       int n)
{
    if (!db || n <= 0)
        return NULL;
    unsigned int bucket = quarkBucket(q[n - 1]);
    for (XrmEntry *e = db->buckets[bucket]; e; e = e->bnext) {
        if (entryPatternEquals(e, q, b, n))
            return e;
    }
    return NULL;
}

static void xrmLinkEntry(XrmDatabase db, XrmEntry *e)
{
    unsigned int bucket = quarkBucket(e->leaf);
    e->bnext = db->buckets[bucket];
    db->buckets[bucket] = e;
    e->next = NULL;
    if (db->tail) {
        db->tail->next = e;
        db->tail = e;
    } else {
        db->head = db->tail = e;
    }
}

static void xrmReplaceEntryValue(XrmEntry *e,
                                 const char *type,
                                 const char *value,
                                 unsigned int value_size)
{
    char *newValue = malloc(value_size);
    char *newType = strdup(type ? type : "String");
    if (!newValue || !newType) {
        free(newValue);
        free(newType);
        return;
    }
    memcpy(newValue, value, value_size);
    free(e->value);
    free(e->type);
    e->value = newValue;
    e->type = newType;
    e->value_size = value_size;
}

static void xrmInsertOrReplaceFromPattern(XrmDatabase db,
                                          const char *pattern,
                                          const char *type,
                                          const char *value,
                                          unsigned int value_size)
{
    XrmQuark *q = NULL;
    XrmBinding *b = NULL;
    int n = parsePattern(pattern, &q, &b);
    if (n <= 0) {
        free(q);
        free(b);
        return;
    }
    XrmEntry *existing = xrmFindEntryByPattern(db, q, b, n);
    free(q);
    free(b);
    if (existing) {
        xrmReplaceEntryValue(existing, type, value, value_size);
        return;
    }
    XrmEntry *e = xrmAllocEntry(pattern, type, value, value_size);
    if (!e)
        return;
    xrmLinkEntry(db, e);
}

static XrmDatabase xrmNewDatabase(void)
{
    return calloc(1, sizeof(struct _XrmHashBucketRec));
}

void XrmInitialize(void)
{
    /* Nothing to set up: the quark table lives in libX11's Quarks.c (staged
     * under $(OUT)/upstream/src/) and initializes on first use.
     */
}

/* The XrmStringToQuarkList / XrmStringToBindingQuarkList implementations below
 * derive from libX11's src/Xrm.c (which we do not compile because its
 * surrounding database code is replaced by the simpler routines in this file).
 * The original upstream code is:
 *
 *     Copyright 1987, 1988, 1990, 1991, 1998 The Open Group
 *
 * See doc/COPYING in the libX11 source tree for the full notice; the MIT-style
 * terms there are compatible with this project's own MIT license.
 */
static XrmQuark quarkFromSegment(const char *start, size_t length)
{
    char *segment = Xmalloc(length + 1);
    if (segment == NULL)
        return NULLQUARK;
    memcpy(segment, start, length);
    segment[length] = '\0';
    XrmQuark quark = XrmStringToQuark(segment);
    Xfree(segment);
    return quark;
}

void XrmStringToQuarkList(register _Xconst char *name,
                          register XrmQuarkList quarks)
{
    if (name != NULL) {
        const char *segment = name;
        const char *cursor = name;
        while (*cursor != '\0') {
            if (*cursor == '.' || *cursor == '*') {
                if (cursor != segment)
                    *quarks++ =
                        quarkFromSegment(segment, (size_t) (cursor - segment));
                segment = cursor + 1;
            }
            cursor++;
        }
        if (cursor != segment)
            *quarks++ = quarkFromSegment(segment, (size_t) (cursor - segment));
    }
    *quarks = NULLQUARK;
}

void XrmStringToBindingQuarkList(register _Xconst char *name,
                                 register XrmBindingList bindings,
                                 register XrmQuarkList quarks)
{
    XrmBinding binding = XrmBindTightly;
    if (name != NULL) {
        const char *segment = name;
        const char *cursor = name;
        while (*cursor != '\0') {
            if (*cursor == '.' || *cursor == '*') {
                if (cursor != segment) {
                    *bindings++ = binding;
                    *quarks++ =
                        quarkFromSegment(segment, (size_t) (cursor - segment));
                    binding = XrmBindTightly;
                }
                if (*cursor == '*')
                    binding = XrmBindLoosely;
                segment = cursor + 1;
            }
            cursor++;
        }
        if (cursor != segment) {
            *bindings++ = binding;
            *quarks++ = quarkFromSegment(segment, (size_t) (cursor - segment));
        }
    }
    *quarks = NULLQUARK;
}

void XrmDestroyDatabase(XrmDatabase db)
{
    if (!db)
        return;
    XrmEntry *e = db->head;
    while (e) {
        XrmEntry *next = e->next;
        xrmFreeEntry(e);
        e = next;
    }
    free(db);
}

/* Decode the X resource value escape sequences defined in the Xlib spec
 * (appendix "Resource Manager Specifications"):
 *
 *   \n           -> 0x0a
 *   \t           -> 0x09
 *   \r           -> 0x0d
 *   \\           -> 0x5c
 *   \<3 octals>  -> the byte whose octal code is given
 *   \<other>     -> keep both chars verbatim
 *
 * Motif resource files (and any *.translations resource it consumes) encode
 * literal newlines as the two-character sequence "\n"; without this decode
 * XtParseTranslationTable receives a backslash-n token and fails to split
 * rules.
 *
 * Returns the decoded length. "dst" must be at least "srcLen" bytes; decoded
 * length is always <= srcLen.
 */
static size_t xrmDecodeValueEscapes(char *dst, const char *src, size_t srcLen)
{
    size_t di = 0;
    for (size_t si = 0; si < srcLen;) {
        if (src[si] != '\\' || si + 1 >= srcLen) {
            dst[di++] = src[si++];
            continue;
        }
        char next = src[si + 1];
        if (next == 'n') {
            dst[di++] = '\n';
            si += 2;
        } else if (next == 't') {
            dst[di++] = '\t';
            si += 2;
        } else if (next == 'r') {
            dst[di++] = '\r';
            si += 2;
        } else if (next == '\\') {
            dst[di++] = '\\';
            si += 2;
        } else if (next >= '0' && next <= '7' && si + 3 < srcLen &&
                   src[si + 2] >= '0' && src[si + 2] <= '7' &&
                   src[si + 3] >= '0' && src[si + 3] <= '7') {
            int v = ((next - '0') << 6) | ((src[si + 2] - '0') << 3) |
                    (src[si + 3] - '0');
            dst[di++] = (char) v;
            si += 4;
        } else {
            dst[di++] = src[si++];
        }
    }
    return di;
}

/* Split a resource line "name: value" (or "name*foo: value") into pattern and
 * value, ignoring leading whitespace and one ':'. The value is decoded per
 * xrmDecodeValueEscapes so stored bytes are the literal characters Xt/Motif
 * consumers expect.
 */
static int parseLine(const char *line, char **pattern_out, char **value_out)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '!' || *line == '#' || *line == '\0')
        return 0;
    const char *colon = strchr(line, ':');
    if (!colon)
        return 0;
    const char *end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\n' || end[-1] == '\r')) {
        end--;
    }
    const char *patEnd = colon;
    while (patEnd > line && (patEnd[-1] == ' ' || patEnd[-1] == '\t'))
        patEnd--;
    if (patEnd == line)
        return 0;
    const char *valStart = colon + 1;
    while (valStart < end && (*valStart == ' ' || *valStart == '\t'))
        valStart++;
    size_t patLen = (size_t) (patEnd - line);
    size_t valLen = (size_t) (end - valStart);
    char *pat = malloc(patLen + 1);
    char *val = malloc(valLen + 1);
    if (!pat || !val) {
        free(pat);
        free(val);
        return 0;
    }
    memcpy(pat, line, patLen);
    pat[patLen] = '\0';
    size_t decoded = xrmDecodeValueEscapes(val, valStart, valLen);
    val[decoded] = '\0';
    *pattern_out = pat;
    *value_out = val;
    return 1;
}

void XrmPutLineResource(XrmDatabase *pdb, _Xconst char *line)
{
    if (!pdb || !line)
        return;
    if (!*pdb)
        *pdb = xrmNewDatabase();
    if (!*pdb)
        return;
    char *pattern = NULL;
    char *value = NULL;
    if (!parseLine(line, &pattern, &value))
        return;
    xrmInsertOrReplaceFromPattern(*pdb, pattern, "String", value,
                                  (unsigned int) strlen(value) + 1);
    free(pattern);
    free(value);
}

void XrmPutStringResource(XrmDatabase *pdb,
                          _Xconst char *specifier,
                          _Xconst char *value)
{
    if (!pdb || !specifier || !value)
        return;
    if (!*pdb)
        *pdb = xrmNewDatabase();
    if (!*pdb)
        return;
    xrmInsertOrReplaceFromPattern(*pdb, specifier, "String", value,
                                  (unsigned int) strlen(value) + 1);
}

void XrmPutResource(XrmDatabase *pdb,
                    _Xconst char *specifier,
                    _Xconst char *type,
                    XrmValue *value)
{
    if (!pdb || !specifier || !value)
        return;
    if (!*pdb)
        *pdb = xrmNewDatabase();
    if (!*pdb)
        return;
    xrmInsertOrReplaceFromPattern(*pdb, specifier, type,
                                  (const char *) value->addr, value->size);
}

/* Strip trailing CR/LF in place; returns the new length. */
static size_t rstripNewline(char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    return len;
}

/* Join one logical line out of "data" starting at offset *off. Resource file
 * syntax (app-defaults, .Xresources) allows a backslash at end of line to
 * splice the next physical line; Motif's *XmLabel.fontList and compound-string
 * font specs lean on this.
 *
 * Returns a malloc'd joined line, or NULL on EOF / OOM. Advances *off past the
 * consumed bytes.
 */
static char *readJoinedLineFromString(const char *data, size_t *off)
{
    size_t start = *off;
    if (data[start] == '\0')
        return NULL;
    /* Buffer grows as physical lines accumulate. */
    size_t cap = 256;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    size_t outLen = 0;
    out[0] = '\0';
    while (data[start] != '\0') {
        const char *eol = strchr(data + start, '\n');
        size_t physLen =
            eol ? (size_t) (eol - (data + start)) : strlen(data + start);
        /* Strip trailing CR. */
        size_t trim = physLen;
        if (trim > 0 && data[start + trim - 1] == '\r')
            trim--;
        Bool continued = trim > 0 && data[start + trim - 1] == '\\';
        size_t copyLen = continued ? trim - 1 : trim;
        if (outLen + copyLen + 1 > cap) {
            while (outLen + copyLen + 1 > cap)
                cap *= 2;
            char *grow = realloc(out, cap);
            if (!grow) {
                free(out);
                return NULL;
            }
            out = grow;
        }
        memcpy(out + outLen, data + start, copyLen);
        outLen += copyLen;
        out[outLen] = '\0';
        start += physLen + (eol ? 1 : 0);
        if (!continued)
            break;
    }
    *off = start;
    return out;
}

XrmDatabase XrmGetStringDatabase(_Xconst char *data)
{
    if (!data)
        return NULL;
    XrmDatabase db = xrmNewDatabase();
    if (!db)
        return NULL;
    size_t off = 0;
    char *line;
    while ((line = readJoinedLineFromString(data, &off)) != NULL) {
        XrmPutLineResource(&db, line);
        free(line);
    }
    return db;
}

XrmDatabase XrmGetFileDatabase(_Xconst char *filename)
{
    if (!filename)
        return NULL;
    FILE *f = fopen(filename, "r");
    if (!f)
        return NULL;
    XrmDatabase db = xrmNewDatabase();
    if (!db) {
        fclose(f);
        return NULL;
    }
    /* Buffer holds a logical line built from one or more continued physical
     * lines.
     */
    size_t cap = 1024;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return db;
    }
    char chunk[1024];
    while (fgets(chunk, sizeof(chunk), f)) {
        size_t bufLen = 0;
        for (;;) {
            size_t chunkLen = strlen(chunk);
            chunkLen = rstripNewline(chunk, chunkLen);
            Bool continued = chunkLen > 0 && chunk[chunkLen - 1] == '\\';
            size_t copyLen = continued ? chunkLen - 1 : chunkLen;
            if (bufLen + copyLen + 1 > cap) {
                while (bufLen + copyLen + 1 > cap)
                    cap *= 2;
                char *grow = realloc(buf, cap);
                if (!grow) {
                    free(buf);
                    fclose(f);
                    return db;
                }
                buf = grow;
            }
            memcpy(buf + bufLen, chunk, copyLen);
            bufLen += copyLen;
            buf[bufLen] = '\0';
            if (!continued)
                break;
            if (!fgets(chunk, sizeof(chunk), f))
                break;
        }
        XrmPutLineResource(&db, buf);
    }
    free(buf);
    fclose(f);
    return db;
}

/* Reconstruct the source-text pattern for an entry (for file output). */
static char *entryPatternToString(const XrmEntry *e)
{
    size_t total = 1;
    for (int i = 0; i < e->complen; i++) {
        const char *seg = XrmQuarkToString(e->quarks[i]);
        size_t segLen = seg ? strlen(seg) : 0;
        /* Every position can have a binding marker, including i == 0 when
         * bindings[0] is loose.
         */
        size_t sepLen = (i == 0 && e->bindings[0] == XrmBindTightly) ? 0 : 1;
        total += sepLen + segLen;
    }
    char *out = malloc(total);
    if (!out)
        return NULL;
    size_t off = 0;
    for (int i = 0; i < e->complen; i++) {
        if (i == 0) {
            if (e->bindings[0] == XrmBindLoosely)
                out[off++] = '*';
        } else {
            out[off++] = (e->bindings[i] == XrmBindLoosely) ? '*' : '.';
        }
        const char *seg = XrmQuarkToString(e->quarks[i]);
        size_t segLen = seg ? strlen(seg) : 0;
        if (segLen > 0)
            memcpy(out + off, seg, segLen);
        off += segLen;
    }
    out[off] = '\0';
    return out;
}

/* Inverse of xrmDecodeValueEscapes: re-escape the bytes that would otherwise
 * corrupt the on-disk line format.
 *
 * Returns a malloc'd NUL- terminated string.
 */
static char *xrmEncodeValueEscapes(const char *src, size_t srcLen)
{
    /* Worst case: every byte becomes a 2-character escape. */
    char *out = malloc(srcLen * 2 + 1);
    if (!out)
        return NULL;
    size_t di = 0;
    for (size_t si = 0; si < srcLen; si++) {
        unsigned char c = (unsigned char) src[si];
        switch (c) {
        case '\n':
            out[di++] = '\\';
            out[di++] = 'n';
            break;
        case '\t':
            out[di++] = '\\';
            out[di++] = 't';
            break;
        case '\r':
            out[di++] = '\\';
            out[di++] = 'r';
            break;
        case '\\':
            out[di++] = '\\';
            out[di++] = '\\';
            break;
        default:
            out[di++] = (char) c;
        }
    }
    out[di] = '\0';
    return out;
}

void XrmPutFileDatabase(XrmDatabase db, _Xconst char *fileName)
{
    if (!db || !fileName)
        return;
    FILE *f = fopen(fileName, "w");
    if (!f)
        return;
    for (XrmEntry *e = db->head; e; e = e->next) {
        char *pat = entryPatternToString(e);
        if (!pat)
            continue;
        size_t valueLen = e->value ? strlen(e->value) : 0;
        char *encoded =
            xrmEncodeValueEscapes(e->value ? e->value : "", valueLen);
        if (!encoded) {
            free(pat);
            continue;
        }
        fprintf(f, "%s: %s\n", pat, encoded);
        free(encoded);
        free(pat);
    }
    fclose(f);
}

/* Per-query-position specificity code for one entry's match. Higher is more
 * specific, comparison is lexicographic from position 0 to the leaf (memcmp on
 * a uint8_t vector). The Xt resource precedence spec dictates this ordering at
 * every component:
 *
 *   7 = tight-binding name match     ('.' on the dot, name quark)
 *   6 = tight-binding class match    ('.' on the dot, class quark)
 *   5 = tight-binding wildcard match ('.' on the dot, '?' quark)
 *   4 = loose-binding name match     ('*' absorbed prefix, name quark)
 *   3 = loose-binding class match    ('*' absorbed prefix, class quark)
 *   2 = loose-binding wildcard match ('*' absorbed prefix, '?' quark)
 *   1 = elided                       (query position consumed by a '*'
 *                                     without a corresponding pattern
 *                                     component matching here)
 *   0 = no match                     (only appears in unfilled slots)
 *
 * Flat-sum scoring (the previous variant) violated left-to-right precedence
 * because deeper tight matches could outweigh a higher-level binding
 * difference. lexcompare against the per-level vector reproduces the spec rule
 * "first differing position decides" used by every real Xt/Motif build.
 */
typedef uint8_t XrmLevelScore;
enum {
    XRM_LVL_NONE = 0,
    XRM_LVL_ELIDED = 1,
    XRM_LVL_LOOSE_WILDCARD = 2,
    XRM_LVL_LOOSE_CLASS = 3,
    XRM_LVL_LOOSE_NAME = 4,
    XRM_LVL_TIGHT_WILDCARD = 5,
    XRM_LVL_TIGHT_CLASS = 6,
    XRM_LVL_TIGHT_NAME = 7,
};

#define XRM_QUERY_VEC_MAX (XRM_PREFIX_MAX + 2)

static XrmLevelScore matchCode(Bool tight, Bool nameMatch, Bool classMatch)
{
    if (tight) {
        if (!nameMatch && !classMatch)
            return XRM_LVL_TIGHT_WILDCARD;
        return nameMatch ? XRM_LVL_TIGHT_NAME : XRM_LVL_TIGHT_CLASS;
    }
    if (!nameMatch && !classMatch)
        return XRM_LVL_LOOSE_WILDCARD;
    return nameMatch ? XRM_LVL_LOOSE_NAME : XRM_LVL_LOOSE_CLASS;
}

/* Walk one entry's pattern against the query. The recursion explores all legal
 * loose-binding placements and keeps the lexicographically largest
 * complete-match vector in "best".
 *
 * Returns True iff at least one complete match was found.
 */
static Bool matchEntryWalk(const XrmEntry *e,
                           const XrmQuark *nameQ,
                           const XrmQuark *classQ,
                           int queryLen,
                           int pi,
                           int qi,
                           XrmLevelScore *cur,
                           XrmLevelScore *best,
                           Bool *haveBest)
{
    if (pi == e->complen) {
        if (qi != queryLen)
            return False;
        if (!*haveBest || memcmp(cur, best, (size_t) queryLen) > 0) {
            memcpy(best, cur, (size_t) queryLen);
            *haveBest = True;
        }
        return True;
    }
    int remaining = e->complen - pi;
    if (e->bindings[pi] == XrmBindLoosely) {
        /* Each remaining pattern component consumes at least one query
         * position, so the latest j that still leaves room is queryLen -
         * remaining.
         */
        int maxJ = queryLen - remaining;
        Bool any = False;
        for (int j = qi; j <= maxJ; j++) {
            Bool nameMatch = False;
            Bool classMatch = False;
            if (!patternQuarkMatches(e->quarks[pi], nameQ[j], classQ[j],
                                     &nameMatch, &classMatch))
                continue;
            for (int k = qi; k < j; k++)
                cur[k] = XRM_LVL_ELIDED;
            cur[j] = matchCode(False, nameMatch, classMatch);
            if (matchEntryWalk(e, nameQ, classQ, queryLen, pi + 1, j + 1, cur,
                               best, haveBest))
                any = True;
        }
        return any;
    }
    if (qi >= queryLen)
        return False;
    Bool nameMatch = False;
    Bool classMatch = False;
    if (!patternQuarkMatches(e->quarks[pi], nameQ[qi], classQ[qi], &nameMatch,
                             &classMatch))
        return False;
    cur[qi] = matchCode(True, nameMatch, classMatch);
    return matchEntryWalk(e, nameQ, classQ, queryLen, pi + 1, qi + 1, cur, best,
                          haveBest);
}

static Bool matchEntry(const XrmEntry *e,
                       const XrmQuark *nameQ,
                       const XrmQuark *classQ,
                       int queryLen,
                       XrmLevelScore *best)
{
    XrmLevelScore cur[XRM_QUERY_VEC_MAX] = {0};
    memset(best, 0, (size_t) queryLen);
    Bool haveBest = False;
    matchEntryWalk(e, nameQ, classQ, queryLen, 0, 0, cur, best, &haveBest);
    return haveBest;
}

/* Split a dotted name into a quark array (no bindings; query paths have
 * implicit tight binding throughout).
 *
 * Returns the count, or -1 on OOM. Empty components from leading or trailing
 * '.' are skipped.
 */
static int splitNameToQuarks(const char *s, XrmQuark *out, int max)
{
    int n = 0;
    if (!s)
        return 0;
    const char *segment = s;
    const char *cursor = s;
    while (*cursor != '\0' && n < max) {
        if (*cursor == '.' || *cursor == '*') {
            if (cursor != segment) {
                size_t len = (size_t) (cursor - segment);
                char *tmp = malloc(len + 1);
                if (!tmp)
                    return -1;
                memcpy(tmp, segment, len);
                tmp[len] = '\0';
                out[n++] = XrmStringToQuark(tmp);
                free(tmp);
            }
            segment = cursor + 1;
        }
        cursor++;
    }
    if (cursor != segment && n < max) {
        size_t len = (size_t) (cursor - segment);
        char *tmp = malloc(len + 1);
        if (!tmp)
            return -1;
        memcpy(tmp, segment, len);
        tmp[len] = '\0';
        out[n++] = XrmStringToQuark(tmp);
        free(tmp);
    }
    return n;
}

Bool XrmGetResource(XrmDatabase db,
                    _Xconst char *str_name,
                    _Xconst char *str_class,
                    char **str_type_return,
                    XrmValuePtr value_return)
{
    if (str_type_return)
        *str_type_return = NULL;
    if (value_return) {
        value_return->addr = NULL;
        value_return->size = 0;
    }
    if (!db || !str_name || !value_return)
        return False;

    enum { MAX_COMPS = XRM_PREFIX_MAX + 2 };
    XrmQuark nameQ[MAX_COMPS];
    XrmQuark classQ[MAX_COMPS];
    int nameLen = splitNameToQuarks(str_name, nameQ, MAX_COMPS);
    int classLen =
        str_class ? splitNameToQuarks(str_class, classQ, MAX_COMPS) : 0;
    if (nameLen < 0 || classLen < 0 || nameLen == 0)
        return False;
    int queryLen = nameLen > classLen ? nameLen : classLen;
    for (int i = nameLen; i < queryLen; i++)
        nameQ[i] = NULLQUARK;
    for (int i = classLen; i < queryLen; i++)
        classQ[i] = NULLQUARK;

    XrmQuark leafName = nameQ[queryLen - 1];
    XrmQuark leafClass =
        queryLen <= classLen ? classQ[queryLen - 1] : NULLQUARK;

    XrmEntry *best = NULL;
    XrmLevelScore bestVec[XRM_QUERY_VEC_MAX] = {0};

    /* Visit candidate entries via the leaf-quark buckets. A pattern can match
     * only if its leaf matches the query's name leaf or class leaf. When name
     * and class hash to the same bucket the search walks it once; otherwise it
     * walks both.
     */
    unsigned int bucketName = quarkBucket(leafName);
    unsigned int bucketClass =
        leafClass != NULLQUARK ? quarkBucket(leafClass) : bucketName;
    unsigned int bucketWildcard = quarkBucket(wildcardComponentQuark());
    unsigned int seen[3] = {bucketName, bucketClass, bucketWildcard};
    int seenCount = 0;
    for (int i = 0; i < 3; i++) {
        Bool duplicate = False;
        for (int j = 0; j < seenCount; j++) {
            if (seen[j] == seen[i]) {
                duplicate = True;
                break;
            }
        }
        if (!duplicate)
            seen[seenCount++] = seen[i];
    }
    XrmQuark wildcard = wildcardComponentQuark();
    for (int s = 0; s < seenCount; s++) {
        for (XrmEntry *e = db->buckets[seen[s]]; e; e = e->bnext) {
            if (e->leaf != leafName && e->leaf != leafClass &&
                e->leaf != wildcard)
                continue;
            XrmLevelScore vec[XRM_QUERY_VEC_MAX];
            if (!matchEntry(e, nameQ, classQ, queryLen, vec))
                continue;
            if (!best || memcmp(vec, bestVec, (size_t) queryLen) > 0) {
                memcpy(bestVec, vec, (size_t) queryLen);
                best = e;
            }
        }
    }
    if (!best)
        return False;
    if (str_type_return)
        *str_type_return = best->type;
    value_return->addr = (XPointer) best->value;
    value_return->size = best->value_size;
    return True;
}

XrmDatabase XrmGetDatabase(Display *display)
{
    if (!display)
        return NULL;
    return display->db;
}

void XrmSetDatabase(Display *display, XrmDatabase database)
{
    if (!display)
        return;
    /* The caller owns the old database per Xlib convention; this function does
     * not free what was there. The standard pattern is XrmDestroyDatabase(old)
     * before XrmSetDatabase.
     */
    display->db = database;
}

/* Move all entries from "from" into "into", respecting override. Both databases
 * own their entries; on completion "from" is destroyed and its entries are
 * either folded into "into" or freed.
 *
 * Replacement scans the per-leaf bucket in "into" (O(N/buckets)) instead of the
 * whole list, so merging two N-entry databases is O(N) average rather than the
 * O(N^2) the previous flat list produced.
 */
static void xrmCombineInto(XrmDatabase from, XrmDatabase into, Bool override)
{
    XrmEntry *e = from->head;
    while (e) {
        XrmEntry *next = e->next;
        XrmEntry *existing =
            xrmFindEntryByPattern(into, e->quarks, e->bindings, e->complen);
        if (existing) {
            if (override) {
                /* Steal e's value/type rather than copy + free. */
                free(existing->value);
                free(existing->type);
                existing->value = e->value;
                existing->type = e->type;
                existing->value_size = e->value_size;
                e->value = NULL;
                e->type = NULL;
            }
            xrmFreeEntry(e);
        } else {
            /* Detach e from from->head, which the loop is walking. */
            e->next = NULL;
            e->bnext = NULL;
            xrmLinkEntry(into, e);
        }
        e = next;
    }
    /* All entries are now either reused in "into" or freed; clear "from"'s
     * lists before destroying so XrmDestroyDatabase doesn't double-free.
     */
    memset(from->buckets, 0, sizeof(from->buckets));
    from->head = NULL;
    from->tail = NULL;
    XrmDestroyDatabase(from);
}

void XrmMergeDatabases(XrmDatabase from, XrmDatabase *into)
{
    if (!from || !into)
        return;
    if (!*into) {
        *into = from;
        return;
    }
    xrmCombineInto(from, *into, True);
}

void XrmCombineDatabase(XrmDatabase from, XrmDatabase *into, Bool override)
{
    if (!from || !into)
        return;
    if (!*into) {
        *into = from;
        return;
    }
    xrmCombineInto(from, *into, override);
}

Status XrmCombineFileDatabase(_Xconst char *filename,
                              XrmDatabase *target,
                              Bool override)
{
    XrmDatabase fileDb = XrmGetFileDatabase(filename);
    if (!fileDb)
        return False;
    XrmCombineDatabase(fileDb, target, override);
    return True;
}

void XrmParseCommand(XrmDatabase *pdb,
                     XrmOptionDescList options,
                     int num_options,
                     _Xconst char *prefix,
                     int *argc,
                     char **argv)
{
    if (!pdb || !options || !argc || !argv)
        return;
    if (!*pdb)
        *pdb = xrmNewDatabase();
    if (!*pdb)
        return;
    if (!prefix)
        prefix = "";

    int kept = 1; /* argv[0] always survives. */
    int original_argc = *argc;
    for (int i = 1; i < *argc;) {
        char *arg = argv[i];
        int matched = 0;
        for (int o = 0; o < num_options; o++) {
            size_t optionLen = strlen(options[o].option);
            Bool stickyMatch =
                options[o].argKind == XrmoptionStickyArg &&
                strncmp(arg, options[o].option, optionLen) == 0 &&
                arg[optionLen] != '\0';
            if (!stickyMatch && strcmp(arg, options[o].option) != 0)
                continue;
            char specifier[256];
            const char *res = options[o].specifier;
            if (res && res[0] == '.')
                snprintf(specifier, sizeof(specifier), "%s%s", prefix, res);
            else
                snprintf(specifier, sizeof(specifier), "%s.%s", prefix,
                         res ? res : "");
            const char *value = NULL;
            int consumed = 1;
            switch (options[o].argKind) {
            case XrmoptionNoArg:
                value = options[o].value;
                break;
            case XrmoptionIsArg:
                value = arg;
                break;
            case XrmoptionStickyArg:
                value = arg + optionLen;
                break;
            case XrmoptionSepArg:
                if (i + 1 < *argc) {
                    value = argv[i + 1];
                    consumed = 2;
                }
                break;
            case XrmoptionResArg:
                if (i + 1 < *argc) {
                    XrmPutLineResource(pdb, argv[i + 1]);
                    consumed = 2;
                    matched = 1;
                }
                break;
            case XrmoptionSkipArg:
                consumed = 2;
                break;
            case XrmoptionSkipLine:
                consumed = *argc - i;
                break;
            case XrmoptionSkipNArgs:
                consumed = 1 + (int) (intptr_t) options[o].value;
                break;
            default:
                break;
            }
            if (value) {
                XrmPutStringResource(pdb, specifier, value);
                matched = 1;
            } else if (options[o].argKind == XrmoptionNoArg) {
                matched = 1;
            }
            i += consumed;
            if (matched)
                break;
        }
        if (!matched) {
            argv[kept++] = arg;
            i++;
        }
    }
    *argc = kept;
    /* Mirror upstream libX11 ParseCmd.c: only NULL-terminate when compression
     * actually freed a slot. libXt's _XtAppInit passes a heap-allocated argv
     * sized exactly to *argc, so writing argv[*argc] unconditionally smashes
     * the byte past the allocation (caught by AddressSanitizer in CI).
     */
    if (kept < original_argc)
        argv[kept] = NULL;
}

const char *XrmLocaleOfDatabase(XrmDatabase db)
{
    (void) db;
    return "C";
}

/* The bucket-based quark API (XrmQGetSearchList, XrmQGetSearchResource,
 * XrmQGetResource) is libXt's primary path into the resource database: every
 * widget Set/GetValues, every _XtDisplayInitialize boot probe, and Motif's
 * giant resource cascade all funnel through it. XrmQGetSearchList holds the
 * full prefix and the database pointer inside the caller's slots, then
 * XrmQGetSearchResource reassembles the (prefix + leaf) quark path and
 * dispatches through XrmQGetResource.
 *
 * Search-list layout:
 *
 *     [0]                  -> (XrmHashTable) db
 *     [1]                  -> n      (number of prefix components)
 *     [2 .. 2 + n - 1]     -> name quarks
 *     [2 + n .. 2 + 2n -1] -> class quarks
 *     [2 + 2n]             -> NULL terminator
 *
 * The caller-supplied list_length must hold 3 + 2 * n slots or the call returns
 * False so libXt's doubling loop in _XtDisplayInitialize widens the buffer and
 * retries.
 */

Bool XrmQGetResource(XrmDatabase db,
                     XrmNameList quark_name,
                     XrmClassList quark_class,
                     XrmRepresentation *quark_type_return,
                     XrmValuePtr value_return)
{
    if (quark_type_return)
        *quark_type_return = 0;
    if (value_return) {
        value_return->addr = NULL;
        value_return->size = 0;
    }
    if (!db || !quark_name)
        return False;

    /* Compute name/class lengths from the NULLQUARK terminators. */
    int nameLen = 0;
    while (quark_name && quark_name[nameLen] != NULLQUARK)
        nameLen++;
    int classLen = 0;
    if (quark_class)
        while (quark_class[classLen] != NULLQUARK)
            classLen++;
    if (nameLen == 0)
        return False;

    int queryLen = nameLen > classLen ? nameLen : classLen;
    if (queryLen > XRM_PREFIX_MAX + 1)
        return False;

    XrmQuark nameQ[XRM_PREFIX_MAX + 2];
    XrmQuark classQ[XRM_PREFIX_MAX + 2];
    for (int i = 0; i < nameLen; i++)
        nameQ[i] = quark_name[i];
    for (int i = nameLen; i < queryLen; i++)
        nameQ[i] = NULLQUARK;
    for (int i = 0; i < classLen; i++)
        classQ[i] = quark_class[i];
    for (int i = classLen; i < queryLen; i++)
        classQ[i] = NULLQUARK;

    XrmQuark leafName = nameQ[queryLen - 1];
    XrmQuark leafClass =
        classLen >= queryLen ? classQ[queryLen - 1] : NULLQUARK;
    unsigned int bucketName = quarkBucket(leafName);
    unsigned int bucketClass =
        leafClass != NULLQUARK ? quarkBucket(leafClass) : bucketName;
    unsigned int bucketWildcard = quarkBucket(wildcardComponentQuark());
    unsigned int seen[3] = {bucketName, bucketClass, bucketWildcard};
    int seenCount = 0;
    for (int i = 0; i < 3; i++) {
        Bool duplicate = False;
        for (int j = 0; j < seenCount; j++) {
            if (seen[j] == seen[i]) {
                duplicate = True;
                break;
            }
        }
        if (!duplicate)
            seen[seenCount++] = seen[i];
    }
    XrmQuark wildcard = wildcardComponentQuark();

    XrmEntry *best = NULL;
    XrmLevelScore bestVec[XRM_QUERY_VEC_MAX] = {0};
    for (int s = 0; s < seenCount; s++) {
        for (XrmEntry *e = db->buckets[seen[s]]; e; e = e->bnext) {
            if (e->leaf != leafName && e->leaf != leafClass &&
                e->leaf != wildcard)
                continue;
            XrmLevelScore vec[XRM_QUERY_VEC_MAX];
            if (!matchEntry(e, nameQ, classQ, queryLen, vec))
                continue;
            if (!best || memcmp(vec, bestVec, (size_t) queryLen) > 0) {
                memcpy(bestVec, vec, (size_t) queryLen);
                best = e;
            }
        }
    }
    if (!best)
        return False;
    if (quark_type_return)
        *quark_type_return = best->type ? XrmStringToQuark(best->type) : 0;
    value_return->addr = (XPointer) best->value;
    value_return->size = best->value_size;
    return True;
}

static int countQuarkList(const XrmQuark *q)
{
    int n = 0;
    if (q)
        while (q[n] != NULLQUARK)
            n++;
    return n;
}

Bool XrmQGetSearchList(XrmDatabase db,
                       XrmNameList names,
                       XrmClassList classes,
                       XrmSearchList list_return,
                       int list_length)
{
    /* libXt callers interpret False as "buffer too small" and retry with larger
     * storage. Pack the database pointer plus the prefix arrays into the
     * caller's slots so XrmQGetSearchResource can reconstruct the full path;
     * the layout is documented above. For unsupported over-deep prefixes,
     * return a valid empty list so callers stop retrying and the follow-up
     * resource lookup simply fails.
     */
    if (!list_return || list_length <= 0)
        return False;
    int n_names = countQuarkList(names);
    int n_classes = countQuarkList(classes);
    int n = n_names > n_classes ? n_names : n_classes;
    if (n > XRM_PREFIX_MAX) {
        list_return[0] = NULL;
        return True;
    }
    int needed = 3 + 2 * n;
    if (list_length < needed)
        return False;
    list_return[0] = (XrmHashTable) db;
    list_return[1] = (XrmHashTable) (uintptr_t) n;
    for (int i = 0; i < n; i++) {
        XrmQuark nq = (i < n_names) ? names[i] : NULLQUARK;
        XrmQuark cq = (i < n_classes) ? classes[i] : NULLQUARK;
        list_return[2 + i] = (XrmHashTable) (uintptr_t) nq;
        list_return[2 + n + i] = (XrmHashTable) (uintptr_t) cq;
    }
    list_return[2 + 2 * n] = NULL;
    return True;
}

Bool XrmQGetSearchResource(XrmSearchList searchList,
                           register XrmName name,
                           register XrmClass class,
                           XrmRepresentation *pType,
                           XrmValue *pValue)
{
    if (pType)
        *pType = 0;
    if (pValue) {
        pValue->addr = NULL;
        pValue->size = 0;
    }
    if (!searchList || !searchList[0])
        return False;
    XrmDatabase db = (XrmDatabase) searchList[0];
    int n = (int) (uintptr_t) searchList[1];
    if (n < 0 || n > XRM_PREFIX_MAX)
        return False;
    if (name == NULLQUARK)
        return False;

    /* Rebuild the full path: <prefix names...> + leaf name + NULLQUARK, matched
     * by <prefix classes...> + leaf class + NULLQUARK. +2 for the leaf slot
     * plus the terminator.
     */
    XrmQuark full_names[XRM_PREFIX_MAX + 2];
    XrmQuark full_classes[XRM_PREFIX_MAX + 2];
    for (int i = 0; i < n; i++) {
        full_names[i] = (XrmQuark) (uintptr_t) searchList[2 + i];
        full_classes[i] = (XrmQuark) (uintptr_t) searchList[2 + n + i];
    }
    full_names[n] = name;
    full_classes[n] = class;
    full_names[n + 1] = NULLQUARK;
    full_classes[n + 1] = NULLQUARK;

    return XrmQGetResource(db, full_names, full_classes, pType, pValue);
}

void XrmQPutResource(XrmDatabase *pdb,
                     XrmBindingList bindings,
                     XrmQuarkList quarks,
                     XrmRepresentation type,
                     XrmValue *value)
{
    if (!pdb || !quarks || !value)
        return;
    if (!*pdb)
        *pdb = xrmNewDatabase();
    if (!*pdb)
        return;
    int n = 0;
    while (quarks[n] != NULLQUARK)
        n++;
    if (n == 0)
        return;

    XrmQuark *qcopy = malloc(sizeof(XrmQuark) * n);
    XrmBinding *bcopy = malloc(sizeof(XrmBinding) * n);
    if (!qcopy || !bcopy) {
        free(qcopy);
        free(bcopy);
        return;
    }
    for (int i = 0; i < n; i++) {
        qcopy[i] = quarks[i];
        bcopy[i] = bindings ? bindings[i] : XrmBindTightly;
    }

    XrmEntry *existing = xrmFindEntryByPattern(*pdb, qcopy, bcopy, n);
    unsigned int size = value->size;
    if (size == 0 && value->addr)
        size = (unsigned int) strlen((const char *) value->addr) + 1;
    const char *typeName = XrmQuarkToString(type);
    if (existing) {
        if (value->addr && size > 0)
            xrmReplaceEntryValue(existing, typeName ? typeName : "String",
                                 (const char *) value->addr, size);
        free(qcopy);
        free(bcopy);
        return;
    }
    if (!value->addr || size == 0) {
        free(qcopy);
        free(bcopy);
        return;
    }
    XrmEntry *e = calloc(1, sizeof(*e));
    if (!e) {
        free(qcopy);
        free(bcopy);
        return;
    }
    e->quarks = qcopy;
    e->bindings = bcopy;
    e->complen = n;
    e->leaf = qcopy[n - 1];
    e->type = strdup(typeName ? typeName : "String");
    e->value = malloc(size);
    if (!e->type || !e->value) {
        free(e->type);
        free(e->value);
        free(e->quarks);
        free(e->bindings);
        free(e);
        return;
    }
    memcpy(e->value, value->addr, size);
    e->value_size = size;
    xrmLinkEntry(*pdb, e);
}

void XrmQPutStringResource(XrmDatabase *pdb,
                           XrmBindingList bindings,
                           XrmQuarkList quarks,
                           _Xconst char *value)
{
    XrmValue xrmValue;
    xrmValue.addr = (XPointer) (value ? value : "");
    xrmValue.size = (unsigned int) strlen((const char *) xrmValue.addr) + 1;
    XrmQPutResource(pdb, bindings, quarks, XrmStringToQuark("String"),
                    &xrmValue);
}

/* Test whether "e" could match SOME completion of the given prefix.
 *
 * An entry's components 0..K-1 must consume the prefix (with loose bindings
 * allowed to elide query positions or absorb them entirely), and the remaining
 * components K..complen-1 form the "completion." For XrmEnumAllLevels the
 * completion may be any length >= 1; for XrmEnumOneLevel exactly 1.
 *
 * The previous implementation compared e->quarks[i] == prefix[i] 1:1 across the
 * prefix and rejected any loose-binding entry that didn't happen to line up, so
 * resources like "*background" stored with a leading wildcard were never
 * returned. Xt/Motif resource walkers depend on enumeration honoring loose
 * bindings exactly the way lookup does.
 */
static Bool enumPrefixMatches(const XrmEntry *e,
                              const XrmQuark *nameQ,
                              const XrmQuark *classQ,
                              int nameLen,
                              int classLen,
                              int prefixLen,
                              int mode,
                              int pi,
                              int qi)
{
    if (qi == prefixLen) {
        int remaining = e->complen - pi;
        if (mode == XrmEnumAllLevels)
            return remaining >= 1;
        if (mode == XrmEnumOneLevel)
            return remaining == 1;
        return False;
    }
    if (pi == e->complen)
        return False;
    if (e->bindings[pi] == XrmBindLoosely) {
        /* Try matching this loose component at any prefix position j; positions
         * qi..j-1 are elided.
         */
        for (int j = qi; j < prefixLen; j++) {
            Bool nameMatch = False;
            Bool classMatch = False;
            XrmQuark nq = j < nameLen ? nameQ[j] : NULLQUARK;
            XrmQuark cq = j < classLen ? classQ[j] : NULLQUARK;
            if (!patternQuarkMatches(e->quarks[pi], nq, cq, &nameMatch,
                                     &classMatch))
                continue;
            if (enumPrefixMatches(e, nameQ, classQ, nameLen, classLen,
                                  prefixLen, mode, pi + 1, j + 1))
                return True;
        }
        /* Alternative: the loose binding's matched component lands in the
         * completion (j >= prefixLen), absorbing all remaining prefix positions
         * as elisions.
         */
        int remainingCompletion = e->complen - pi;
        if (mode == XrmEnumAllLevels)
            return remainingCompletion >= 1;
        if (mode == XrmEnumOneLevel)
            return remainingCompletion == 1;
        return False;
    }
    /* Tight: must match at qi exactly. */
    Bool nameMatch = False;
    Bool classMatch = False;
    XrmQuark nq = qi < nameLen ? nameQ[qi] : NULLQUARK;
    XrmQuark cq = qi < classLen ? classQ[qi] : NULLQUARK;
    if (!patternQuarkMatches(e->quarks[pi], nq, cq, &nameMatch, &classMatch))
        return False;
    return enumPrefixMatches(e, nameQ, classQ, nameLen, classLen, prefixLen,
                             mode, pi + 1, qi + 1);
}

Bool XrmEnumerateDatabase(XrmDatabase db,
                          XrmNameList names,
                          XrmClassList classes,
                          int mode,
                          Bool (*proc)(XrmDatabase *,
                                       XrmBindingList,
                                       XrmQuarkList,
                                       XrmRepresentation *,
                                       XrmValue *,
                                       XPointer),
                          XPointer arg)
{
    if (!db || !proc)
        return False;
    int nameLen = countQuarkList(names);
    int classLen = countQuarkList(classes);
    int prefixLen = nameLen > classLen ? nameLen : classLen;

    XrmEntry *e = db->head;
    while (e) {
        XrmEntry *next = e->next;
        Bool matches = enumPrefixMatches(e, names, classes, nameLen, classLen,
                                         prefixLen, mode, 0, 0);
        if (matches) {
            XrmRepresentation typeQuark =
                e->type ? XrmStringToQuark(e->type) : 0;
            XrmValue v;
            v.addr = (XPointer) e->value;
            v.size = e->value_size;
            /* The caller's proc receives terminator-NULLQUARK arrays;
             * stack-allocate the temporary buffers since complen is bounded by
             * XRM_PREFIX_MAX in practice.
             */
            XrmQuark qbuf[XRM_PREFIX_MAX + 2];
            XrmBinding bbuf[XRM_PREFIX_MAX + 2];
            int cap = e->complen;
            if (cap > XRM_PREFIX_MAX + 1)
                cap = XRM_PREFIX_MAX + 1;
            for (int i = 0; i < cap; i++) {
                qbuf[i] = e->quarks[i];
                bbuf[i] = e->bindings[i];
            }
            qbuf[cap] = NULLQUARK;
            if (proc(&db, bbuf, qbuf, &typeQuark, &v, arg))
                return True;
        }
        e = next;
    }
    return False;
}
