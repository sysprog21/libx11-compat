#include "hardext.h"

#include "debug.h"
#include "gl4es.h"
#include "init.h"
#include "logs.h"
#include "loader.h"

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

static int tested = 0;

hardext_t hardext = {0};

char *gl4es_original_vendor = NULL;
char *gl4es_original_renderer = NULL;

static int testGLSL(const char *version, int uniformLoc)
{
    // check if glsl 120 shaders are supported... by compiling one !
    LOAD_GLES2(glCreateShader);
    LOAD_GLES2(glShaderSource);
    LOAD_GLES2(glCompileShader);
    LOAD_GLES2(glGetShaderiv);
    LOAD_GLES2(glDeleteShader);
    LOAD_GLES(glGetError);

    GLuint shad = gles_glCreateShader(GL_VERTEX_SHADER);
    const char *shadTest[4] = {
        version,
        "#extension require GL_IMG_uniform_buffer_object"
        "\n"
        "layout(location = 0) in vec4 vecPos;\n",
        uniformLoc ? "layout(location = 0) uniform mat4 matMVP;\n"
                   : "uniform mat4 matMVP;\n",
        "void main() {\n"
        " gl_Position = matMVP * vecPos;\n"
        "}\n"};
    gles_glShaderSource(shad, 4, shadTest, NULL);
    gles_glCompileShader(shad);
    GLint compiled;
    gles_glGetShaderiv(shad, GL_COMPILE_STATUS, &compiled);
    gles_glDeleteShader(shad);
    gles_glGetError();  // reset GL Error

    return compiled;
}

static int testTextureCubeLod()
{
    LOAD_GLES2(glCreateShader);
    LOAD_GLES2(glShaderSource);
    LOAD_GLES2(glCompileShader);
    LOAD_GLES2(glGetShaderiv);
    LOAD_GLES2(glDeleteShader);
    LOAD_GLES(glGetError);

    GLuint shad = gles_glCreateShader(GL_FRAGMENT_SHADER);
    const char *shadTest[3] = {
        "#version 100",
        "\n"
        "#extension GL_EXT_shader_texture_lod : enable\n"
        "uniform samplerCube samCube;\n"
        "varying mediump vec3 coordCube;\n",
        "void main() {\n"
        " gl_FragColor = textureCubeLod(samCube, coordCube, 0.0);\n"
        "}\n"};
    gles_glShaderSource(shad, 3, shadTest, NULL);
    gles_glCompileShader(shad);
    GLint compiled;
    gles_glGetShaderiv(shad, GL_COMPILE_STATUS, &compiled);
    gles_glDeleteShader(shad);
    gles_glGetError();  // reset GL Error

    return compiled;
}

