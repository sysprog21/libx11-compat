#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Intrinsic.h>
#include <X11/Xresource.h>

#define FAIL(...)                     \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr);          \
        exit(1);                      \
    } while (0)

#define CHECK(cond, ...)       \
    do {                       \
        if (!(cond))           \
            FAIL(__VA_ARGS__); \
    } while (0)

typedef struct {
    int count;
    int saw_tight;
    int saw_loose;
} EnumState;

static Bool enum_proc(XrmDatabase *db,
                      XrmBindingList bindings,
                      XrmQuarkList quarks,
                      XrmRepresentation *type,
                      XrmValue *value,
                      XPointer closure)
{
    (void) db;
    (void) type;
    EnumState *state = (EnumState *) closure;
    state->count++;
    if (quarks[0] == XrmStringToQuark("top") &&
        quarks[1] == XrmStringToQuark("frame") &&
        quarks[2] == XrmStringToQuark("row") &&
        quarks[3] == XrmStringToQuark("col") &&
        quarks[4] == XrmStringToQuark("button") &&
        quarks[5] == XrmStringToQuark("label") &&
        quarks[6] == XrmStringToQuark("fontList") &&
        bindings[6] == XrmBindTightly && value->addr &&
        !strcmp((char *) value->addr, "tight-font")) {
        state->saw_tight = 1;
    }
    if (quarks[0] == XrmStringToQuark("XmLabel") &&
        quarks[1] == XrmStringToQuark("fontList") &&
        bindings[0] == XrmBindLoosely && bindings[1] == XrmBindTightly &&
        value->addr && !strcmp((char *) value->addr, "loose-font")) {
        state->saw_loose = 1;
    }
    return False;
}

static void check_get(XrmDatabase db,
                      const char *name,
                      const char *class_name,
                      const char *expected,
                      const char *label)
{
    XrmValue value = {0, NULL};
    char *type = NULL;
    CHECK(XrmGetResource(db, name, class_name, &type, &value),
          "%s: XrmGetResource missed", label);
    CHECK(value.addr && !strcmp((char *) value.addr, expected),
          "%s: got %s expected %s", label,
          value.addr ? (char *) value.addr : "(null)", expected);
}

int main(void)
{
    XrmDatabase db = NULL;

    XrmPutStringResource(&db, "top.frame.row.col.button.label.fontList",
                         "tight-font");
    XrmPutStringResource(&db, "*XmLabel.fontList", "loose-font");
    XrmPutStringResource(&db, "*Label.FontList", "class-font");
    XrmPutStringResource(&db, "*fontList", "least-specific-font");

    const char *name = "top.frame.row.col.button.label.fontList";
    const char *class_name = "Top.Frame.Row.Col.Button.XmLabel.FontList";

    check_get(db, name, class_name, "tight-font", "tight path");

    XrmDatabase loose_db = NULL;
    XrmPutStringResource(&loose_db, "*XmLabel.fontList", "loose-font");
    XrmPutStringResource(&loose_db, "*fontList", "least-specific-font");
    check_get(loose_db, name, class_name, "loose-font", "loose class path");

    XrmName names[8];
    XrmClass classes[8];
    XrmStringToNameList("top.frame.row.col.button.label", names);
    XrmStringToClassList("Top.Frame.Row.Col.Button.XmLabel", classes);
    XrmHashTable search[32];
    CHECK(XrmQGetSearchList(db, names, classes, search, 32),
          "XrmQGetSearchList failed for six-level prefix");
    XrmRepresentation qtype = 0;
    XrmValue qvalue = {0, NULL};
    CHECK(XrmQGetSearchResource(search, XrmStringToName("fontList"),
                                XrmStringToClass("FontList"), &qtype, &qvalue),
          "XrmQGetSearchResource missed fontList");
    CHECK(qvalue.addr && !strcmp((char *) qvalue.addr, "tight-font"),
          "XrmQGetSearchResource returned %s",
          qvalue.addr ? (char *) qvalue.addr : "(null)");

    EnumState all = {0, 0, 0};
    XrmEnumerateDatabase(db, NULL, NULL, XrmEnumAllLevels, enum_proc,
                         (XPointer) &all);
    CHECK(all.count >= 4 && all.saw_tight && all.saw_loose,
          "XrmEnumerateDatabase did not report expected entries");

    EnumState one = {0, 0, 0};
    XrmName prefix_names[7];
    XrmClass prefix_classes[7];
    XrmStringToNameList("top.frame.row.col.button.label", prefix_names);
    XrmStringToClassList("Top.Frame.Row.Col.Button.XmLabel", prefix_classes);
    XrmEnumerateDatabase(db, prefix_names, prefix_classes, XrmEnumOneLevel,
                         enum_proc, (XPointer) &one);
    /* Per XrmEnumerateDatabase spec, a one-level enumeration must report
     * every resource that could be reached by appending exactly one
     * name/class to the prefix. Three of the four db entries qualify:
     *   - the tight top...label.fontList (1 component below the prefix)
     *   - *XmLabel.fontList    (loose XmLabel matches prefix class[5])
     *   - *fontList            (loose binding absorbs the whole prefix)
     * *Label.FontList does not because case-sensitive name/class lookup
     * doesn't match the lowercase 'label' in the prefix path. The older
     * implementation rejected loose-bound entries during prefix matching
     * and only saw the tight one. */
    CHECK(one.count == 3 && one.saw_tight && one.saw_loose,
          "one-level enumeration count=%d saw_tight=%d saw_loose=%d", one.count,
          one.saw_tight, one.saw_loose);

    XrmDestroyDatabase(loose_db);
    XrmDestroyDatabase(db);
    puts("test_libxt_resources: ok");
    return 0;
}
