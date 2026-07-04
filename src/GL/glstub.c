#include "stub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "attributes.h"
#include "init.h"

#define STUB(ret, def, args)             \
    ret APIENTRY_GL4ES gl4es_##def args  \
    {                                    \
        if (!globals4es.silentstub)      \
            printf("stub: %s;\n", #def); \
    }                                    \
    AliasExport(ret, def, , args);

STUB(void,
     glClearAccum,
     (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha));
STUB(void,
     glCopyPixels,
     (GLint x, GLint y, GLsizei width, GLsizei height, GLenum type));
STUB(void, glDrawBuffer, (GLenum mode));
STUB(void, glEdgeFlag, (GLboolean flag));
STUB(void, glIndexf, (GLfloat c));
STUB(void, glPolygonStipple, (const GLubyte *mask));
STUB(void, glReadBuffer, (GLenum mode));
STUB(void,
     glColorTable,
     (GLenum target,
      GLenum internalformat,
      GLsizei width,
      GLenum format,
      GLenum type,
      const GLvoid *table));

STUB(void, glAccum, (GLenum op, GLfloat value));
STUB(void,
     glPrioritizeTextures,
     (GLsizei n, const GLuint *textures, const GLclampf *priorities));
STUB(void, glPassThrough, (GLfloat token));
STUB(void, glIndexMask, (GLuint mask));
STUB(void, glClearIndex, (GLfloat c));
STUB(void, glGetPolygonStipple, (GLubyte * pattern));
STUB(void, glFeedbackBuffer, (GLsizei size, GLenum type, GLfloat *buffer));
STUB(void, glEdgeFlagv, (GLboolean * flag));
#undef STUB