EXPORT
void GetHardwareExtensions(int notest)
{
    if (tested)
        return;
    // put some default values
    hardext.maxtex = 2;
    hardext.maxsize = 2048;
    hardext.maxlights = 8;
    hardext.maxplanes = 6;
    hardext.maxdrawbuffers = 1;

    hardext.esversion = globals4es.es;
    if (notest) {
        SHUT_LOGD("Hardware test disabled, nothing activated...\n");
        if (hardext.esversion == 2) {
            hardext.maxteximage = 4;
            hardext.maxvarying = 8;
            hardext.maxtex = 8;
            hardext.maxvattrib = 16;
            hardext.npot = 1;
            hardext.fbo = 1;
            hardext.blendcolor = 1;
            hardext.blendsub = 1;
            hardext.blendfunc = 1;
            hardext.blendeq = 1;
            hardext.mirrored = 1;
            hardext.pointsprite = 1;
            hardext.pointsize = 1;
            hardext.cubemap = 1;
            hardext.maxdrawbuffers = 1;
        }
        return;
    }
    SHUT_LOGD("Hardware test on current Context...\n");
    tested = 1;
    LOAD_GLES(glGetString);
    LOAD_GLES(glGetIntegerv);
    LOAD_GLES(glGetError);
    // Now get extensions
    const char *Exts = (const char *) gles_glGetString(GL_EXTENSIONS);
// Parse them!
#define S(A, B, C)                                                         \
    if (strstr(Exts, A)) {                                                 \
        hardext.B = 1;                                                     \
        SHUT_LOGD("Extension %s detected%s", A, C ? " and used\n" : "\n"); \
    }
    if (hardext.esversion > 1)
        hardext.npot = 1;
    if (strstr(Exts, "GL_APPLE_texture_2D_limited_npot "))
        hardext.npot = 1;
    if (strstr(Exts, "GL_IMG_texture_npot "))
        hardext.npot = 1;  // it should enable mipmap (so hardext.npot=2), but
                           // mipmap (so level > 0) needs to be POT-sized?!!
    if (strstr(Exts, "GL_ARB_texture_non_power_of_two ") ||
        strstr(Exts, "GL_OES_texture_npot "))
        hardext.npot = 3;
    if (hardext.npot > 0) {
        SHUT_LOGD("Hardware %s NPOT detected and used\n",
                  hardext.npot == 3
                      ? "Full"
                      : (hardext.npot == 2 ? "Limited+Mipmap" : "Limited"));
    }
    S("GL_EXT_blend_minmax ", blendminmax, 1);
    if (hardext.esversion > 2) {
        SHUT_LOGD(
            "Extension GL_EXT_draw_buffers is in core ES3, and so used\n");
        hardext.drawbuffers = 1;
    } else {
        S("GL_EXT_draw_buffers ", drawbuffers, 1);
    }
    if (hardext.esversion < 2) {
        S("GL_OES_framebuffer_object ", fbo, 1);
        S("GL_OES_point_sprite ", pointsprite, 1);
        S("GL_OES_point_size_array ", pointsize, 0);
        S("GL_OES_texture_cube_map ", cubemap, 1);
        S("GL_EXT_blend_color ", blendcolor, 1);
        S("GL_OES_blend_subtract ", blendsub, 1);
        S("GL_OES_blend_func_separate ", blendfunc, 1);
        S("GL_OES_blend_equation_separate ", blendeq, 1);
        S("GL_OES_texture_mirrored_repeat ", mirrored, 1);
    } else {
        hardext.fbo = 1;
        SHUT_LOGD("FBO are in core, and so used\n");
        hardext.pointsprite = 1;
        SHUT_LOGD("PointSprite are in core, and so used\n");
        hardext.pointsize = 1;
        SHUT_LOGD("CubeMap are in core, and so used\n");
        hardext.cubemap = 1;
        SHUT_LOGD("BlendColor is in core, and so used\n");
        hardext.blendcolor = 1;
        SHUT_LOGD("Blend Subtract is in core, and so used\n");
        hardext.blendsub = 1;
        SHUT_LOGD(
            "Blend Function and Equation Separation is in core, and so used\n");
        hardext.blendfunc = 1;
        hardext.blendeq = 1;
        SHUT_LOGD("Texture Mirrored Repeat is in core, and so used\n");
        hardext.mirrored = 1;
    }
    S("GL_OES_mapbuffer ", mapbuffer, 0);
    S("GL_OES_element_index_uint ", elementuint, 1);
    S("GL_OES_packed_depth_stencil ", depthstencil, 1);
    S("GL_OES_depth24 ", depth24, 1);
    S("GL_OES_rgb8_rgba8 ", rgba8, 1);
    S("GL_EXT_multi_draw_arrays ", multidraw, 0);
    if (!globals4es.nobgra) {
        S("GL_EXT_texture_format_BGRA8888 ", bgra8888, 1);
    }
    if (!globals4es.nodepthtex) {
        S("GL_OES_depth_texture ", depthtex, 1);
        S("GL_OES_texture_stencil8 ", stenciltex, 1);
    }
    S("GL_OES_draw_texture ", drawtex, 1);
    S("GL_EXT_texture_rg ", rgtex, 1);
    if (globals4es.floattex) {
        S("GL_OES_texture_float ", floattex, 1);
        S("GL_OES_texture_half_float ", halffloattex, 1);
        S("GL_EXT_color_buffer_float ", floatfbo, 1);
        S("GL_EXT_color_buffer_half_float ", halffloatfbo, 1);
    }
    S("GL_AOS4_texture_format_RGB332", rgb332, 0);
    S("GL_AOS4_texture_format_RGB332REV", rgb332rev, 0);
    S("GL_AOS4_texture_format_RGBA1555REV", rgba1555rev, 1);
    S("GL_AOS4_texture_format_RGBA8888", rgba8888, 1);
    S("GL_AOS4_texture_format_RGBA8888REV", rgba8888rev, 1);

    if (hardext.esversion > 1) {
        if (!globals4es.nohighp) {
            S("GL_OES_fragment_precision_high ", highp, 1);
            if (!hardext.highp) {
                // check if highp is supported anyway
                LOAD_GLES2(glGetShaderPrecisionFormat);
                if (gles_glGetShaderPrecisionFormat) {
                    GLint range[2] = {0};
                    GLint precision = 0;
                    gles_glGetShaderPrecisionFormat(
                        GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
                    if (!(range[0] == 0 && range[1] == 0 && precision == 0)) {
                        hardext.highp =
                            2;  // no need to declare #entension here
                        SHUT_LOGD(
                            "high precision float in fragment shader available "
                            "and used\n");
                    }
                }
            }
        }
        if (!globals4es.noshaderlod)
            S("GL_EXT_shader_texture_lod", shaderlod, 1);
        if (hardext.shaderlod) {
            // test is textureCubeLod need EXT or not (seems to be a bug in some
            // PVR driver)
            if (testTextureCubeLod()) {
                hardext.cubelod = 1;
                SHUT_LOGD(
                    "textureCubeLod in fragment doesn't need trailing EXT\n");
            }
        }
        S("GL_EXT_frag_depth ", fragdepth, 1);
        gles_glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &hardext.maxvattrib);
        SHUT_LOGD("Max vertex attrib: %d\n", hardext.maxvattrib);
        S("GL_OES_standard_derivatives ", derivatives, 1);
        S("GL_ARM_shader_framebuffer_fetch", shader_fbfetch, 1);
        S("GL_OES_get_program ", prgbinary, 1);
        if (!hardext.prgbinary) {
            S("GL_OES_get_program_binary ", prgbinary, 1);
        }
        if (hardext.prgbinary) {
            gles_glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS_OES,
                               &hardext.prgbin_n);
            SHUT_LOGD("Number of supported Program Binary Format: %d\n",
                      hardext.prgbin_n);
        }
    }
    // Now get some max stuffs
    gles_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &hardext.maxsize);
    SHUT_LOGD("Max texture size: %d\n", hardext.maxsize);
    gles_glGetIntegerv((hardext.esversion == 1) ? GL_MAX_TEXTURE_UNITS
                                                : GL_MAX_TEXTURE_IMAGE_UNITS,
                       &hardext.maxtex);
    if (hardext.esversion == 1) {
        gles_glGetIntegerv(GL_MAX_LIGHTS, &hardext.maxlights);
        gles_glGetIntegerv(GL_MAX_CLIP_PLANES, &hardext.maxplanes);
        hardext.maxteximage = hardext.maxtex;
    } else {
        // simulated stuff using the FPE
        hardext.maxlights = 8;
        hardext.maxplanes = 6;
        gles_glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &hardext.maxteximage);
        gles_glGetIntegerv(GL_MAX_VARYING_VECTORS, &hardext.maxvarying);
        SHUT_LOGD("Max Varying Vector: %d\n", hardext.maxvarying);
        if (hardext.maxvattrib < 16 && hardext.maxtex > 4)
            hardext.maxtex = 4;  // with less then 16 vertexattrib, more then 4
                                 // textures seems unreasonnable
    }
    int hardmaxtex = hardext.maxtex;
    if (hardext.maxtex > MAX_TEX)
        hardext.maxtex =
            MAX_TEX;  // caping, as there are some fixed-sized array...
    if (hardext.maxteximage > MAX_TEX)
        hardext.maxteximage = MAX_TEX;
    if (hardext.maxlights > MAX_LIGHT)
        hardext.maxlights = MAX_LIGHT;  // caping lights too
    if (hardext.maxplanes > MAX_CLIP_PLANES)
        hardext.maxplanes = MAX_CLIP_PLANES;  // caping planes, even 6 should be
                                              // the max supported anyway
    SHUT_LOGD(
        "Texture Units: %d/%d (hardware: %d), Max lights: %d, Max planes: %d\n",
        hardext.maxtex, hardext.maxteximage, hardmaxtex, hardext.maxlights,
        hardext.maxplanes);
    S("GL_EXT_texture_filter_anisotropic ", aniso, 1);
    if (hardext.aniso) {
        gles_glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &hardext.aniso);
        if (gles_glGetError() != GL_NO_ERROR)
            hardext.aniso = 0;
        if (hardext.aniso)
            SHUT_LOGD("Max Anisotropic filtering: %d\n", hardext.aniso);
    }
    if (hardext.drawbuffers) {
        gles_glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS_EXT,
                           &hardext.maxcolorattach);
        gles_glGetIntegerv(GL_MAX_DRAW_BUFFERS_ARB, &hardext.maxdrawbuffers);
    }
    if (hardext.maxcolorattach < 1)
        hardext.maxcolorattach = 1;
    if (hardext.maxcolorattach > MAX_DRAW_BUFFERS)
        hardext.maxcolorattach = MAX_DRAW_BUFFERS;
    if (hardext.maxdrawbuffers < 1)
        hardext.maxdrawbuffers = 1;
    if (hardext.maxdrawbuffers > MAX_DRAW_BUFFERS)
        hardext.maxdrawbuffers = MAX_DRAW_BUFFERS;
    SHUT_LOGD("Max Color Attachments: %d / Draw buffers: %d\n",
              hardext.maxdrawbuffers, hardext.maxcolorattach);
    // get GLES driver signatures...
    const char *vendor = (const char *) gles_glGetString(GL_VENDOR);
    SHUT_LOGD("Hardware vendor is %s\n", vendor);
    if (!gl4es_original_vendor) {
        gl4es_original_vendor = strdup(vendor);
    }
    if (strstr(vendor, "ARM"))
        hardext.vendor = VEND_ARM;
    else if (strstr(vendor, "Imagination Technologies"))
        hardext.vendor = VEND_IMGTEC;
    if (hardext.esversion > 1) {
        if (testGLSL("#version 120", 1))
            hardext.glsl120 = 1;
        if (testGLSL("#version 300 es", 0))
            hardext.glsl300es = 1;
        if (testGLSL("#version 310 es", 1))
            hardext.glsl310es = 1;
    }
    if (!gl4es_original_renderer) {
        const char *renderer = (const char *) gles_glGetString(GL_RENDERER);
        gl4es_original_renderer = strdup(renderer);
    }
    if (hardext.glsl120) {
        SHUT_LOGD("GLSL 120 supported and used\n");
    }
    if (hardext.glsl300es) {
        SHUT_LOGD("GLSL 300 es supported%s\n",
                  (hardext.glsl120 || hardext.glsl310es) ? "" : " and used");
    }
    if (hardext.glsl310es) {
        SHUT_LOGD("GLSL 310 es supported%s\n",
                  hardext.glsl120 ? "" : " and used");
    }
}
