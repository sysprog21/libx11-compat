#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xlibint.h>

static char defaultValue[1024];

static const char *baseProgramName(const char *program)
{
    if (!program)
        return "";
    const char *slash = strrchr(program, '/');
    return slash ? slash + 1 : program;
}

static char *trim(char *text)
{
    while (isspace((unsigned char) *text))
        text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char) end[-1]))
        *--end = '\0';
    return text;
}

static Bool resourceMatches(const char *specifier,
                            const char *program,
                            const char *name)
{
    char exact[512];
    char loose[512];
    snprintf(exact, sizeof(exact), "%s.%s", baseProgramName(program), name);
    snprintf(loose, sizeof(loose), "%s*%s", baseProgramName(program), name);
    size_t nameLength = strlen(name);
    size_t specifierLength = strlen(specifier);

    return !strcmp(specifier, name) || !strcmp(specifier, exact) ||
           !strcmp(specifier, loose) ||
           (specifierLength == nameLength + 1 && specifier[0] == '*' &&
            !strcmp(specifier + 1, name)) ||
           (specifierLength > nameLength &&
            (specifier[specifierLength - nameLength - 1] == '.' ||
             specifier[specifierLength - nameLength - 1] == '*') &&
            !strcmp(specifier + specifierLength - nameLength, name));
}

static char *lookupDefaultsText(const char *data,
                                const char *program,
                                const char *name)
{
    if (!data || !name)
        return NULL;

    char *copy = strdup(data);
    if (!copy)
        return NULL;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *stripped = trim(line);
        if (stripped[0] == '\0' || stripped[0] == '!' || stripped[0] == '#')
            continue;
        char *colon = strchr(stripped, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *specifier = trim(stripped);
        char *value = trim(colon + 1);
        if (resourceMatches(specifier, program, name)) {
            snprintf(defaultValue, sizeof(defaultValue), "%s", value);
            free(copy);
            return defaultValue;
        }
    }
    free(copy);
    return NULL;
}

static char *lookupDefaultsFile(const char *path,
                                const char *program,
                                const char *name)
{
    if (!path || path[0] == '\0')
        return NULL;
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *data = malloc((size_t) size + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t readSize = fread(data, 1, (size_t) size, file);
    fclose(file);
    data[readSize] = '\0';
    char *value = lookupDefaultsText(data, program, name);
    free(data);
    return value;
}

static char *lookupHomeDefaults(const char *program, const char *name)
{
    const char *home = getenv("HOME");
    if (!home)
        return NULL;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.Xdefaults", home);
    return lookupDefaultsFile(path, program, name);
}

static char *lookupBuiltInDefault(const char *name)
{
    if (!name)
        return NULL;
    static const struct {
        const char *name;
        char *value;
    } builtIns[] = {
        {"font", "fixed"},
        {"Font", "fixed"},
        {"background", "#c4c4c4"},
        {"Background", "#c4c4c4"},
        {"dpi", "96"},
        {"DPI", "96"},
        {"Dpi", "96"},
        {"Xft.dpi", "96"},
        {"xft.dpi", "96"},
    };
    for (size_t i = 0; i < sizeof(builtIns) / sizeof(builtIns[0]); i++) {
        if (!strcmp(name, builtIns[i].name))
            return builtIns[i].value;
    }
    return NULL;
}

char *XGetDefault(Display *display,
                  char _Xconst *program,
                  register _Xconst char *name)
{
    char *value;
    if (display) {
        value = lookupDefaultsText(display->xdefaults, program, name);
        if (value)
            return value;
    }
    if ((value = lookupDefaultsFile(getenv("XENVIRONMENT"), program, name)))
        return value;
    if ((value = lookupHomeDefaults(program, name)))
        return value;
    return lookupBuiltInDefault(name);
}
