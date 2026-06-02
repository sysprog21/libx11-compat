#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xresource.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "util.h"

/* Minimal X resource manager. Stores entries as a linked list of
 * (pattern, type, value) triples. Patterns use the syntax from
 * XrmGetResource / XrmGetStringDatabase: components separated by '.' for
 * tight binding and '*' for loose binding. Wildcards '?' / '*' inside a
 * single component are not supported. */

typedef struct XrmEntry {
    char *pattern;
    char *type;
    char *value;
    unsigned int value_size; /* includes terminating NUL */
    struct XrmEntry *next;
} XrmEntry;

struct _XrmHashBucketRec {
    XrmEntry *head;
};

static XrmEntry *xrmAllocEntry(const char *pattern,
                               const char *type,
                               const char *value,
                               unsigned int value_size)
{
    XrmEntry *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->pattern = strdup(pattern);
    e->type = strdup(type ? type : "String");
    e->value = malloc(value_size);
    if (!e->pattern || !e->type || !e->value) {
        free(e->pattern);
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
    free(e->pattern);
    free(e->type);
    free(e->value);
    free(e);
}

static XrmEntry *xrmFindExact(XrmDatabase db, const char *pattern)
{
    if (!db)
        return NULL;
    for (XrmEntry *e = db->head; e; e = e->next) {
        if (strcmp(e->pattern, pattern) == 0)
            return e;
    }
    return NULL;
}

static void xrmReplaceOrInsert(XrmDatabase db,
                               const char *pattern,
                               const char *type,
                               const char *value,
                               unsigned int value_size)
{
    XrmEntry *existing = xrmFindExact(db, pattern);
    if (existing) {
        char *newValue = malloc(value_size);
        char *newType = strdup(type ? type : "String");
        if (!newValue || !newType) {
            free(newValue);
            free(newType);
            return;
        }
        memcpy(newValue, value, value_size);
        free(existing->value);
        free(existing->type);
        existing->value = newValue;
        existing->type = newType;
        existing->value_size = value_size;
        return;
    }
    XrmEntry *e = xrmAllocEntry(pattern, type, value, value_size);
    if (!e)
        return;
    e->next = db->head;
    db->head = e;
}

static XrmDatabase xrmNewDatabase(void)
{
    XrmDatabase db = calloc(1, sizeof(*db));
    return db;
}

void XrmInitialize(void)
{
    /* Nothing to set up: the quark table lives in libX11's Quarks.c
     * (staged under $(OUT)/upstream/src/) and initializes on first use. */
}

/* The XrmStringToQuarkList / XrmStringToBindingQuarkList implementations
 * below derive from libX11's src/Xrm.c (which we do not compile because
 * its surrounding database code is replaced by the simpler routines in
 * this file). The original upstream code is:
 *
 *     Copyright 1987, 1988, 1990, 1991, 1998 The Open Group
 *
 * See doc/COPYING in the libX11 source tree for the full notice; the
 * MIT-style terms there are compatible with this project's own MIT
 * license.
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

/* Split a resource line "name: value" (or "name*foo: value") into
 * pattern and value, ignoring leading whitespace and one ':'. */
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
    memcpy(val, valStart, valLen);
    val[valLen] = '\0';
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
    xrmReplaceOrInsert(*pdb, pattern, "String", value,
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
    xrmReplaceOrInsert(*pdb, specifier, "String", value,
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
    xrmReplaceOrInsert(*pdb, specifier, type, (const char *) value->addr,
                       value->size);
}

XrmDatabase XrmGetStringDatabase(_Xconst char *data)
{
    if (!data)
        return NULL;
    XrmDatabase db = xrmNewDatabase();
    if (!db)
        return NULL;
    const char *p = data;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t) (eol - p) : strlen(p);
        char *line = malloc(len + 1);
        if (!line)
            break;
        memcpy(line, p, len);
        line[len] = '\0';
        XrmPutLineResource(&db, line);
        free(line);
        if (!eol)
            break;
        p = eol + 1;
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
    char line[2048];
    XrmDatabase db = xrmNewDatabase();
    if (!db) {
        fclose(f);
        return NULL;
    }
    while (fgets(line, sizeof(line), f)) {
        XrmPutLineResource(&db, line);
    }
    fclose(f);
    return db;
}

void XrmPutFileDatabase(XrmDatabase db, _Xconst char *fileName)
{
    if (!db || !fileName)
        return;
    FILE *f = fopen(fileName, "w");
    if (!f)
        return;
    for (XrmEntry *e = db->head; e; e = e->next) {
        fprintf(f, "%s: %s\n", e->pattern, e->value);
    }
    fclose(f);
}

/* Match a stored pattern against (str_name, str_class). Components in the
 * pattern are split by '.' (tight) or '*' (loose). The query joins name and
 * class component-by-component (with '.'); each pattern component can
 * match the corresponding name OR class component. A '*' in the pattern
 * skips zero or more components in the query. */
static int splitInto(const char *src, char **comps, char *bindings, int max)
{
    int n = 0;
    const char *p = src;
    while (*p != '\0' && n < max) {
        char binding = '.';
        while (*p == '.' || *p == '*') {
            if (*p == '*')
                binding = '*';
            p++;
        }
        if (*p == '\0')
            break;
        const char *start = p;
        while (*p != '\0' && *p != '.' && *p != '*')
            p++;
        size_t len = (size_t) (p - start);
        comps[n] = malloc(len + 1);
        if (!comps[n])
            return -1;
        memcpy(comps[n], start, len);
        comps[n][len] = '\0';
        bindings[n] = binding;
        n++;
    }
    return n;
}

static void freeComps(char **comps, int n)
{
    for (int i = 0; i < n; i++)
        free(comps[i]);
}

/* Match and score a pattern against a query. Returns 0 if no match;
 * otherwise a positive score where higher means more specific.
 * Scoring (highest weight first):
 *   - Tight (.) bindings beat loose (*) bindings.
 *   - Name-component matches beat class-component matches.
 *   - Longer patterns (more matched components) beat shorter ones.
 * '*' bindings prefer the leftmost possible match, then continue. */
static int matchAndScore(char **patComps,
                         char *patBindings,
                         int patLen,
                         char **nameComps,
                         char **classComps,
                         int queryLen,
                         int pi,
                         int qi,
                         int scoreSoFar)
{
    while (pi < patLen) {
        if (patBindings[pi] == '*') {
            int bestSub = 0;
            for (int j = qi; j < queryLen; j++) {
                int compScore = 0;
                /* Score each candidate match: name match = 4, class
                 * match = 2, and add 1 for tight binding (which this
                 * is not, so just the base). Skipped query components
                 * cost nothing. */
                int nameMatch =
                    nameComps[j] && strcmp(patComps[pi], nameComps[j]) == 0;
                int classMatch =
                    classComps[j] && strcmp(patComps[pi], classComps[j]) == 0;
                int wildMatch = strcmp(patComps[pi], "?") == 0;
                if (!nameMatch && !classMatch && !wildMatch)
                    continue;
                if (nameMatch)
                    compScore = 4;
                else if (classMatch)
                    compScore = 2;
                else
                    compScore = 1;
                int sub = matchAndScore(patComps, patBindings, patLen,
                                        nameComps, classComps, queryLen, pi + 1,
                                        j + 1, scoreSoFar + compScore);
                if (sub > bestSub)
                    bestSub = sub;
            }
            return bestSub;
        }
        if (qi >= queryLen)
            return 0;
        int nameMatch =
            nameComps[qi] && strcmp(patComps[pi], nameComps[qi]) == 0;
        int classMatch =
            classComps[qi] && strcmp(patComps[pi], classComps[qi]) == 0;
        int wildMatch = strcmp(patComps[pi], "?") == 0;
        if (!nameMatch && !classMatch && !wildMatch)
            return 0;
        /* Tight binding is preferred: +8. Name beats class: +4 vs +2.
         * Wildcard is +1. */
        scoreSoFar += 8;
        if (nameMatch)
            scoreSoFar += 4;
        else if (classMatch)
            scoreSoFar += 2;
        else
            scoreSoFar += 1;
        pi++;
        qi++;
    }
    if (qi != queryLen)
        return 0;
    /* +1 ensures any complete match returns a positive score even when
     * scoreSoFar started at zero (e.g. a single-component loose match
     * with class only). */
    return scoreSoFar + 1;
}

Bool XrmGetResource(XrmDatabase db,
                    _Xconst char *str_name,
                    _Xconst char *str_class,
                    char **str_type_return,
                    XrmValuePtr value_return)
{
    if (!db || !str_name || !value_return)
        return False;
    if (!str_class)
        str_class = "";

    enum { MAX_COMPS = 16 };
    char *nameComps[MAX_COMPS] = {0};
    char nameBindings[MAX_COMPS] = {0};
    char *classComps[MAX_COMPS] = {0};
    char classBindings[MAX_COMPS] = {0};
    int nameLen = splitInto(str_name, nameComps, nameBindings, MAX_COMPS);
    int classLen = splitInto(str_class, classComps, classBindings, MAX_COMPS);
    if (nameLen < 0 || classLen < 0) {
        freeComps(nameComps, MAX_COMPS);
        freeComps(classComps, MAX_COMPS);
        return False;
    }
    /* Pad the shorter list with NULLs so we can pair them at the same
     * index without bounds errors. */
    int queryLen = nameLen > classLen ? nameLen : classLen;
    for (int i = nameLen; i < queryLen; i++)
        nameComps[i] = NULL;
    for (int i = classLen; i < queryLen; i++)
        classComps[i] = NULL;

    XrmEntry *best = NULL;
    int bestScore = 0;
    for (XrmEntry *e = db->head; e; e = e->next) {
        char *patComps[MAX_COMPS] = {0};
        char patBindings[MAX_COMPS] = {0};
        int patLen = splitInto(e->pattern, patComps, patBindings, MAX_COMPS);
        if (patLen > 0) {
            int score = matchAndScore(patComps, patBindings, patLen, nameComps,
                                      classComps, queryLen, 0, 0, 0);
            if (score > bestScore) {
                bestScore = score;
                best = e;
            }
        }
        freeComps(patComps, MAX_COMPS);
    }
    freeComps(nameComps, MAX_COMPS);
    freeComps(classComps, MAX_COMPS);
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
    /* Caller owns the old database per Xlib convention: we do not free
     * what was there. The standard pattern is XrmDestroyDatabase(old)
     * before XrmSetDatabase. */
    display->db = database;
}

void XrmMergeDatabases(XrmDatabase from, XrmDatabase *into)
{
    if (!from || !into)
        return;
    if (!*into) {
        *into = from;
        return;
    }
    for (XrmEntry *e = from->head; e; e = e->next) {
        xrmReplaceOrInsert(*into, e->pattern, e->type, e->value, e->value_size);
    }
    XrmDestroyDatabase(from);
}

void XrmCombineDatabase(XrmDatabase from, XrmDatabase *into, Bool override)
{
    if (!from || !into)
        return;
    if (!*into) {
        *into = from;
        return;
    }
    for (XrmEntry *e = from->head; e; e = e->next) {
        if (!override && xrmFindExact(*into, e->pattern))
            continue;
        xrmReplaceOrInsert(*into, e->pattern, e->type, e->value, e->value_size);
    }
    XrmDestroyDatabase(from);
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
    /* Mirror upstream libX11 ParseCmd.c: only NULL-terminate when
     * compression actually freed a slot. libXt's _XtAppInit passes a
     * heap-allocated argv sized exactly to *argc, so writing argv[*argc]
     * unconditionally smashes the byte past the allocation (caught by
     * AddressSanitizer in CI). */
    if (kept < original_argc)
        argv[kept] = NULL;
}

const char *XrmLocaleOfDatabase(XrmDatabase db)
{
    (void) db;
    return "C";
}

/* The bucket-based quark API (XrmQGetSearchList, XrmQGetSearchResource,
 * XrmQGetResource) is libXt's primary path into the resource database --
 * every widget Set/GetValues, every _XtDisplayInitialize boot probe, and
 * Motif's giant resource cascade all funnel through it. The local
 * database is a flat linked list of (pattern, type, value) strings rather
 * than the bucketed quark tree libX11 ships, so we bridge by encoding
 * the database pointer *and* the widget prefix arrays into the search
 * list slots, then reassembling the full name/class path inside
 * XrmQGetSearchResource. Skipping the prefix and looking up the leaf
 * alone misses every hierarchical Motif resource and can false-hit
 * unrelated same-leaf entries, so the encoding is load-bearing.
 *
 * Search-list layout:
 *
 *     [0]                  -> (XrmHashTable) db
 *     [1]                  -> n      (number of prefix components)
 *     [2 .. 2 + n - 1]     -> name quarks
 *     [2 + n .. 2 + 2n -1] -> class quarks
 *     [2 + 2n]             -> NULL terminator
 *
 * The caller-supplied list_length must hold 3 + 2 * n slots or we return
 * False so libXt's doubling loop in _XtDisplayInitialize widens the
 * buffer and retries. */

/* Xlib documents resource paths up to 100 components; round to 128 so we
 * accept the entire spec range without forcing libXt's _XtDisplayInitialize
 * doubling loop to give up on a legitimate deep widget tree. */
#define XRM_PREFIX_MAX 128

static char *quarkToCString(XrmQuark q)
{
    /* XrmQuarkToString returns NULL for NULLQUARK. Callers prefer an
     * empty string for "wildcard / unspecified" since XrmGetResource
     * handles a "" class as "any class". The String typedef is libXt's,
     * not libX11's, so spell out char * here. */
    char *s = XrmQuarkToString(q);
    return s ? s : (char *) "";
}

static char *quarkListToCString(const XrmQuark *quarks)
{
    if (!quarks) {
        char *empty = malloc(1);
        if (empty)
            empty[0] = '\0';
        return empty;
    }

    /* Two-pass: first measure the joined length (with overflow guards on
     * each addition so a pathological quark table cannot wrap size_t),
     * then allocate and fill. */
    size_t total = 1;
    for (int i = 0; quarks[i] != NULLQUARK; i++) {
        const char *segment = quarkToCString(quarks[i]);
        size_t len = strlen(segment);
        size_t sep = (i > 0) ? 1 : 0;
        if (sep > SIZE_MAX - total || len > SIZE_MAX - total - sep)
            return NULL;
        total += sep + len;
    }

    char *path = malloc(total);
    if (!path)
        return NULL;

    size_t off = 0;
    for (int i = 0; quarks[i] != NULLQUARK; i++) {
        const char *segment = quarkToCString(quarks[i]);
        size_t len = strlen(segment);
        if (i > 0)
            path[off++] = '.';
        memcpy(path + off, segment, len);
        off += len;
    }
    path[off] = '\0';
    return path;
}

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

    /* Concatenate the quark lists back into the dotted strings the
     * pattern matcher in XrmGetResource expects. Resource paths and Xt
     * widget names are not byte-limited, so size these buffers from the
     * quark strings rather than imposing a fixed stack cap. */
    char *name_buf = quarkListToCString(quark_name);
    char *class_buf = quarkListToCString(quark_class);
    if (!name_buf || !class_buf) {
        free(name_buf);
        free(class_buf);
        return False;
    }

    char *type_str = NULL;
    Bool found =
        XrmGetResource(db, name_buf, class_buf, &type_str, value_return);
    free(name_buf);
    free(class_buf);
    if (!found)
        return False;
    if (quark_type_return)
        *quark_type_return = type_str ? XrmStringToQuark(type_str) : 0;
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
    /* libXt callers interpret False as "buffer too small" and retry with
     * larger storage. Pack the database pointer plus the prefix arrays
     * into the caller's slots so XrmQGetSearchResource can reconstruct
     * the full path; the layout is documented above. For unsupported
     * over-deep prefixes, return a valid empty list so callers stop
     * retrying and the follow-up resource lookup simply fails. */
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

    /* Rebuild the full path: <prefix names...> + leaf name + NULLQUARK,
     * matched by <prefix classes...> + leaf class + NULLQUARK. +2 for
     * the leaf slot plus the terminator. */
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
    (void) pdb;
    (void) bindings;
    (void) quarks;
    (void) type;
    (void) value;
}

void XrmQPutStringResource(XrmDatabase *pdb,
                           XrmBindingList bindings,
                           XrmQuarkList quarks,
                           _Xconst char *value)
{
    (void) pdb;
    (void) bindings;
    (void) quarks;
    (void) value;
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
    (void) db;
    (void) names;
    (void) classes;
    (void) mode;
    (void) proc;
    (void) arg;
    return False;
}
