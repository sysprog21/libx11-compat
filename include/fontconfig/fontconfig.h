#ifndef FONTCONFIG_FONTCONFIG_H
#define FONTCONFIG_FONTCONFIG_H

#include <stdarg.h>

typedef unsigned char FcChar8;
typedef unsigned short FcChar16;
typedef unsigned int FcChar32;
typedef int FcBool;
typedef int FcResult;
typedef int FcType;
typedef struct _FcPattern FcPattern;
typedef struct _FcObjectSet FcObjectSet;
typedef struct _FcFontSet {
    int nfont;
    int sfont;
    FcPattern **fonts;
} FcFontSet;
typedef struct _FcCharSet FcCharSet;
typedef struct _FcMatrix {
    double xx;
    double xy;
    double yx;
    double yy;
} FcMatrix;
typedef struct _FcLangSet FcLangSet;
typedef struct _FcConfig FcConfig;

typedef struct _FcValue {
    FcType type;
    union {
        const FcChar8 *s;
        int i;
        double d;
        FcBool b;
        const FcMatrix *m;
        const FcCharSet *c;
        void *f;
        const FcLangSet *l;
    } u;
} FcValue;

#define FcFalse 0
#define FcTrue 1

#define FcResultMatch 0
#define FcResultNoMatch 1
#define FcResultTypeMismatch 2
#define FcResultNoId 3
#define FcResultOutOfMemory 4

#define FcMatchPattern 0

#define FcTypeVoid 0
#define FcTypeInteger 1
#define FcTypeDouble 2
#define FcTypeString 3
#define FcTypeBool 4
#define FcTypeMatrix 5
#define FcTypeCharSet 6
#define FcTypeFTFace 7
#define FcTypeLangSet 8

#define FC_FAMILY "family"
#define FC_STYLE "style"
#define FC_SLANT "slant"
#define FC_WEIGHT "weight"
#define FC_SIZE "size"
#define FC_PIXEL_SIZE "pixelsize"
#define FC_FILE "file"
#define FC_INDEX "index"
#define FC_FOUNDRY "foundry"
#define FC_FULLNAME "fullname"
#define FC_LANG "lang"
#define FC_DPI "dpi"
#define FC_SPACING "spacing"
#define FC_ANTIALIAS "antialias"
#define FC_HINTING "hinting"
#define FC_HINT_STYLE "hintstyle"
#define FC_AUTOHINT "autohint"
#define FC_OUTLINE "outline"
#define FC_SCALABLE "scalable"
#define FC_RGBA "rgba"
#define FC_MINSPACE "minspace"
#define FC_CHARSET "charset"
#define FC_COLOR "color"
#define FC_RENDER "render"
#define FC_NAMELANG "namelang"

#define FC_SLANT_ROMAN 0
#define FC_SLANT_ITALIC 100
#define FC_SLANT_OBLIQUE 110

#define FC_WEIGHT_LIGHT 50
#define FC_WEIGHT_REGULAR 80
#define FC_WEIGHT_MEDIUM 100
#define FC_WEIGHT_BOLD 200

/* Maximum bytes needed to encode any UTF-8 codepoint. The RFC-3629 range stops
 * at 4, but fontconfig sizes scratch buffers off the original 6-byte UTF-8 spec
 * and downstream clients (Xfig) declare arrays of this size. Keep the larger
 * bound so caller-supplied buffers always have room.
 */
#define FC_UTF8_MAX_LEN 6

#define FC_PROPORTIONAL 0
#define FC_DUAL 90
#define FC_MONO 100
#define FC_CHARCELL 110

#define FC_HINT_NONE 0
#define FC_HINT_SLIGHT 1
#define FC_HINT_MEDIUM 2
#define FC_HINT_FULL 3

#define FC_RGBA_UNKNOWN 0
#define FC_RGBA_RGB 1
#define FC_RGBA_BGR 2
#define FC_RGBA_VRGB 3
#define FC_RGBA_VBGR 4
#define FC_RGBA_NONE 5

FcPattern *FcPatternCreate(void);
FcPattern *FcPatternDuplicate(const FcPattern *pattern);
void FcPatternDestroy(FcPattern *pattern);
void FcPatternReference(FcPattern *pattern);
FcBool FcPatternDel(FcPattern *pattern, const char *object);
FcBool FcPatternRemove(FcPattern *pattern, const char *object, int n);
int FcUcs4ToUtf8(FcChar32 ucs4, FcChar8 dest[FC_UTF8_MAX_LEN]);
int FcUtf8ToUcs4(const FcChar8 *src, FcChar32 *dst, int len);
FcPattern *FcNameParse(const FcChar8 *name);
FcBool FcConfigSubstitute(FcConfig *config, FcPattern *pattern, int kind);
void FcDefaultSubstitute(FcPattern *pattern);
FcPattern *FcFontMatch(FcConfig *config, FcPattern *pattern, FcResult *result);
FcFontSet *FcFontList(FcConfig *config, FcPattern *pattern, FcObjectSet *os);
FcFontSet *FcFontSort(FcConfig *config,
                      FcPattern *pattern,
                      FcBool trim,
                      FcCharSet **csp,
                      FcResult *result);
void FcFontSetDestroy(FcFontSet *set);
FcCharSet *FcCharSetCreate(void);
FcBool FcCharSetAddChar(FcCharSet *charset, FcChar32 ucs4);
FcBool FcCharSetHasChar(const FcCharSet *charset, FcChar32 ucs4);
void FcCharSetDestroy(FcCharSet *charset);
FcObjectSet *FcObjectSetCreate(void);
FcBool FcObjectSetAdd(FcObjectSet *os, const char *object);
void FcObjectSetDestroy(FcObjectSet *os);
FcBool FcPatternAdd(FcPattern *pattern,
                    const char *object,
                    FcValue value,
                    FcBool append);
FcBool FcPatternAddString(FcPattern *pattern,
                          const char *object,
                          const FcChar8 *s);
FcBool FcPatternAddInteger(FcPattern *pattern, const char *object, int i);
FcBool FcPatternAddDouble(FcPattern *pattern, const char *object, double d);
FcBool FcPatternAddBool(FcPattern *pattern, const char *object, FcBool b);
FcBool FcPatternAddCharSet(FcPattern *pattern,
                           const char *object,
                           const FcCharSet *c);
FcBool FcPatternAddMatrix(FcPattern *pattern,
                          const char *object,
                          const FcMatrix *matrix);
FcResult FcPatternGet(const FcPattern *pattern,
                      const char *object,
                      int n,
                      FcValue *value);
FcResult FcPatternGetString(const FcPattern *pattern,
                            const char *object,
                            int n,
                            FcChar8 **s);
FcResult FcPatternGetInteger(const FcPattern *pattern,
                             const char *object,
                             int n,
                             int *i);
FcResult FcPatternGetDouble(const FcPattern *pattern,
                            const char *object,
                            int n,
                            double *d);
FcResult FcPatternGetBool(const FcPattern *pattern,
                          const char *object,
                          int n,
                          FcBool *b);

#endif /* FONTCONFIG_FONTCONFIG_H */
