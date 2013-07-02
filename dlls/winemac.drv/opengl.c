/*
 * Mac driver OpenGL support
 *
 * Copyright 2012 Alexandre Julliard
 * Copyright 2012, 2013 Ken Thomases for CodeWeavers Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include "macdrv.h"

#include "winuser.h"
#include "winternl.h"
#include "wine/library.h"
#include "wine/debug.h"
#include "wine/wgl.h"
#include "wine/wgl_driver.h"
#include "wine/wglext.h"

#define __gl_h_
#define __gltypes_h_
#include <OpenGL/OpenGL.h>
#include <OpenGL/glu.h>
#include <OpenGL/CGLRenderers.h>
#include <dlfcn.h>

WINE_DEFAULT_DEBUG_CHANNEL(wgl);


struct gl_info {
    char *glVersion;
    char *glExtensions;

    char wglExtensions[4096];

    GLint max_viewport_dims[2];
};

static struct gl_info gl_info;


struct wgl_context
{
    HDC                     hdc;
    int                     format;
    macdrv_opengl_context   context;
    CGLContextObj           cglcontext;
    macdrv_view             draw_view;
    struct wgl_pbuffer     *draw_pbuffer;
    macdrv_view             read_view;
    struct wgl_pbuffer     *read_pbuffer;
    BOOL                    has_been_current;
    BOOL                    sharing;
    DWORD                   last_flush_time;
};


struct wgl_pbuffer
{
    CGLPBufferObj   pbuffer;
    int             format;
    BOOL            no_texture;
    int             max_level;
    GLint           level;
    GLenum          face;
};

static CFMutableDictionaryRef dc_pbuffers;

static CRITICAL_SECTION dc_pbuffers_section;
static CRITICAL_SECTION_DEBUG dc_pbuffers_section_debug =
{
    0, 0, &dc_pbuffers_section,
    { &dc_pbuffers_section_debug.ProcessLocksList, &dc_pbuffers_section_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dc_pbuffers_section") }
};
static CRITICAL_SECTION dc_pbuffers_section = { &dc_pbuffers_section_debug, -1, 0, 0, 0, 0 };


static struct opengl_funcs opengl_funcs;

#define USE_GL_FUNC(name) #name,
static const char *opengl_func_names[] = { ALL_WGL_FUNCS };
#undef USE_GL_FUNC


static void (*pglCopyColorTable)(GLenum target, GLenum internalformat, GLint x, GLint y,
                                 GLsizei width);
static void (*pglCopyPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type);
static void (*pglFlush)(void);
static void (*pglFlushRenderAPPLE)(void);
static void (*pglReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, void *pixels);
static void (*pglViewport)(GLint x, GLint y, GLsizei width, GLsizei height);


struct color_mode {
    GLint   mode;
    int     bits_per_pixel;
    GLint   color_bits; /* including alpha_bits */
    int     red_bits, red_shift;
    int     green_bits, green_shift;
    int     blue_bits, blue_shift;
    GLint   alpha_bits, alpha_shift;
    BOOL    is_float;
    int     color_ordering;
};

/* The value of "color_ordering" is somewhat arbitrary.  It incorporates some
   observations of the behavior of Windows systems, but also subjective judgments
   about what color formats are more "normal" than others.

   On at least some Windows systems, integer color formats are listed before
   floating-point formats.  Within the integer formats, higher color bits were
   usually listed before lower color bits, while for floating-point formats it
   was the reverse.  However, that leads D3D to select 64-bit integer formats in
   preference to 32-bit formats when the latter would be sufficient.  It seems
   that a 32-bit format is much more likely to be normally used in that case.

   Also, there are certain odd color formats supported on the Mac which seem like
   they would be less appropriate than more common ones.  For instance, the color
   formats with alpha in a separate byte (e.g. kCGLRGB888A8Bit with R8G8B8 in one
   32-bit value and A8 in a separate 8-bit value) and the formats with 10-bit RGB
   components.

   For two color formats which differ only in whether or not they have alpha bits,
   we use the same ordering.  pixel_format_comparator() gives alpha bits a
   different weight than color formats.
 */
static const struct color_mode color_modes[] = {
    { kCGLRGB444Bit,        16,     12,     4,  8,      4,  4,      4,  0,      0,  0,  FALSE,  5 },
    { kCGLARGB4444Bit,      16,     16,     4,  8,      4,  4,      4,  0,      4,  12, FALSE,  5 },
    { kCGLRGB444A8Bit,      24,     20,     4,  8,      4,  4,      4,  0,      8,  16, FALSE,  10 },
    { kCGLRGB555Bit,        16,     15,     5,  10,     5,  5,      5,  0,      0,  0,  FALSE,  4 },
    { kCGLARGB1555Bit,      16,     16,     5,  10,     5,  5,      5,  0,      1,  15, FALSE,  4 },
    { kCGLRGB555A8Bit,      24,     23,     5,  10,     5,  5,      5,  0,      8,  16, FALSE,  9 },
    { kCGLRGB565Bit,        16,     16,     5,  11,     6,  5,      5,  0,      0,  0,  FALSE,  3 },
    { kCGLRGB565A8Bit,      24,     24,     5,  11,     6,  5,      5,  0,      8,  16, FALSE,  8 },
    { kCGLRGB888Bit,        32,     24,     8,  16,     8,  8,      8,  0,      0,  0,  FALSE,  0 },
    { kCGLARGB8888Bit,      32,     32,     8,  16,     8,  8,      8,  0,      8,  24, FALSE,  0 },
    { kCGLRGB888A8Bit,      40,     32,     8,  16,     8,  8,      8,  0,      8,  32, FALSE,  7 },
    { kCGLRGB101010Bit,     32,     30,     10, 20,     10, 10,     10, 0,      0,  0,  FALSE,  6 },
    { kCGLARGB2101010Bit,   32,     32,     10, 20,     10, 10,     10, 0,      2,  30, FALSE,  6 },
    { kCGLRGB101010_A8Bit,  40,     38,     10, 20,     10, 10,     10, 0,      8,  32, FALSE,  11 },
    { kCGLRGB121212Bit,     48,     36,     12, 24,     12, 12,     12, 0,      0,  0,  FALSE,  2 },
    { kCGLARGB12121212Bit,  48,     48,     12, 24,     12, 12,     12, 0,      12, 36, FALSE,  2 },
    { kCGLRGB161616Bit,     64,     48,     16, 48,     16, 32,     16, 16,     0,  0,  FALSE,  1 },
    { kCGLRGBA16161616Bit,  64,     64,     16, 48,     16, 32,     16, 16,     16, 0,  FALSE,  1 },
    { kCGLRGBFloat64Bit,    64,     48,     16, 32,     16, 16,     16, 0,      0,  0,  TRUE,   12 },
    { kCGLRGBAFloat64Bit,   64,     64,     16, 48,     16, 32,     16, 16,     16, 0,  TRUE,   12 },
    { kCGLRGBFloat128Bit,   128,    96,     32, 96,     32, 64,     32, 32,     0,  0,  TRUE,   13 },
    { kCGLRGBAFloat128Bit,  128,    128,    32, 96,     32, 64,     32, 32,     32, 0,  TRUE,   13 },
    { kCGLRGBFloat256Bit,   256,    192,    64, 192,    64, 128,    64, 64,     0,  0,  TRUE,   14 },
    { kCGLRGBAFloat256Bit,  256,    256,    64, 192,    64, 128,    64, 64,     64, 0,  TRUE,   15 },
};


static const struct {
    GLint   mode;
    int     bits;
} depth_stencil_modes[] = {
    { kCGL0Bit,     0 },
    { kCGL1Bit,     1 },
    { kCGL2Bit,     2 },
    { kCGL3Bit,     3 },
    { kCGL4Bit,     4 },
    { kCGL5Bit,     5 },
    { kCGL6Bit,     6 },
    { kCGL8Bit,     8 },
    { kCGL10Bit,    10 },
    { kCGL12Bit,    12 },
    { kCGL16Bit,    16 },
    { kCGL24Bit,    24 },
    { kCGL32Bit,    32 },
    { kCGL48Bit,    48 },
    { kCGL64Bit,    64 },
    { kCGL96Bit,    96 },
    { kCGL128Bit,   128 },
};


typedef struct {
    GLint   renderer_id;
    GLint   buffer_modes;
    GLint   color_modes;
    GLint   accum_modes;
    GLint   depth_modes;
    GLint   stencil_modes;
    GLint   max_aux_buffers;
    GLint   max_sample_buffers;
    GLint   max_samples;
    BOOL    offscreen;
    BOOL    accelerated;
    BOOL    backing_store;
    BOOL    window;
    BOOL    online;
} renderer_properties;


typedef struct {
    unsigned int window:1;
    unsigned int pbuffer:1;
    unsigned int accelerated:1;
    unsigned int color_mode:5; /* index into color_modes table */
    unsigned int aux_buffers:3;
    unsigned int depth_bits:8;
    unsigned int stencil_bits:8;
    unsigned int accum_mode:5; /* 1 + index into color_modes table (0 means no accum buffer) */
    unsigned int double_buffer:1;
    unsigned int stereo:1;
    unsigned int sample_buffers:1;
    unsigned int samples:5;
    unsigned int backing_store:1;
} pixel_format;


typedef union
{
    pixel_format    format;
    UInt64          code;
} pixel_format_or_code;
C_ASSERT(sizeof(((pixel_format_or_code*)0)->format) <= sizeof(((pixel_format_or_code*)0)->code));


static pixel_format *pixel_formats;
static int nb_formats, nb_displayable_formats;


static void *opengl_handle;


static const char* debugstr_attrib(int attrib, int value)
{
    static const struct {
        int attrib;
        const char *name;
    } attrib_names[] = {
#define ATTRIB(a) { a, #a }
        ATTRIB(WGL_ACCELERATION_ARB),
        ATTRIB(WGL_ACCUM_ALPHA_BITS_ARB),
        ATTRIB(WGL_ACCUM_BITS_ARB),
        ATTRIB(WGL_ACCUM_BLUE_BITS_ARB),
        ATTRIB(WGL_ACCUM_GREEN_BITS_ARB),
        ATTRIB(WGL_ACCUM_RED_BITS_ARB),
        ATTRIB(WGL_ALPHA_BITS_ARB),
        ATTRIB(WGL_ALPHA_SHIFT_ARB),
        ATTRIB(WGL_AUX_BUFFERS_ARB),
        ATTRIB(WGL_BIND_TO_TEXTURE_RECTANGLE_RGB_NV),
        ATTRIB(WGL_BIND_TO_TEXTURE_RECTANGLE_RGBA_NV),
        ATTRIB(WGL_BIND_TO_TEXTURE_RGB_ARB),
        ATTRIB(WGL_BIND_TO_TEXTURE_RGBA_ARB),
        ATTRIB(WGL_BLUE_BITS_ARB),
        ATTRIB(WGL_BLUE_SHIFT_ARB),
        ATTRIB(WGL_COLOR_BITS_ARB),
        ATTRIB(WGL_DEPTH_BITS_ARB),
        ATTRIB(WGL_DOUBLE_BUFFER_ARB),
        ATTRIB(WGL_DRAW_TO_BITMAP_ARB),
        ATTRIB(WGL_DRAW_TO_PBUFFER_ARB),
        ATTRIB(WGL_DRAW_TO_WINDOW_ARB),
        ATTRIB(WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB),
        ATTRIB(WGL_GREEN_BITS_ARB),
        ATTRIB(WGL_GREEN_SHIFT_ARB),
        ATTRIB(WGL_NEED_PALETTE_ARB),
        ATTRIB(WGL_NEED_SYSTEM_PALETTE_ARB),
        ATTRIB(WGL_NUMBER_OVERLAYS_ARB),
        ATTRIB(WGL_NUMBER_PIXEL_FORMATS_ARB),
        ATTRIB(WGL_NUMBER_UNDERLAYS_ARB),
        ATTRIB(WGL_PIXEL_TYPE_ARB),
        ATTRIB(WGL_RED_BITS_ARB),
        ATTRIB(WGL_RED_SHIFT_ARB),
        ATTRIB(WGL_SAMPLE_BUFFERS_ARB),
        ATTRIB(WGL_SAMPLES_ARB),
        ATTRIB(WGL_SHARE_ACCUM_ARB),
        ATTRIB(WGL_SHARE_DEPTH_ARB),
        ATTRIB(WGL_SHARE_STENCIL_ARB),
        ATTRIB(WGL_STENCIL_BITS_ARB),
        ATTRIB(WGL_STEREO_ARB),
        ATTRIB(WGL_SUPPORT_GDI_ARB),
        ATTRIB(WGL_SUPPORT_OPENGL_ARB),
        ATTRIB(WGL_SWAP_LAYER_BUFFERS_ARB),
        ATTRIB(WGL_SWAP_METHOD_ARB),
        ATTRIB(WGL_TRANSPARENT_ALPHA_VALUE_ARB),
        ATTRIB(WGL_TRANSPARENT_ARB),
        ATTRIB(WGL_TRANSPARENT_BLUE_VALUE_ARB),
        ATTRIB(WGL_TRANSPARENT_GREEN_VALUE_ARB),
        ATTRIB(WGL_TRANSPARENT_INDEX_VALUE_ARB),
        ATTRIB(WGL_TRANSPARENT_RED_VALUE_ARB),
#undef ATTRIB
    };
    int i;
    const char *attrib_name = NULL;
    const char *value_name = NULL;

    for (i = 0; i < sizeof(attrib_names) / sizeof(attrib_names[0]); i++)
    {
        if (attrib_names[i].attrib == attrib)
        {
            attrib_name = attrib_names[i].name;
            break;
        }
    }

    if (!attrib_name)
        attrib_name = wine_dbg_sprintf("Attrib 0x%04x", attrib);

    switch (attrib)
    {
        case WGL_ACCELERATION_ARB:
            switch (value)
            {
                case WGL_FULL_ACCELERATION_ARB:     value_name = "WGL_FULL_ACCELERATION_ARB"; break;
                case WGL_GENERIC_ACCELERATION_ARB:  value_name = "WGL_GENERIC_ACCELERATION_ARB"; break;
                case WGL_NO_ACCELERATION_ARB:       value_name = "WGL_NO_ACCELERATION_ARB"; break;
            }
            break;
        case WGL_PIXEL_TYPE_ARB:
            switch (value)
            {
                case WGL_TYPE_COLORINDEX_ARB:           value_name = "WGL_TYPE_COLORINDEX_ARB"; break;
                case WGL_TYPE_RGBA_ARB:                 value_name = "WGL_TYPE_RGBA_ARB"; break;
                case WGL_TYPE_RGBA_FLOAT_ARB:           value_name = "WGL_TYPE_RGBA_FLOAT_ARB"; break;
                case WGL_TYPE_RGBA_UNSIGNED_FLOAT_EXT:  value_name = "WGL_TYPE_RGBA_UNSIGNED_FLOAT_EXT"; break;
            }
            break;
        case WGL_SWAP_METHOD_ARB:
            switch (value)
            {
                case WGL_SWAP_COPY_ARB:         value_name = "WGL_SWAP_COPY_ARB"; break;
                case WGL_SWAP_EXCHANGE_ARB:     value_name = "WGL_SWAP_EXCHANGE_ARB"; break;
                case WGL_SWAP_UNDEFINED_ARB:    value_name = "WGL_SWAP_UNDEFINED_ARB"; break;
            }
            break;
    }

    if (!value_name)
        value_name = wine_dbg_sprintf("%d / 0x%04x", value, value);

    return wine_dbg_sprintf("%40s: %s", attrib_name, value_name);
}


static BOOL get_renderer_property(CGLRendererInfoObj renderer_info, GLint renderer_index,
                                  CGLRendererProperty property, GLint *value)
{
    CGLError err = CGLDescribeRenderer(renderer_info, renderer_index, property, value);
    if (err != kCGLNoError)
        WARN("CGLDescribeRenderer failed for property %d: %d %s\n", property, err, CGLErrorString(err));
    return (err == kCGLNoError);
}


static void get_renderer_properties(CGLRendererInfoObj renderer_info, int renderer_index, renderer_properties* properties)
{
    GLint value;

    memset(properties, 0, sizeof(*properties));

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPRendererID, &value))
        properties->renderer_id = value & kCGLRendererIDMatchingMask;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPBufferModes, &value))
        properties->buffer_modes = value;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPColorModes, &value))
        properties->color_modes = value;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPAccumModes, &value))
        properties->accum_modes = value;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPDepthModes, &value))
        properties->depth_modes = value;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPStencilModes, &value))
        properties->stencil_modes = value;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPMaxAuxBuffers, &value))
        properties->max_aux_buffers = value;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPMaxSampleBuffers, &value))
        properties->max_sample_buffers = value;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPMaxSamples, &value))
        properties->max_samples = value;

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPOffScreen, &value))
        properties->offscreen = (value != 0);

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPAccelerated, &value))
        properties->accelerated = (value != 0);

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPBackingStore, &value))
        properties->backing_store = (value != 0);

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPWindow, &value))
        properties->window = (value != 0);

    if (get_renderer_property(renderer_info, renderer_index, kCGLRPOnline, &value))
        properties->online = (value != 0);
}


static void dump_renderer(const renderer_properties* renderer)
{
    int i;

    TRACE("Renderer ID: 0x%08x\n", renderer->renderer_id);
    TRACE("Buffer modes:\n");
    TRACE("    Monoscopic:    %s\n", (renderer->buffer_modes & kCGLMonoscopicBit) ? "YES" : "NO");
    TRACE("    Stereoscopic:  %s\n", (renderer->buffer_modes & kCGLStereoscopicBit) ? "YES" : "NO");
    TRACE("    Single buffer: %s\n", (renderer->buffer_modes & kCGLSingleBufferBit) ? "YES" : "NO");
    TRACE("    Double buffer: %s\n", (renderer->buffer_modes & kCGLDoubleBufferBit) ? "YES" : "NO");

    TRACE("Color buffer modes:\n");
    for (i = 0; i < sizeof(color_modes)/sizeof(color_modes[0]); i++)
    {
        if (renderer->color_modes & color_modes[i].mode)
        {
            TRACE("    Color size %d, Alpha size %d", color_modes[i].color_bits, color_modes[i].alpha_bits);
            if (color_modes[i].is_float)
                TRACE(", Float");
            TRACE("\n");
        }
    }

    TRACE("Accumulation buffer sizes: { ");
    for (i = 0; i < sizeof(color_modes)/sizeof(color_modes[0]); i++)
    {
        if (renderer->accum_modes & color_modes[i].mode)
            TRACE("%d, ", color_modes[i].color_bits);
    }
    TRACE("}\n");

    TRACE("Depth buffer sizes: { ");
    for (i = 0; i < sizeof(depth_stencil_modes)/sizeof(depth_stencil_modes[0]); i++)
    {
        if (renderer->depth_modes & depth_stencil_modes[i].mode)
            TRACE("%d, ", depth_stencil_modes[i].bits);
    }
    TRACE("}\n");

    TRACE("Stencil buffer sizes: { ");
    for (i = 0; i < sizeof(depth_stencil_modes)/sizeof(depth_stencil_modes[0]); i++)
    {
        if (renderer->stencil_modes & depth_stencil_modes[i].mode)
            TRACE("%d, ", depth_stencil_modes[i].bits);
    }
    TRACE("}\n");

    TRACE("Max. Auxiliary Buffers: %d\n", renderer->max_aux_buffers);
    TRACE("Max. Sample Buffers: %d\n", renderer->max_sample_buffers);
    TRACE("Max. Samples: %d\n", renderer->max_samples);
    TRACE("Offscreen: %s\n", renderer->offscreen ? "YES" : "NO");
    TRACE("Accelerated: %s\n", renderer->accelerated ? "YES" : "NO");
    TRACE("Backing store: %s\n", renderer->backing_store ? "YES" : "NO");
    TRACE("Window: %s\n", renderer->window ? "YES" : "NO");
    TRACE("Online: %s\n", renderer->online ? "YES" : "NO");
}


static inline UInt64 code_for_pixel_format(const pixel_format* format)
{
    pixel_format_or_code pfc;

    pfc.code = 0;
    pfc.format = *format;
    return pfc.code;
}


static inline pixel_format pixel_format_for_code(UInt64 code)
{
    pixel_format_or_code pfc;

    pfc.code = code;
    return pfc.format;
}


static const char *debugstr_pf(const pixel_format *pf)
{
    return wine_dbg_sprintf("w/p/a %u/%u/%u col %u%s/%u dp/stn/ac/ax/b/db/str %u/%u/%u/%u/%u/%u/%u samp %u/%u %017llx",
                            pf->window,
                            pf->pbuffer,
                            pf->accelerated,
                            color_modes[pf->color_mode].color_bits,
                            (color_modes[pf->color_mode].is_float ? "f" : ""),
                            color_modes[pf->color_mode].alpha_bits,
                            pf->depth_bits,
                            pf->stencil_bits,
                            pf->accum_mode ? color_modes[pf->accum_mode - 1].color_bits : 0,
                            pf->aux_buffers,
                            pf->backing_store,
                            pf->double_buffer,
                            pf->stereo,
                            pf->sample_buffers,
                            pf->samples,
                            code_for_pixel_format(pf));
}


static unsigned int best_color_mode(GLint modes, GLint color_size, GLint alpha_size, GLint color_float)
{
    int best = -1;
    int i;

    for (i = 0; i < sizeof(color_modes)/sizeof(color_modes[0]); i++)
    {
        if ((modes & color_modes[i].mode) &&
            color_modes[i].color_bits >= color_size &&
            color_modes[i].alpha_bits >= alpha_size &&
            !color_modes[i].is_float == !color_float)
        {
            if (best < 0) /* no existing best choice */
                best = i;
            else if (color_modes[i].color_bits == color_size &&
                     color_modes[i].alpha_bits == alpha_size) /* candidate is exact match */
            {
                /* prefer it over a best which isn't exact or which has a higher bpp */
                if (color_modes[best].color_bits != color_size ||
                    color_modes[best].alpha_bits != alpha_size ||
                    color_modes[i].bits_per_pixel < color_modes[best].bits_per_pixel)
                    best = i;
            }
            else if (color_modes[i].color_bits < color_modes[best].color_bits ||
                     (color_modes[i].color_bits == color_modes[best].color_bits &&
                      color_modes[i].alpha_bits < color_modes[best].alpha_bits)) /* prefer closer */
                best = i;
        }
    }

    if (best < 0)
    {
        /* Couldn't find a match.  Return first one that renderer supports. */
        for (i = 0; i < sizeof(color_modes)/sizeof(color_modes[0]); i++)
        {
            if (modes & color_modes[i].mode)
                return i;
        }
    }

    return best;
}


static unsigned int best_accum_mode(GLint modes, GLint accum_size)
{
    int best = -1;
    int i;

    for (i = 0; i < sizeof(color_modes)/sizeof(color_modes[0]); i++)
    {
        if ((modes & color_modes[i].mode) && color_modes[i].color_bits >= accum_size)
        {
            /* Prefer the fewest color bits, then prefer more alpha bits, then
               prefer more bits per pixel. */
            if (best < 0)
                best = i;
            else if (color_modes[i].color_bits < color_modes[best].color_bits)
                best = i;
            else if (color_modes[i].color_bits == color_modes[best].color_bits)
            {
                if (color_modes[i].alpha_bits > color_modes[best].alpha_bits)
                    best = i;
                else if (color_modes[i].alpha_bits == color_modes[best].alpha_bits &&
                         color_modes[i].bits_per_pixel > color_modes[best].bits_per_pixel)
                    best = i;
            }
        }
    }

    if (best < 0)
    {
        /* Couldn't find a match.  Return last one that renderer supports. */
        for (i = sizeof(color_modes)/sizeof(color_modes[0]) - 1; i >= 0; i--)
        {
            if (modes & color_modes[i].mode)
                return i;
        }
    }

    return best;
}


static void enum_renderer_pixel_formats(renderer_properties renderer, CFMutableArrayRef pixel_format_array,
                                        CFMutableSetRef pixel_format_set)
{
    CGLPixelFormatAttribute attribs[64] = {
        kCGLPFAMinimumPolicy,
        kCGLPFAClosestPolicy,
        kCGLPFARendererID, renderer.renderer_id,
        kCGLPFASingleRenderer,
    };
    int n = 5, n_stack[16], n_stack_idx = -1;
    unsigned int tried_pixel_formats = 0, failed_pixel_formats = 0, dupe_pixel_formats = 0,
                 new_pixel_formats = 0;
    pixel_format request;
    unsigned int double_buffer;
    unsigned int accelerated = renderer.accelerated;

    if (accelerated)
    {
        attribs[n++] = kCGLPFAAccelerated;
        attribs[n++] = kCGLPFANoRecovery;
    }

    n_stack[++n_stack_idx] = n;
    for (double_buffer = 0; double_buffer <= 1; double_buffer++)
    {
        unsigned int aux;

        n = n_stack[n_stack_idx];

        if ((!double_buffer && !(renderer.buffer_modes & kCGLSingleBufferBit)) ||
            (double_buffer && !(renderer.buffer_modes & kCGLDoubleBufferBit)))
            continue;

        if (double_buffer)
            attribs[n++] = kCGLPFADoubleBuffer;
        memset(&request, 0, sizeof(request));
        request.accelerated = accelerated;
        request.double_buffer = double_buffer;

        /* Don't bother with in-between aux buffers values: either 0 or max. */
        n_stack[++n_stack_idx] = n;
        for (aux = 0; aux <= renderer.max_aux_buffers; aux += renderer.max_aux_buffers)
        {
            unsigned int color_mode;

            n = n_stack[n_stack_idx];

            attribs[n++] = kCGLPFAAuxBuffers;
            attribs[n++] = aux;
            request.aux_buffers = aux;

            n_stack[++n_stack_idx] = n;
            for (color_mode = 0; color_mode < sizeof(color_modes)/sizeof(color_modes[0]); color_mode++)
            {
                unsigned int depth_mode;

                n = n_stack[n_stack_idx];

                if (!(renderer.color_modes & color_modes[color_mode].mode))
                    continue;

                attribs[n++] = kCGLPFAColorSize;
                attribs[n++] = color_modes[color_mode].color_bits;
                attribs[n++] = kCGLPFAAlphaSize;
                attribs[n++] = color_modes[color_mode].alpha_bits;
                if (color_modes[color_mode].is_float)
                    attribs[n++] = kCGLPFAColorFloat;
                request.color_mode = color_mode;

                n_stack[++n_stack_idx] = n;
                for (depth_mode = 0; depth_mode < sizeof(depth_stencil_modes)/sizeof(depth_stencil_modes[0]); depth_mode++)
                {
                    unsigned int stencil_mode;

                    n = n_stack[n_stack_idx];

                    if (!(renderer.depth_modes & depth_stencil_modes[depth_mode].mode))
                        continue;

                    attribs[n++] = kCGLPFADepthSize;
                    attribs[n++] = depth_stencil_modes[depth_mode].bits;
                    request.depth_bits = depth_stencil_modes[depth_mode].bits;

                    n_stack[++n_stack_idx] = n;
                    for (stencil_mode = 0; stencil_mode < sizeof(depth_stencil_modes)/sizeof(depth_stencil_modes[0]); stencil_mode++)
                    {
                        unsigned int stereo;

                        n = n_stack[n_stack_idx];

                        if (!(renderer.stencil_modes & depth_stencil_modes[stencil_mode].mode))
                            continue;
                        if (accelerated && depth_stencil_modes[depth_mode].bits != 24 && stencil_mode > 0)
                            continue;

                        attribs[n++] = kCGLPFAStencilSize;
                        attribs[n++] = depth_stencil_modes[stencil_mode].bits;
                        request.stencil_bits = depth_stencil_modes[stencil_mode].bits;

                        /* FIXME: Could trim search space a bit here depending on GPU.
                                  For ATI Radeon HD 4850, kCGLRGBA16161616Bit implies stereo-capable. */
                        n_stack[++n_stack_idx] = n;
                        for (stereo = 0; stereo <= 1; stereo++)
                        {
                            int accum_mode;

                            n = n_stack[n_stack_idx];

                            if ((!stereo && !(renderer.buffer_modes & kCGLMonoscopicBit)) ||
                                (stereo && !(renderer.buffer_modes & kCGLStereoscopicBit)))
                                continue;

                            if (stereo)
                                attribs[n++] = kCGLPFAStereo;
                            request.stereo = stereo;

                            /* Starts at -1 for a 0 accum size */
                            n_stack[++n_stack_idx] = n;
                            for (accum_mode = -1; accum_mode < (int)(sizeof(color_modes)/sizeof(color_modes[0])); accum_mode++)
                            {
                                unsigned int target_pass;

                                n = n_stack[n_stack_idx];

                                if (accum_mode >= 0)
                                {
                                    if (!(renderer.accum_modes & color_modes[accum_mode].mode))
                                        continue;

                                    attribs[n++] = kCGLPFAAccumSize;
                                    attribs[n++] = color_modes[accum_mode].color_bits;
                                    request.accum_mode = accum_mode + 1;
                                }
                                else
                                    request.accum_mode = 0;

                                /* Targets to request are:
                                        accelerated: window OR window + pbuffer
                                        software: window + pbuffer */
                                n_stack[++n_stack_idx] = n;
                                for (target_pass = 0; target_pass <= accelerated; target_pass++)
                                {
                                    unsigned int samples, max_samples;

                                    n = n_stack[n_stack_idx];

                                    attribs[n++] = kCGLPFAWindow;
                                    request.window = 1;

                                    if (!accelerated || target_pass > 0)
                                    {
                                        attribs[n++] = kCGLPFAPBuffer;
                                        request.pbuffer = 1;
                                    }
                                    else
                                        request.pbuffer = 0;

                                    /* FIXME: Could trim search space a bit here depending on GPU.
                                              For Nvidia GeForce 8800 GT, limited to 4 samples for color_bits >= 128.
                                              For ATI Radeon HD 4850, can't multi-sample for color_bits >= 64 or pbuffer. */
                                    n_stack[++n_stack_idx] = n;
                                    max_samples = renderer.max_sample_buffers ? max(1, renderer.max_samples) : 1;
                                    for (samples = 1; samples <= max_samples; samples *= 2)
                                    {
                                        unsigned int backing_store, min_backing_store, max_backing_store;

                                        n = n_stack[n_stack_idx];

                                        if (samples > 1)
                                        {
                                            attribs[n++] = kCGLPFASampleBuffers;
                                            attribs[n++] = renderer.max_sample_buffers;
                                            attribs[n++] = kCGLPFASamples;
                                            attribs[n++] = samples;
                                            request.sample_buffers = renderer.max_sample_buffers;
                                            request.samples = samples;
                                        }
                                        else
                                            request.sample_buffers = request.samples = 0;

                                        if (renderer.backing_store && double_buffer)
                                        {
                                            /* The software renderer seems to always preserve the backing store, whether
                                               we ask for it or not.  So don't bother not asking for it. */
                                            min_backing_store = accelerated ? 0 : 1;
                                            max_backing_store = 1;
                                        }
                                        else
                                            min_backing_store = max_backing_store = 0;
                                        n_stack[++n_stack_idx] = n;
                                        for (backing_store = min_backing_store; backing_store <= max_backing_store; backing_store++)
                                        {
                                            CGLPixelFormatObj pix;
                                            GLint virtualScreens;
                                            CGLError err;

                                            n = n_stack[n_stack_idx];

                                            if (backing_store)
                                                attribs[n++] = kCGLPFABackingStore;
                                            request.backing_store = backing_store;

                                            attribs[n] = 0;

                                            err = CGLChoosePixelFormat(attribs, &pix, &virtualScreens);
                                            if (err == kCGLNoError && pix)
                                            {
                                                pixel_format pf;
                                                GLint value, color_size, alpha_size, color_float;
                                                UInt64 pf_code;
                                                CFNumberRef code_object;
                                                BOOL dupe;

                                                memset(&pf, 0, sizeof(pf));

                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAAccelerated, &value) == kCGLNoError)
                                                    pf.accelerated = value;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAAuxBuffers, &value) == kCGLNoError)
                                                    pf.aux_buffers = value;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFADepthSize, &value) == kCGLNoError)
                                                    pf.depth_bits = value;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFADoubleBuffer, &value) == kCGLNoError)
                                                    pf.double_buffer = value;
                                                if (pf.double_buffer &&
                                                    CGLDescribePixelFormat(pix, 0, kCGLPFABackingStore, &value) == kCGLNoError)
                                                    pf.backing_store = value;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAPBuffer, &value) == kCGLNoError)
                                                    pf.pbuffer = value;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFASampleBuffers, &value) == kCGLNoError)
                                                    pf.sample_buffers = value;
                                                if (pf.sample_buffers &&
                                                    CGLDescribePixelFormat(pix, 0, kCGLPFASamples, &value) == kCGLNoError)
                                                    pf.samples = value;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAStencilSize, &value) == kCGLNoError)
                                                    pf.stencil_bits = value;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAStereo, &value) == kCGLNoError)
                                                    pf.stereo = value;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAWindow, &value) == kCGLNoError)
                                                    pf.window = value;

                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAColorSize, &color_size) != kCGLNoError)
                                                    color_size = 0;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAAlphaSize, &alpha_size) != kCGLNoError)
                                                    alpha_size = 0;
                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAColorFloat, &color_float) != kCGLNoError)
                                                    color_float = 0;
                                                pf.color_mode = best_color_mode(renderer.color_modes, color_size, alpha_size, color_float);

                                                if (CGLDescribePixelFormat(pix, 0, kCGLPFAAccumSize, &value) == kCGLNoError && value)
                                                    pf.accum_mode = best_accum_mode(renderer.accum_modes, value) + 1;

                                                CGLReleasePixelFormat(pix);

                                                pf_code = code_for_pixel_format(&pf);

                                                code_object = CFNumberCreate(NULL, kCFNumberSInt64Type, &pf_code);
                                                if ((dupe = CFSetContainsValue(pixel_format_set, code_object)))
                                                    dupe_pixel_formats++;
                                                else
                                                {
                                                    CFSetAddValue(pixel_format_set, code_object);
                                                    CFArrayAppendValue(pixel_format_array, code_object);
                                                    new_pixel_formats++;
                                                }
                                                CFRelease(code_object);

                                                if (pf_code == code_for_pixel_format(&request))
                                                    TRACE("%s%s\n", debugstr_pf(&pf), dupe ? " (duplicate)" : "");
                                                else
                                                {
                                                    TRACE("%s remapped from %s%s\n", debugstr_pf(&pf), debugstr_pf(&request),
                                                          dupe ? " (duplicate)" : "");
                                                }
                                            }
                                            else
                                            {
                                                failed_pixel_formats++;
                                                TRACE("%s failed request err %d %s\n", debugstr_pf(&request), err, err ? CGLErrorString(err) : "");
                                            }

                                            tried_pixel_formats++;
                                        }

                                        n_stack_idx--;
                                    }

                                    n_stack_idx--;
                                }

                                n_stack_idx--;
                            }

                            n_stack_idx--;
                        }

                        n_stack_idx--;
                    }

                    n_stack_idx--;
                }

                n_stack_idx--;
            }

            n_stack_idx--;
        }

        n_stack_idx--;
    }

    n_stack_idx--;

    TRACE("Number of pixel format attribute combinations: %u\n", tried_pixel_formats);
    TRACE(" Number which failed to choose a pixel format: %u\n", failed_pixel_formats);
    TRACE("   Number which chose redundant pixel formats: %u\n", dupe_pixel_formats);
    TRACE("Number of new pixel formats for this renderer: %u\n", new_pixel_formats);
}


/* The docs for WGL_ARB_pixel_format say:
    Indices are assigned to pixel formats in the following order:
    1. Accelerated pixel formats that are displayable
    2. Accelerated pixel formats that are displayable and which have
       extended attributes
    3. Generic pixel formats
    4. Accelerated pixel formats that are non displayable
 */
static int pixel_format_category(pixel_format pf)
{
    /* non-displayable */
    if (!pf.window)
        return 4;

    /* non-accelerated a.k.a. software a.k.a. generic */
    if (!pf.accelerated)
        return 3;

    /* extended attributes that can't be represented in PIXELFORMATDESCRIPTOR */
    if (color_modes[pf.color_mode].is_float)
        return 2;

    /* accelerated, displayable, no extended attributes */
    return 1;
}


static CFComparisonResult pixel_format_comparator(const void *val1, const void *val2, void *context)
{
    CFNumberRef number1 = val1;
    CFNumberRef number2 = val2;
    UInt64 code1, code2;
    pixel_format pf1, pf2;
    int category1, category2;

    CFNumberGetValue(number1, kCFNumberLongLongType, &code1);
    CFNumberGetValue(number2, kCFNumberLongLongType, &code2);
    pf1 = pixel_format_for_code(code1);
    pf2 = pixel_format_for_code(code2);
    category1 = pixel_format_category(pf1);
    category2 = pixel_format_category(pf2);

    if (category1 < category2)
        return kCFCompareLessThan;
    if (category1 > category2)
        return kCFCompareGreaterThan;

    /* Within a category, sort the "best" formats toward the front since that's
       what wglChoosePixelFormatARB() has to do.  The ordering implemented here
       matches at least one Windows 7 machine's behavior.
     */
    /* Accelerated before unaccelerated. */
    if (pf1.accelerated && !pf2.accelerated)
        return kCFCompareLessThan;
    if (!pf1.accelerated && pf2.accelerated)
        return kCFCompareGreaterThan;

    /* Explicit color mode ordering. */
    if (color_modes[pf1.color_mode].color_ordering < color_modes[pf2.color_mode].color_ordering)
        return kCFCompareLessThan;
    if (color_modes[pf1.color_mode].color_ordering > color_modes[pf2.color_mode].color_ordering)
        return kCFCompareGreaterThan;

    /* Non-pbuffer-capable before pbuffer-capable. */
    if (!pf1.pbuffer && pf2.pbuffer)
        return kCFCompareLessThan;
    if (pf1.pbuffer && !pf2.pbuffer)
        return kCFCompareGreaterThan;

    /* Fewer samples before more samples. */
    if (pf1.samples < pf2.samples)
        return kCFCompareLessThan;
    if (pf1.samples > pf2.samples)
        return kCFCompareGreaterThan;

    /* Monoscopic before stereoscopic.  (This is a guess.) */
    if (!pf1.stereo && pf2.stereo)
        return kCFCompareLessThan;
    if (pf1.stereo && !pf2.stereo)
        return kCFCompareGreaterThan;

    /* Single buffered before double buffered. */
    if (!pf1.double_buffer && pf2.double_buffer)
        return kCFCompareLessThan;
    if (pf1.double_buffer && !pf2.double_buffer)
        return kCFCompareGreaterThan;

    /* Possibly-optimized double buffering before backing-store-preserving
       double buffering. */
    if (!pf1.backing_store && pf2.backing_store)
        return kCFCompareLessThan;
    if (pf1.backing_store && !pf2.backing_store)
        return kCFCompareGreaterThan;

    /* Bigger depth buffer before smaller depth buffer. */
    if (pf1.depth_bits > pf2.depth_bits)
        return kCFCompareLessThan;
    if (pf1.depth_bits < pf2.depth_bits)
        return kCFCompareGreaterThan;

    /* Smaller stencil buffer before bigger stencil buffer. */
    if (pf1.stencil_bits < pf2.stencil_bits)
        return kCFCompareLessThan;
    if (pf1.stencil_bits > pf2.stencil_bits)
        return kCFCompareGreaterThan;

    /* Smaller alpha bits before larger alpha bits. */
    if (color_modes[pf1.color_mode].alpha_bits < color_modes[pf2.color_mode].alpha_bits)
        return kCFCompareLessThan;
    if (color_modes[pf1.color_mode].alpha_bits > color_modes[pf2.color_mode].alpha_bits)
        return kCFCompareGreaterThan;

    /* Smaller accum buffer before larger accum buffer.  (This is a guess.) */
    if (pf1.accum_mode)
    {
        if (pf2.accum_mode)
        {
            if (color_modes[pf1.accum_mode - 1].color_bits - color_modes[pf1.accum_mode - 1].alpha_bits <
                color_modes[pf2.accum_mode - 1].color_bits - color_modes[pf2.accum_mode - 1].alpha_bits)
                return kCFCompareLessThan;
            if (color_modes[pf1.accum_mode - 1].color_bits - color_modes[pf1.accum_mode - 1].alpha_bits >
                color_modes[pf2.accum_mode - 1].color_bits - color_modes[pf2.accum_mode - 1].alpha_bits)
                return kCFCompareGreaterThan;

            if (color_modes[pf1.accum_mode - 1].bits_per_pixel < color_modes[pf2.accum_mode - 1].bits_per_pixel)
                return kCFCompareLessThan;
            if (color_modes[pf1.accum_mode - 1].bits_per_pixel > color_modes[pf2.accum_mode - 1].bits_per_pixel)
                return kCFCompareGreaterThan;

            if (color_modes[pf1.accum_mode - 1].alpha_bits < color_modes[pf2.accum_mode - 1].alpha_bits)
                return kCFCompareLessThan;
            if (color_modes[pf1.accum_mode - 1].alpha_bits > color_modes[pf2.accum_mode - 1].alpha_bits)
                return kCFCompareGreaterThan;
        }
        else
            return kCFCompareGreaterThan;
    }
    else if (pf2.accum_mode)
        return kCFCompareLessThan;

    /* Fewer auxiliary buffers before more auxiliary buffers.  (This is a guess.) */
    if (pf1.aux_buffers < pf2.aux_buffers)
        return kCFCompareLessThan;
    if (pf1.aux_buffers > pf2.aux_buffers)
        return kCFCompareGreaterThan;

    /* If we get here, arbitrarily sort based on code. */
    if (code1 < code2)
        return kCFCompareLessThan;
    if (code1 > code2)
        return kCFCompareGreaterThan;
    return kCFCompareEqualTo;
}


static BOOL init_pixel_formats(void)
{
    BOOL ret = FALSE;
    CGLRendererInfoObj renderer_info;
    GLint rendererCount;
    CGLError err;
    CFMutableSetRef pixel_format_set;
    CFMutableArrayRef pixel_format_array;
    int i;
    CFRange range;

    TRACE("()\n");

    err = CGLQueryRendererInfo(CGDisplayIDToOpenGLDisplayMask(CGMainDisplayID()), &renderer_info, &rendererCount);
    if (err)
    {
        WARN("CGLQueryRendererInfo failed (%d) %s\n", err, CGLErrorString(err));
        return FALSE;
    }

    pixel_format_set = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
    if (!pixel_format_set)
    {
        WARN("CFSetCreateMutable failed\n");
        CGLDestroyRendererInfo(renderer_info);
        return FALSE;
    }

    pixel_format_array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!pixel_format_array)
    {
        WARN("CFArrayCreateMutable failed\n");
        CFRelease(pixel_format_set);
        CGLDestroyRendererInfo(renderer_info);
        return FALSE;
    }

    for (i = 0; i < rendererCount; i++)
    {
        renderer_properties renderer;

        get_renderer_properties(renderer_info, i, &renderer);
        if (TRACE_ON(wgl))
        {
            TRACE("renderer_properties %d:\n", i);
            dump_renderer(&renderer);
        }

        enum_renderer_pixel_formats(renderer, pixel_format_array, pixel_format_set);
    }

    CFRelease(pixel_format_set);
    CGLDestroyRendererInfo(renderer_info);

    range = CFRangeMake(0, CFArrayGetCount(pixel_format_array));
    if (range.length)
    {
        pixel_formats = HeapAlloc(GetProcessHeap(), 0, range.length * sizeof(*pixel_formats));
        if (pixel_formats)
        {
            CFArraySortValues(pixel_format_array, range, pixel_format_comparator, NULL);
            for (i = 0; i < range.length; i++)
            {
                CFNumberRef number = CFArrayGetValueAtIndex(pixel_format_array, i);
                UInt64 code;

                CFNumberGetValue(number, kCFNumberLongLongType, &code);
                pixel_formats[i] = pixel_format_for_code(code);
                if (pixel_formats[i].window)
                    nb_displayable_formats++;
            }

            nb_formats = range.length;
            TRACE("Total number of unique pixel formats: %d\n", nb_formats);
            ret = TRUE;
        }
        else
            WARN("failed to allocate pixel format list\n");
    }
    else
        WARN("got no pixel formats\n");

    CFRelease(pixel_format_array);
    return ret;
}


static inline BOOL is_valid_pixel_format(int format)
{
    return format > 0 && format <= nb_formats;
}


static inline BOOL is_displayable_pixel_format(int format)
{
    return format > 0 && format <= nb_displayable_formats;
}


static const pixel_format *get_pixel_format(int format, BOOL allow_nondisplayable)
{
    /* Check if the pixel format is valid. Note that it is legal to pass an invalid
     * format in case of probing the number of pixel formats.
     */
    if (is_valid_pixel_format(format) && (is_displayable_pixel_format(format) || allow_nondisplayable))
    {
        TRACE("Returning format %d\n", format);
        return &pixel_formats[format - 1];
    }
    return NULL;
}


static BOOL init_gl_info(void)
{
    CGDirectDisplayID display = CGMainDisplayID();
    CGOpenGLDisplayMask displayMask = CGDisplayIDToOpenGLDisplayMask(display);
    CGLPixelFormatAttribute attribs[] = {
        kCGLPFADisplayMask, displayMask,
        0
    };
    CGLPixelFormatObj pix;
    GLint virtualScreens;
    CGLError err;
    CGLContextObj context;
    CGLContextObj old_context = CGLGetCurrentContext();
    const char *str;

    err = CGLChoosePixelFormat(attribs, &pix, &virtualScreens);
    if (err != kCGLNoError || !pix)
    {
        WARN("CGLChoosePixelFormat() failed with error %d %s\n", err, CGLErrorString(err));
        return FALSE;
    }

    err = CGLCreateContext(pix, NULL, &context);
    CGLReleasePixelFormat(pix);
    if (err != kCGLNoError || !context)
    {
        WARN("CGLCreateContext() failed with error %d %s\n", err, CGLErrorString(err));
        return FALSE;
    }

    err = CGLSetCurrentContext(context);
    if (err != kCGLNoError)
    {
        WARN("CGLSetCurrentContext() failed with error %d %s\n", err, CGLErrorString(err));
        CGLReleaseContext(context);
        return FALSE;
    }

    str = (const char*)opengl_funcs.gl.p_glGetString(GL_VERSION);
    gl_info.glVersion = HeapAlloc(GetProcessHeap(), 0, strlen(str) + 1);
    strcpy(gl_info.glVersion, str);
    str = (const char*)opengl_funcs.gl.p_glGetString(GL_EXTENSIONS);
    gl_info.glExtensions = HeapAlloc(GetProcessHeap(), 0, strlen(str) + 1);
    strcpy(gl_info.glExtensions, str);

    opengl_funcs.gl.p_glGetIntegerv(GL_MAX_VIEWPORT_DIMS, gl_info.max_viewport_dims);

    TRACE("GL version   : %s\n", gl_info.glVersion);
    TRACE("GL renderer  : %s\n", opengl_funcs.gl.p_glGetString(GL_RENDERER));

    CGLSetCurrentContext(old_context);
    CGLReleaseContext(context);

    return TRUE;
}


static BOOL get_gl_view_window_rect(struct macdrv_win_data *data, macdrv_window *window, RECT *rect)
{
    BOOL ret = TRUE;
    *rect = data->client_rect;

    if (data->cocoa_window)
    {
        if (window)
            *window = data->cocoa_window;
        OffsetRect(rect, -data->whole_rect.left, -data->whole_rect.top);
    }
    else
    {
        HWND top = GetAncestor(data->hwnd, GA_ROOT);
        HWND parent = GetAncestor(data->hwnd, GA_PARENT);
        struct macdrv_win_data *top_data = get_win_data(top);

        if (top_data && top_data->cocoa_window)
        {
            if (window)
                *window = top_data->cocoa_window;
            MapWindowPoints(parent, 0, (POINT*)rect, 2);
            OffsetRect(rect, -top_data->whole_rect.left, -top_data->whole_rect.top);
        }
        else
            ret = FALSE;

        release_win_data(top_data);
    }

    return ret;
}


/***********************************************************************
 *              set_win_format
 */
static BOOL set_win_format(struct macdrv_win_data *data, int format)
{
    macdrv_window cocoa_window;

    TRACE("hwnd %p format %d\n", data->hwnd, format);

    if (!get_gl_view_window_rect(data, &cocoa_window, &data->gl_rect))
    {
        ERR("no top-level parent with Cocoa window in this process\n");
        return FALSE;
    }

    if (data->gl_view) macdrv_dispose_view(data->gl_view);
    data->gl_view = macdrv_create_view(cocoa_window, cgrect_from_rect(data->gl_rect));

    if (!data->gl_view)
    {
        WARN("failed to create GL view for window %p rect %s\n", cocoa_window, wine_dbgstr_rect(&data->gl_rect));
        return FALSE;
    }

    TRACE("created GL view %p in window %p at %s\n", data->gl_view, cocoa_window,
          wine_dbgstr_rect(&data->gl_rect));

    data->pixel_format = format;

    return TRUE;
}


/**********************************************************************
 *              set_pixel_format
 *
 * Implementation of wglSetPixelFormat and wglSetPixelFormatWINE.
 */
static BOOL set_pixel_format(HDC hdc, int fmt, BOOL allow_reset)
{
    struct macdrv_win_data *data;
    const pixel_format *pf;
    HWND hwnd = WindowFromDC(hdc);
    BOOL ret = FALSE;

    TRACE("hdc %p format %d\n", hdc, fmt);

    if (!hwnd || hwnd == GetDesktopWindow())
    {
        WARN("not a proper window DC %p/%p\n", hdc, hwnd);
        return FALSE;
    }

    if (!(data = get_win_data(hwnd)))
    {
        FIXME("DC for window %p of other process: not implemented\n", hwnd);
        return FALSE;
    }

    if (!allow_reset && data->pixel_format)  /* cannot change it if already set */
    {
        ret = (data->pixel_format == fmt);
        goto done;
    }

    /* Check if fmt is in our list of supported formats to see if it is supported. */
    pf = get_pixel_format(fmt, FALSE /* non-displayable */);
    if (!pf)
    {
        ERR("Invalid pixel format: %d\n", fmt);
        goto done;
    }

    if (!pf->window)
    {
        WARN("Pixel format %d is not compatible for window rendering\n", fmt);
        goto done;
    }

    if (!set_win_format(data, fmt))
    {
        WARN("Couldn't set format of the window, returning failure\n");
        goto done;
    }

    TRACE("pixel format:\n");
    TRACE("           window: %u\n", (unsigned int)pf->window);
    TRACE("          pBuffer: %u\n", (unsigned int)pf->pbuffer);
    TRACE("      accelerated: %u\n", (unsigned int)pf->accelerated);
    TRACE("       color bits: %u%s\n", (unsigned int)color_modes[pf->color_mode].color_bits, (color_modes[pf->color_mode].is_float ? " float" : ""));
    TRACE("       alpha bits: %u\n", (unsigned int)color_modes[pf->color_mode].alpha_bits);
    TRACE("      aux buffers: %u\n", (unsigned int)pf->aux_buffers);
    TRACE("       depth bits: %u\n", (unsigned int)pf->depth_bits);
    TRACE("     stencil bits: %u\n", (unsigned int)pf->stencil_bits);
    TRACE("       accum bits: %u\n", (unsigned int)pf->accum_mode ? color_modes[pf->accum_mode - 1].color_bits : 0);
    TRACE("    double_buffer: %u\n", (unsigned int)pf->double_buffer);
    TRACE("           stereo: %u\n", (unsigned int)pf->stereo);
    TRACE("   sample_buffers: %u\n", (unsigned int)pf->sample_buffers);
    TRACE("          samples: %u\n", (unsigned int)pf->samples);
    TRACE("    backing_store: %u\n", (unsigned int)pf->backing_store);
    ret = TRUE;

done:
    release_win_data(data);
    if (ret) __wine_set_pixel_format(hwnd, fmt);
    return ret;
}


/**********************************************************************
 *              set_gl_view_parent
 */
void set_gl_view_parent(HWND hwnd, HWND parent)
{
    struct macdrv_win_data *data;

    if (!(data = get_win_data(hwnd))) return;

    if (data->gl_view)
    {
        macdrv_window cocoa_window;

        TRACE("moving GL view %p to parent %p\n", data->gl_view, parent);

        if (!get_gl_view_window_rect(data, &cocoa_window, &data->gl_rect))
        {
            ERR("no top-level parent with Cocoa window in this process\n");
            macdrv_dispose_view(data->gl_view);
            data->gl_view = NULL;
            release_win_data(data);
            __wine_set_pixel_format( hwnd, 0 );
            return;
        }

        macdrv_set_view_window_and_frame(data->gl_view, cocoa_window, cgrect_from_rect(data->gl_rect));
    }

    release_win_data(data);
}


/**********************************************************************
 *              make_context_current
 */
static void make_context_current(struct wgl_context *context, BOOL read)
{
    macdrv_view view;
    struct wgl_pbuffer *pbuffer;

    if (read)
    {
        view = context->read_view;
        pbuffer = context->read_pbuffer;
    }
    else
    {
        view = context->draw_view;
        pbuffer = context->draw_pbuffer;
    }

    if (view || !pbuffer)
        macdrv_make_context_current(context->context, view);
    else
    {
        CGLSetPBuffer(context->cglcontext, pbuffer->pbuffer, pbuffer->face,
                      pbuffer->level, 0);
        CGLSetCurrentContext(context->cglcontext);
    }
}


/**********************************************************************
 *              macdrv_glCopyColorTable
 *
 * Hook into glCopyColorTable as part of the implementation of
 * wglMakeContextCurrentARB.  If the context has a separate readable,
 * temporarily make that current, do glCopyColorTable, and then set it
 * back to the drawable.  This is modeled after what Mesa GLX's Apple
 * implementation does.
 */
static void macdrv_glCopyColorTable(GLenum target, GLenum internalformat, GLint x, GLint y,
                                    GLsizei width)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;

    if (context->read_view || context->read_pbuffer)
        make_context_current(context, TRUE);

    pglCopyColorTable(target, internalformat, x, y, width);

    if (context->read_view || context->read_pbuffer)
        make_context_current(context, FALSE);
}


/**********************************************************************
 *              macdrv_glCopyPixels
 *
 * Hook into glCopyPixels as part of the implementation of
 * wglMakeContextCurrentARB.  If the context has a separate readable,
 * temporarily make that current, do glCopyPixels, and then set it back
 * to the drawable.  This is modeled after what Mesa GLX's Apple
 * implementation does.
 */
static void macdrv_glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;

    if (context->read_view || context->read_pbuffer)
        make_context_current(context, TRUE);

    pglCopyPixels(x, y, width, height, type);

    if (context->read_view || context->read_pbuffer)
        make_context_current(context, FALSE);
}


static void macdrv_glFlush(void)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;
    const pixel_format *pf = &pixel_formats[context->format - 1];
    DWORD now = GetTickCount();

    TRACE("double buffer %d last flush time %d now %d\n", (int)pf->double_buffer,
          context->last_flush_time, now);
    if (pglFlushRenderAPPLE && !pf->double_buffer && (now - context->last_flush_time) < 17)
    {
        TRACE("calling glFlushRenderAPPLE()\n");
        pglFlushRenderAPPLE();
    }
    else
    {
        TRACE("calling glFlush()\n");
        pglFlush();
        context->last_flush_time = now;
    }
}


/**********************************************************************
 *              macdrv_glReadPixels
 *
 * Hook into glReadPixels as part of the implementation of
 * wglMakeContextCurrentARB.  If the context has a separate readable,
 * temporarily make that current, do glReadPixels, and then set it back
 * to the drawable.  This is modeled after what Mesa GLX's Apple
 * implementation does.
 */
static void macdrv_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                GLenum format, GLenum type, void *pixels)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;

    if (context->read_view || context->read_pbuffer)
        make_context_current(context, TRUE);

    pglReadPixels(x, y, width, height, format, type, pixels);

    if (context->read_view || context->read_pbuffer)
        make_context_current(context, FALSE);
}


/**********************************************************************
 *              macdrv_glViewport
 *
 * Hook into glViewport as an opportunity to update the OpenGL context
 * if necessary.  This is modeled after what Mesa GLX's Apple
 * implementation does.
 */
static void macdrv_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;

    macdrv_update_opengl_context(context->context);
    pglViewport(x, y, width, height);
}


/***********************************************************************
 *              macdrv_wglBindTexImageARB
 *
 * WGL_ARB_render_texture: wglBindTexImageARB
 */
static BOOL macdrv_wglBindTexImageARB(struct wgl_pbuffer *pbuffer, int iBuffer)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;
    GLenum source;
    CGLError err;

    TRACE("pbuffer %p iBuffer 0x%x\n", pbuffer, iBuffer);

    if (pbuffer->no_texture)
    {
        SetLastError(ERROR_INVALID_OPERATION);
        return GL_FALSE;
    }

    if (!context->draw_view && context->draw_pbuffer == pbuffer)
        opengl_funcs.gl.p_glFlush();

    switch (iBuffer)
    {
        case WGL_FRONT_LEFT_ARB:
            if (pixel_formats[pbuffer->format - 1].stereo)
                source = GL_FRONT_LEFT;
            else
                source = GL_FRONT;
            break;
        case WGL_FRONT_RIGHT_ARB:
            source = GL_FRONT_RIGHT;
            break;
        case WGL_BACK_LEFT_ARB:
            if (pixel_formats[pbuffer->format - 1].stereo)
                source = GL_BACK_LEFT;
            else
                source = GL_BACK;
            break;
        case WGL_BACK_RIGHT_ARB:
            source = GL_BACK_RIGHT;
            break;
        case WGL_AUX0_ARB: source = GL_AUX0; break;
        case WGL_AUX1_ARB: source = GL_AUX1; break;
        case WGL_AUX2_ARB: source = GL_AUX2; break;
        case WGL_AUX3_ARB: source = GL_AUX3; break;

        case WGL_AUX4_ARB:
        case WGL_AUX5_ARB:
        case WGL_AUX6_ARB:
        case WGL_AUX7_ARB:
        case WGL_AUX8_ARB:
        case WGL_AUX9_ARB:
            FIXME("unsupported source buffer 0x%x\n", iBuffer);
            SetLastError(ERROR_INVALID_DATA);
            return GL_FALSE;

        default:
            WARN("unknown source buffer 0x%x\n", iBuffer);
            SetLastError(ERROR_INVALID_DATA);
            return GL_FALSE;
    }

    err = CGLTexImagePBuffer(context->cglcontext, pbuffer->pbuffer, source);
    if (err != kCGLNoError)
    {
        WARN("CGLTexImagePBuffer failed with err %d %s\n", err, CGLErrorString(err));
        SetLastError(ERROR_INVALID_OPERATION);
        return GL_FALSE;
    }

    return GL_TRUE;
}


/***********************************************************************
 *              macdrv_wglChoosePixelFormatARB
 *
 * WGL_ARB_pixel_format: wglChoosePixelFormatARB
 */
static BOOL macdrv_wglChoosePixelFormatARB(HDC hdc, const int *piAttribIList,
                                           const FLOAT *pfAttribFList, UINT nMaxFormats,
                                           int *piFormats, UINT *nNumFormats)
{
    pixel_format pf, valid;
    const int *iptr;
    int color_bits, red_bits, green_bits, blue_bits, alpha_bits;
    int accum_bits, accum_red_bits, accum_green_bits, accum_blue_bits, accum_alpha_bits;
    int float_color;
    BOOL srgb;
    int i, found = 0;

    TRACE("hdc %p piAttribIList %p pfAttribFList %p nMaxFormats %u piFormats %p nNumFormats %p\n",
          hdc, piAttribIList, pfAttribFList, nMaxFormats, piFormats, nNumFormats);
    if (pfAttribFList)
        FIXME("unused pfAttribFList\n");

    memset(&pf, 0, sizeof(pf));
    memset(&valid, 0, sizeof(valid));
    color_bits = red_bits = green_bits = blue_bits = alpha_bits = 0;
    accum_bits = accum_red_bits = accum_green_bits = accum_blue_bits = accum_alpha_bits = 0;
    float_color = -1;
    srgb = FALSE;

    for (iptr = piAttribIList; iptr && *iptr; iptr += 2)
    {
        int attr = iptr[0];
        int value = iptr[1];

        TRACE("%s\n", debugstr_attrib(attr, value));

        switch (attr)
        {
            case WGL_DRAW_TO_WINDOW_ARB:
                if (valid.window && (!pf.window != !value)) goto cant_match;
                pf.window = (value != 0);
                valid.window = 1;
                break;

            case WGL_DRAW_TO_BITMAP_ARB:
                goto cant_match;

            case WGL_ACCELERATION_ARB:
                if (value == WGL_FULL_ACCELERATION_ARB)
                    value = 1;
                else if (value == WGL_NO_ACCELERATION_ARB)
                    value = 0;
                else
                    goto cant_match;
                if (valid.accelerated && pf.accelerated != value) goto cant_match;
                pf.accelerated = value;
                valid.accelerated = 1;
                break;

            case WGL_NEED_PALETTE_ARB:
            case WGL_NEED_SYSTEM_PALETTE_ARB:
            case WGL_SWAP_LAYER_BUFFERS_ARB:
                if (value) goto cant_match;
                break;

            case WGL_SWAP_METHOD_ARB:
                if (value == WGL_SWAP_COPY_ARB)
                    value = 1;
                else if (value == WGL_SWAP_UNDEFINED_ARB)
                    value = 0;
                else
                    goto cant_match;
                if (valid.backing_store && pf.backing_store != value) goto cant_match;
                if (valid.double_buffer && !pf.double_buffer && value) goto cant_match;
                pf.backing_store = value;
                valid.backing_store = 1;
                break;

            case WGL_NUMBER_OVERLAYS_ARB:
            case WGL_NUMBER_UNDERLAYS_ARB:
                if (value) goto cant_match;
                break;

            case WGL_SHARE_DEPTH_ARB:
            case WGL_SHARE_STENCIL_ARB:
            case WGL_SHARE_ACCUM_ARB:
                /* no effect */
                break;

            case WGL_SUPPORT_GDI_ARB:
                if (value) goto cant_match;
                break;

            case WGL_SUPPORT_OPENGL_ARB:
                if (!value) goto cant_match;
                break;

            case WGL_DOUBLE_BUFFER_ARB:
                if (valid.double_buffer && (!pf.double_buffer != !value)) goto cant_match;
                pf.double_buffer = (value != 0);
                valid.double_buffer = 1;
                if (valid.backing_store && pf.backing_store && !pf.double_buffer) goto cant_match;
                break;

            case WGL_STEREO_ARB:
                if (valid.stereo && (!pf.stereo != !value)) goto cant_match;
                pf.stereo = (value != 0);
                valid.stereo = 1;
                break;

            case WGL_PIXEL_TYPE_ARB:
                if (value == WGL_TYPE_RGBA_FLOAT_ARB)
                    value = 1;
                else if (value == WGL_TYPE_RGBA_ARB)
                    value = 0;
                else
                {
                    /* Mac contexts don't support rendering to unsigned floating
                       point formats, even if GL_EXT_packed_float is supported.
                       So, WGL_TYPE_RGBA_UNSIGNED_FLOAT_EXT is not supported. */
                    goto cant_match;
                }
                if (float_color != -1 && float_color != value) goto cant_match;
                if (srgb && value) goto cant_match;
                float_color = value;
                break;

            case WGL_COLOR_BITS_ARB:
                if (color_bits < value) color_bits = value;
                break;

            case WGL_RED_BITS_ARB:
                if (srgb && value > 8) goto cant_match;
                if (red_bits < value) red_bits = value;
                break;

            case WGL_GREEN_BITS_ARB:
                if (srgb && value > 8) goto cant_match;
                if (green_bits < value) green_bits = value;
                break;

            case WGL_BLUE_BITS_ARB:
                if (srgb && value > 8) goto cant_match;
                if (blue_bits < value) blue_bits = value;
                break;

            case WGL_ALPHA_BITS_ARB:
                if (alpha_bits < value) alpha_bits = value;
                break;

            case WGL_ACCUM_BITS_ARB:
                if (accum_bits < value) accum_bits = value;
                break;

            case WGL_ACCUM_RED_BITS_ARB:
                if (accum_red_bits < value) accum_red_bits = value;
                break;

            case WGL_ACCUM_GREEN_BITS_ARB:
                if (accum_green_bits < value) accum_green_bits = value;
                break;

            case WGL_ACCUM_BLUE_BITS_ARB:
                if (accum_blue_bits < value) accum_blue_bits = value;
                break;

            case WGL_ACCUM_ALPHA_BITS_ARB:
                if (accum_alpha_bits < value) accum_alpha_bits = value;
                break;

            case WGL_DEPTH_BITS_ARB:
                if (value > 255) goto cant_match;
                if (pf.depth_bits < value) pf.depth_bits = value;
                break;

            case WGL_STENCIL_BITS_ARB:
                if (value > 255) goto cant_match;
                if (pf.stencil_bits < value) pf.stencil_bits = value;
                break;

            case WGL_AUX_BUFFERS_ARB:
                if (value > 7) goto cant_match;
                if (pf.aux_buffers < value) pf.aux_buffers = value;
                break;

            case WGL_SAMPLE_BUFFERS_ARB:
                if (value > 1) goto cant_match;
                if (pf.sample_buffers < value) pf.sample_buffers = value;
                break;

            case WGL_SAMPLES_ARB:
                if (value > 31) goto cant_match;
                if (pf.samples < value) pf.samples = value;
                break;

            case WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB: /* a.k.a. WGL_FRAMEBUFFER_SRGB_CAPABLE_EXT */
                /* sRGB is only supported for 8-bit integer color components */
                if (float_color >= 1 || red_bits > 8 || green_bits > 8 || blue_bits > 8)
                    goto cant_match;
                srgb = TRUE;
                break;

            case WGL_NUMBER_PIXEL_FORMATS_ARB:
            case WGL_RED_SHIFT_ARB:
            case WGL_GREEN_SHIFT_ARB:
            case WGL_BLUE_SHIFT_ARB:
            case WGL_ALPHA_SHIFT_ARB:
            case WGL_TRANSPARENT_ARB:
            case WGL_TRANSPARENT_RED_VALUE_ARB:
            case WGL_TRANSPARENT_GREEN_VALUE_ARB:
            case WGL_TRANSPARENT_BLUE_VALUE_ARB:
            case WGL_TRANSPARENT_ALPHA_VALUE_ARB:
            case WGL_TRANSPARENT_INDEX_VALUE_ARB:
                /* ignored */
                break;

            case WGL_DRAW_TO_PBUFFER_ARB:
            case WGL_BIND_TO_TEXTURE_RGB_ARB:
            case WGL_BIND_TO_TEXTURE_RGBA_ARB:
            case WGL_BIND_TO_TEXTURE_RECTANGLE_RGB_NV:
            case WGL_BIND_TO_TEXTURE_RECTANGLE_RGBA_NV:
                if (valid.pbuffer && (!pf.pbuffer != !value)) goto cant_match;
                pf.pbuffer = (value != 0);
                valid.pbuffer = 1;
                if ((attr == WGL_BIND_TO_TEXTURE_RGBA_ARB || attr == WGL_BIND_TO_TEXTURE_RECTANGLE_RGBA_NV) &&
                    !alpha_bits)
                    alpha_bits = 1;
                break;

            default:
                WARN("invalid attribute %s\n", debugstr_attrib(attr, value));
                return GL_FALSE;
        }
    }

    TRACE("required: w/p/a %s/%s/%s col/r/g/b/a %d%s/%d/%d/%d/%d srgb %d ac %d/%d/%d/%d/%d dp/stn/ax/b/db/str %u/%u/%u/%s/%s/%s samp %u/%u\n",
          valid.window ? (pf.window ? "1" : "0") : "?",
          valid.pbuffer ? (pf.pbuffer ? "1" : "0") : "?",
          valid.accelerated ? (pf.accelerated ? "1" : "0") : "?",
          color_bits,
          float_color == -1 ? "?" : float_color ? "f" : "",
          red_bits,
          green_bits,
          blue_bits,
          alpha_bits,
          (int)srgb,
          accum_bits,
          accum_red_bits,
          accum_green_bits,
          accum_blue_bits,
          accum_alpha_bits,
          pf.depth_bits,
          pf.stencil_bits,
          pf.aux_buffers,
          valid.backing_store ? (pf.backing_store ? "1" : "0") : "?",
          valid.double_buffer ? (pf.double_buffer ? "1" : "0") : "?",
          valid.stereo ? (pf.stereo ? "1" : "0") : "?",
          pf.sample_buffers,
          pf.samples);

    for (i = 0; i < nb_formats && found < nMaxFormats; i++)
    {
        const struct color_mode *mode;

        if (valid.window && pixel_formats[i].window != pf.window) continue;
        if (valid.pbuffer && pixel_formats[i].pbuffer != pf.pbuffer) continue;
        if (valid.accelerated && pixel_formats[i].accelerated != pf.accelerated) continue;
        if (valid.double_buffer && pixel_formats[i].double_buffer != pf.double_buffer) continue;
        if (valid.stereo && pixel_formats[i].stereo != pf.stereo) continue;
        if (valid.backing_store && pixel_formats[i].backing_store != pf.backing_store) continue;

        if (pixel_formats[i].aux_buffers < pf.aux_buffers) continue;
        if (pixel_formats[i].depth_bits < pf.depth_bits) continue;
        if (pixel_formats[i].stencil_bits < pf.stencil_bits) continue;
        if (pixel_formats[i].sample_buffers < pf.sample_buffers) continue;
        if (pixel_formats[i].samples < pf.samples) continue;

        mode = &color_modes[pixel_formats[i].color_mode];
        /* If the mode doesn't have alpha, check requested color bits against
           bits per pixel instead of the mode's color bits.  On Windows, color
           bits sometimes exceeds r+g+b (e.g. it's 32 for an R8G8B8A0 pixel format).
           If an app depends on that and requests WGL_COLOR_BITS_ARB == 32 and
           expects that to match such a pixel format, we need to accommodate that. */
        if (mode->alpha_bits)
        {
            if (mode->color_bits < color_bits)
                continue;
        }
        else
        {
            if (mode->bits_per_pixel < color_bits)
                continue;
        }
        if (mode->red_bits < red_bits || mode->green_bits < green_bits ||
            mode->blue_bits < blue_bits || mode->alpha_bits < alpha_bits)
            continue;
        if (float_color != -1 && (!mode->is_float != !float_color)) continue;
        if (srgb && (mode->red_bits != 8 || mode->green_bits != 8 || mode->blue_bits != 8 || mode->is_float))
            continue;

        if (pixel_formats[i].accum_mode)
        {
            mode = &color_modes[pixel_formats[i].accum_mode - 1];
            if (mode->color_bits < accum_bits || mode->red_bits < accum_red_bits ||
                mode->green_bits < accum_green_bits || mode->blue_bits < accum_blue_bits ||
                mode->alpha_bits < accum_alpha_bits)
                continue;
        }
        else if (accum_bits || accum_red_bits || accum_green_bits || accum_blue_bits || accum_alpha_bits)
            continue;

        piFormats[found++] = i + 1;
        TRACE("match: pixel format %d %s\n", i + 1, debugstr_pf(&pixel_formats[i]));
    }

cant_match:
    *nNumFormats = found;

    return TRUE;
}


/**********************************************************************
 *              macdrv_wglCreatePbufferARB
 *
 * WGL_ARB_pbuffer: wglCreatePbufferARB
 */
static struct wgl_pbuffer *macdrv_wglCreatePbufferARB(HDC hdc, int iPixelFormat, int iWidth, int iHeight,
                                                      const int *piAttribList)
{
    struct wgl_pbuffer* pbuffer;
    GLenum target = 0;
    GLenum internalFormat = 0;
    CGLError err;

    TRACE("hdc %p iPixelFormat %d iWidth %d iHeight %d piAttribList %p\n",
          hdc, iPixelFormat, iWidth, iHeight, piAttribList);

    if (!is_valid_pixel_format(iPixelFormat) || !pixel_formats[iPixelFormat - 1].pbuffer)
    {
        WARN("invalid pixel format %d\n", iPixelFormat);
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return NULL;
    }

    pbuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pbuffer));
    pbuffer->format = iPixelFormat;

    for ( ; piAttribList && *piAttribList; piAttribList += 2)
    {
        int attr = piAttribList[0];
        int value = piAttribList[1];

        switch (attr)
        {
            case WGL_PBUFFER_LARGEST_ARB:
                FIXME("WGL_PBUFFER_LARGEST_ARB: %d; ignoring\n", value);
                break;

            case WGL_TEXTURE_FORMAT_ARB:
                switch (value)
                {
                    case WGL_TEXTURE_RGBA_ARB:
                        TRACE("WGL_TEXTURE_FORMAT_ARB: WGL_TEXTURE_RGBA_ARB\n");
                        internalFormat = GL_RGBA;
                        break;
                    case WGL_TEXTURE_RGB_ARB:
                        TRACE("WGL_TEXTURE_FORMAT_ARB: WGL_TEXTURE_RGB_ARB\n");
                        internalFormat = GL_RGB;
                        break;
                    case WGL_NO_TEXTURE_ARB:
                        TRACE("WGL_TEXTURE_FORMAT_ARB: WGL_NO_TEXTURE_ARB\n");
                        internalFormat = 0;
                        break;
                    default:
                        WARN("unknown WGL_TEXTURE_FORMAT_ARB value 0x%x\n", value);
                        SetLastError(ERROR_INVALID_DATA);
                        goto done;
                }
                break;

            case WGL_TEXTURE_TARGET_ARB:
                pbuffer->face = 0;
                switch (value)
                {
                    case WGL_NO_TEXTURE_ARB:
                        TRACE("WGL_TEXTURE_TARGET_ARB: WGL_NO_TEXTURE_ARB\n");
                        target = 0;
                        break;
                    case WGL_TEXTURE_CUBE_MAP_ARB:
                        TRACE("WGL_TEXTURE_TARGET_ARB: WGL_TEXTURE_CUBE_MAP_ARB\n");
                        target = GL_TEXTURE_CUBE_MAP;
                        pbuffer->face = GL_TEXTURE_CUBE_MAP_POSITIVE_X;
                        break;
                    case WGL_TEXTURE_1D_ARB:
                        FIXME("WGL_TEXTURE_TARGET_ARB: WGL_TEXTURE_1D_ARB; not supported\n");
                        SetLastError(ERROR_NO_SYSTEM_RESOURCES);
                        goto done;
                    case WGL_TEXTURE_2D_ARB:
                        TRACE("WGL_TEXTURE_TARGET_ARB: WGL_TEXTURE_2D_ARB\n");
                        target = GL_TEXTURE_2D;
                        break;
                    case WGL_TEXTURE_RECTANGLE_NV:
                        TRACE("WGL_TEXTURE_TARGET_ARB: WGL_TEXTURE_RECTANGLE_NV\n");
                        target = GL_TEXTURE_RECTANGLE;
                        break;
                    default:
                        WARN("unknown WGL_TEXTURE_TARGET_ARB value 0x%x\n", value);
                        SetLastError(ERROR_INVALID_DATA);
                        goto done;
                }
                break;

            case WGL_MIPMAP_TEXTURE_ARB:
                TRACE("WGL_MIPMAP_TEXTURE_ARB: %d\n", value);
                pbuffer->max_level = 0;
                if (value)
                {
                    int size = min(iWidth, iHeight) / 2;
                    while (size)
                    {
                        pbuffer->max_level++;
                        size /= 2;
                    }
                }
                break;

            default:
                WARN("unknown attribute 0x%x\n", attr);
                SetLastError(ERROR_INVALID_DATA);
                goto done;
        }
    }

    if (!target || !internalFormat)
    {
        pbuffer->no_texture = TRUE;
        /* no actual way to turn off ability to texture; use most permissive target */
        target = GL_TEXTURE_RECTANGLE;
        internalFormat = GL_RGB;
    }

    err = CGLCreatePBuffer(iWidth, iHeight, target, internalFormat, pbuffer->max_level, &pbuffer->pbuffer);
    if (err != kCGLNoError)
    {
        WARN("CGLCreatePBuffer failed; err %d %s\n", err, CGLErrorString(err));
        pbuffer->pbuffer = NULL;
        if (err == kCGLBadAlloc)
            SetLastError(ERROR_NO_SYSTEM_RESOURCES);
        else
            SetLastError(ERROR_INVALID_DATA);
    }

done:
    if (!pbuffer->pbuffer)
    {
        HeapFree(GetProcessHeap(), 0, pbuffer);
        return NULL;
    }

    TRACE(" -> %p\n", pbuffer);
    return pbuffer;
}


/**********************************************************************
 *              macdrv_wglDestroyPbufferARB
 *
 * WGL_ARB_pbuffer: wglDestroyPbufferARB
 */
static BOOL macdrv_wglDestroyPbufferARB(struct wgl_pbuffer *pbuffer)
{
    TRACE("pbuffer %p\n", pbuffer);
    if (pbuffer && pbuffer->pbuffer)
        CGLReleasePBuffer(pbuffer->pbuffer);
    HeapFree(GetProcessHeap(), 0, pbuffer);
    return GL_TRUE;
}


/**********************************************************************
 *              macdrv_wglGetExtensionsStringARB
 *
 * WGL_ARB_extensions_string: wglGetExtensionsStringARB
 */
static const GLubyte *macdrv_wglGetExtensionsStringARB(HDC hdc)
{
    /* FIXME: Since we're given an HDC, this should be device-specific.  I.e.
              this can be specific to the CGL renderer like we're supposed to do. */
    TRACE("returning \"%s\"\n", gl_info.wglExtensions);
    return (const GLubyte*)gl_info.wglExtensions;
}


/**********************************************************************
 *              macdrv_wglGetExtensionsStringEXT
 *
 * WGL_EXT_extensions_string: wglGetExtensionsStringEXT
 */
static const GLubyte *macdrv_wglGetExtensionsStringEXT(void)
{
    TRACE("returning \"%s\"\n", gl_info.wglExtensions);
    return (const GLubyte*)gl_info.wglExtensions;
}


/**********************************************************************
 *              macdrv_wglGetPbufferDCARB
 *
 * WGL_ARB_pbuffer: wglGetPbufferDCARB
 */
static HDC macdrv_wglGetPbufferDCARB(struct wgl_pbuffer *pbuffer)
{
    HDC hdc;
    struct wgl_pbuffer *prev;

    hdc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    if (!hdc) return 0;

    EnterCriticalSection(&dc_pbuffers_section);
    prev = (struct wgl_pbuffer*)CFDictionaryGetValue(dc_pbuffers, hdc);
    if (prev)
    {
        CGLReleasePBuffer(prev->pbuffer);
        HeapFree(GetProcessHeap(), 0, prev);
    }
    CFDictionarySetValue(dc_pbuffers, hdc, pbuffer);
    LeaveCriticalSection(&dc_pbuffers_section);

    TRACE("pbuffer %p -> hdc %p\n", pbuffer, hdc);
    return hdc;
}


/**********************************************************************
 *              macdrv_wglGetPixelFormatAttribivARB
 *
 * WGL_ARB_pixel_format: wglGetPixelFormatAttribivARB
 */
static BOOL macdrv_wglGetPixelFormatAttribivARB(HDC hdc, int iPixelFormat, int iLayerPlane,
                                                UINT nAttributes, const int *piAttributes, int *piValues)
{
    const pixel_format *pf;
    UINT i;

    TRACE("hdc %p iPixelFormat %d iLayerPlane %d nAttributes %u piAttributes %p piValues %p\n",
          hdc, iPixelFormat, iLayerPlane, nAttributes, piAttributes, piValues);

    if (!nAttributes) return GL_TRUE;

    if (nAttributes == 1 && piAttributes[0] == WGL_NUMBER_PIXEL_FORMATS_ARB)
    {
        piValues[0] = nb_formats;
        TRACE("%s\n", debugstr_attrib(piAttributes[0], piValues[0]));
        return GL_TRUE;
    }

    pf = get_pixel_format(iPixelFormat, TRUE /* non-displayable */);
    if (!pf)
    {
        WARN("invalid pixel format %d\n", iPixelFormat);
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return GL_FALSE;
    }

    for (i = 0; i < nAttributes; ++i)
    {
        switch (piAttributes[i])
        {
            case WGL_NUMBER_PIXEL_FORMATS_ARB:
                piValues[i] = nb_formats;
                break;

            case WGL_DRAW_TO_WINDOW_ARB:
                piValues[i] = pf->window ? GL_TRUE : GL_FALSE;
                break;

            case WGL_DRAW_TO_BITMAP_ARB:
                piValues[i] = GL_FALSE;
                break;

            case WGL_ACCELERATION_ARB:
                if (iLayerPlane) goto invalid_layer;
                if (pf->accelerated)
                    piValues[i] = WGL_FULL_ACCELERATION_ARB;
                else
                    piValues[i] = WGL_NO_ACCELERATION_ARB;
                break;

            case WGL_NEED_PALETTE_ARB:
            case WGL_NEED_SYSTEM_PALETTE_ARB:
            case WGL_SWAP_LAYER_BUFFERS_ARB:
                piValues[i] = GL_FALSE;
                break;

            case WGL_SWAP_METHOD_ARB:
                if (pf->double_buffer && pf->backing_store)
                    piValues[i] = WGL_SWAP_COPY_ARB;
                else
                    piValues[i] = WGL_SWAP_UNDEFINED_ARB;
                break;

            case WGL_NUMBER_OVERLAYS_ARB:
            case WGL_NUMBER_UNDERLAYS_ARB:
                piValues[i] = 0;
                break;

            case WGL_TRANSPARENT_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = GL_FALSE;
                break;

            case WGL_TRANSPARENT_RED_VALUE_ARB:
            case WGL_TRANSPARENT_GREEN_VALUE_ARB:
            case WGL_TRANSPARENT_BLUE_VALUE_ARB:
            case WGL_TRANSPARENT_ALPHA_VALUE_ARB:
            case WGL_TRANSPARENT_INDEX_VALUE_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = 0;
                break;

            case WGL_SHARE_DEPTH_ARB:
            case WGL_SHARE_STENCIL_ARB:
            case WGL_SHARE_ACCUM_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = GL_TRUE;
                break;

            case WGL_SUPPORT_GDI_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = GL_FALSE;
                break;

            case WGL_SUPPORT_OPENGL_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = GL_TRUE;
                break;

            case WGL_DOUBLE_BUFFER_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = pf->double_buffer ? GL_TRUE : GL_FALSE;
                break;

            case WGL_STEREO_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = pf->stereo ? GL_TRUE : GL_FALSE;
                break;

            case WGL_PIXEL_TYPE_ARB:
                if (iLayerPlane) goto invalid_layer;
                if (color_modes[pf->color_mode].is_float)
                    piValues[i] = WGL_TYPE_RGBA_FLOAT_ARB;
                else
                    piValues[i] = WGL_TYPE_RGBA_ARB;
                /* WGL_EXT_pixel_format_packed_float may be supported, which should in theory
                   make another pixel type available: WGL_TYPE_RGBA_UNSIGNED_FLOAT_EXT.
                   However, Mac contexts don't support rendering to unsigned floating-point
                   formats, even when GL_EXT_packed_float is supported. */
                break;

            case WGL_COLOR_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                /* If the mode doesn't have alpha, return bits per pixel instead
                   of color bits.  On Windows, color bits sometimes exceeds r+g+b
                   (e.g. it's 32 for an R8G8B8A0 pixel format).  If an app depends
                   on that and expects that WGL_COLOR_BITS_ARB >= 32 for such a
                   pixel format, we need to accommodate that. */
                if (color_modes[pf->color_mode].alpha_bits)
                    piValues[i] = color_modes[pf->color_mode].color_bits;
                else
                    piValues[i] = color_modes[pf->color_mode].bits_per_pixel;
                break;

            case WGL_RED_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = color_modes[pf->color_mode].red_bits;
                break;

            case WGL_RED_SHIFT_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = color_modes[pf->color_mode].red_shift;
                break;

            case WGL_GREEN_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = color_modes[pf->color_mode].green_bits;
                break;

            case WGL_GREEN_SHIFT_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = color_modes[pf->color_mode].green_shift;
                break;

            case WGL_BLUE_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = color_modes[pf->color_mode].blue_bits;
                break;

            case WGL_BLUE_SHIFT_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = color_modes[pf->color_mode].blue_shift;
                break;

            case WGL_ALPHA_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = color_modes[pf->color_mode].alpha_bits;
                break;

            case WGL_ALPHA_SHIFT_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = color_modes[pf->color_mode].alpha_shift;
                break;

            case WGL_ACCUM_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                if (pf->accum_mode)
                    piValues[i] = color_modes[pf->accum_mode - 1].color_bits;
                else
                    piValues[i] = 0;
                break;

            case WGL_ACCUM_RED_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                if (pf->accum_mode)
                    piValues[i] = color_modes[pf->accum_mode - 1].red_bits;
                else
                    piValues[i] = 0;
                break;

            case WGL_ACCUM_GREEN_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                if (pf->accum_mode)
                    piValues[i] = color_modes[pf->accum_mode - 1].green_bits;
                else
                    piValues[i] = 0;
                break;

            case WGL_ACCUM_BLUE_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                if (pf->accum_mode)
                    piValues[i] = color_modes[pf->accum_mode - 1].blue_bits;
                else
                    piValues[i] = 0;
                break;

            case WGL_ACCUM_ALPHA_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                if (pf->accum_mode)
                    piValues[i] = color_modes[pf->accum_mode - 1].alpha_bits;
                else
                    piValues[i] = 0;
                break;

            case WGL_DEPTH_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = pf->depth_bits;
                break;

            case WGL_STENCIL_BITS_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = pf->stencil_bits;
                break;

            case WGL_AUX_BUFFERS_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = pf->aux_buffers;
                break;

            case WGL_SAMPLE_BUFFERS_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = pf->sample_buffers;
                break;

            case WGL_SAMPLES_ARB:
                if (iLayerPlane) goto invalid_layer;
                piValues[i] = pf->samples;
                break;

            case WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB: /* a.k.a. WGL_FRAMEBUFFER_SRGB_CAPABLE_EXT */
                if (iLayerPlane) goto invalid_layer;
                /* sRGB is only supported for 8-bit integer color components */
                if (color_modes[pf->color_mode].red_bits == 8 &&
                    color_modes[pf->color_mode].green_bits == 8 &&
                    color_modes[pf->color_mode].blue_bits == 8 &&
                    !color_modes[pf->color_mode].is_float)
                    piValues[i] = GL_TRUE;
                else
                    piValues[i] = GL_FALSE;
                break;

            case WGL_DRAW_TO_PBUFFER_ARB:
            case WGL_BIND_TO_TEXTURE_RGB_ARB:
            case WGL_BIND_TO_TEXTURE_RECTANGLE_RGB_NV:
                piValues[i] = pf->pbuffer ? GL_TRUE : GL_FALSE;
                break;

            case WGL_BIND_TO_TEXTURE_RGBA_ARB:
            case WGL_BIND_TO_TEXTURE_RECTANGLE_RGBA_NV:
                piValues[i] = (pf->pbuffer && color_modes[pf->color_mode].alpha_bits) ? GL_TRUE : GL_FALSE;
                break;

            case WGL_MAX_PBUFFER_WIDTH_ARB:
                piValues[i] = gl_info.max_viewport_dims[0];
                break;

            case WGL_MAX_PBUFFER_HEIGHT_ARB:
                piValues[i] = gl_info.max_viewport_dims[1];
                break;

            case WGL_MAX_PBUFFER_PIXELS_ARB:
                piValues[i] = gl_info.max_viewport_dims[0] * gl_info.max_viewport_dims[1];
                break;

            default:
                WARN("invalid attribute %x\n", piAttributes[i]);
                return GL_FALSE;
        }

        TRACE("%s\n", debugstr_attrib(piAttributes[i], piValues[i]));
    }

    return GL_TRUE;

invalid_layer:
    FIXME("unsupported iLayerPlane %d\n", iLayerPlane);
    return GL_FALSE;
}


/**********************************************************************
 *              macdrv_wglGetPixelFormatAttribfvARB
 *
 * WGL_ARB_pixel_format: wglGetPixelFormatAttribfvARB
 */
static BOOL macdrv_wglGetPixelFormatAttribfvARB(HDC hdc, int iPixelFormat, int iLayerPlane,
                                                UINT nAttributes, const int *piAttributes, FLOAT *pfValues)
{
    int *attr;
    int ret;

    TRACE("hdc %p iPixelFormat %d iLayerPlane %d nAttributes %u piAttributes %p pfValues %p\n",
          hdc, iPixelFormat, iLayerPlane, nAttributes, piAttributes, pfValues);

    /* Allocate a temporary array to store integer values */
    attr = HeapAlloc(GetProcessHeap(), 0, nAttributes * sizeof(int));
    if (!attr)
    {
        ERR("couldn't allocate %d array\n", nAttributes);
        return GL_FALSE;
    }

    /* Piggy-back on wglGetPixelFormatAttribivARB */
    ret = macdrv_wglGetPixelFormatAttribivARB(hdc, iPixelFormat, iLayerPlane, nAttributes, piAttributes, attr);
    if (ret)
    {
        UINT i;

        /* Convert integer values to float. Should also check for attributes
           that can give decimal values here */
        for (i = 0; i < nAttributes; i++)
            pfValues[i] = attr[i];
    }

    HeapFree(GetProcessHeap(), 0, attr);
    return ret;
}


/**********************************************************************
 *              macdrv_wglGetSwapIntervalEXT
 *
 * WGL_EXT_swap_control: wglGetSwapIntervalEXT
 */
static int macdrv_wglGetSwapIntervalEXT(void)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;
    long value;
    CGLError err;

    TRACE("\n");

    err = CGLGetParameter(context->cglcontext, kCGLCPSwapInterval, (GLint*)&value);
    if (err != kCGLNoError)
    {
        WARN("CGLGetParameter(kCGLCPSwapInterval) failed; error %d %s\n",
             err, CGLErrorString(err));
        value = 1;
    }

    return value;
}


/***********************************************************************
 *              macdrv_wglMakeContextCurrentARB
 *
 * WGL_ARB_make_current_read: wglMakeContextCurrentARB
 *
 * This is not supported directly by OpenGL on the Mac.  We emulate it
 * by hooking into glReadPixels, glCopyPixels, and glCopyColorTable to
 * temporarily swap the drawable.  This follows the technique used in
 * the implementation of Mesa GLX for Apple.
 */
static BOOL macdrv_wglMakeContextCurrentARB(HDC draw_hdc, HDC read_hdc, struct wgl_context *context)
{
    struct macdrv_win_data *data;
    HWND hwnd;

    TRACE("draw_hdc %p read_hdc %p context %p/%p/%p\n", draw_hdc, read_hdc, context,
          (context ? context->context : NULL), (context ? context->cglcontext : NULL));

    if (!context)
    {
        macdrv_make_context_current(NULL, NULL);
        NtCurrentTeb()->glContext = NULL;
        return TRUE;
    }

    if ((hwnd = WindowFromDC(draw_hdc)))
    {
        if (!(data = get_win_data(hwnd)))
        {
            FIXME("draw DC for window %p of other process: not implemented\n", hwnd);
            return FALSE;
        }

        if (!data->pixel_format)
        {
            WARN("no pixel format set\n");
            release_win_data(data);
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        if (context->format != data->pixel_format)
        {
            WARN("mismatched pixel format draw_hdc %p %u context %p %u\n", draw_hdc, data->pixel_format, context, context->format);
            release_win_data(data);
            SetLastError(ERROR_INVALID_PIXEL_FORMAT);
            return FALSE;
        }

        context->draw_view = data->gl_view;
        context->draw_pbuffer = NULL;
        release_win_data(data);
    }
    else
    {
        struct wgl_pbuffer *pbuffer;

        EnterCriticalSection(&dc_pbuffers_section);
        pbuffer = (struct wgl_pbuffer*)CFDictionaryGetValue(dc_pbuffers, draw_hdc);
        if (pbuffer)
        {
            if (context->format != pbuffer->format)
            {
                WARN("mismatched pixel format draw_hdc %p %u context %p %u\n", draw_hdc, pbuffer->format, context, context->format);
                LeaveCriticalSection(&dc_pbuffers_section);
                SetLastError(ERROR_INVALID_PIXEL_FORMAT);
                return FALSE;
            }
        }
        else
        {
            WARN("no window or pbuffer for DC\n");
            LeaveCriticalSection(&dc_pbuffers_section);
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }

        context->draw_view = NULL;
        context->draw_pbuffer = pbuffer;
        LeaveCriticalSection(&dc_pbuffers_section);
    }

    context->read_view = NULL;
    context->read_pbuffer = NULL;
    if (read_hdc && read_hdc != draw_hdc)
    {
        if ((hwnd = WindowFromDC(read_hdc)))
        {
            if ((data = get_win_data(hwnd)))
            {
                if (data->gl_view != context->draw_view)
                    context->read_view = data->gl_view;
                release_win_data(data);
            }
        }
        else
        {
            EnterCriticalSection(&dc_pbuffers_section);
            context->read_pbuffer = (struct wgl_pbuffer*)CFDictionaryGetValue(dc_pbuffers, read_hdc);
            LeaveCriticalSection(&dc_pbuffers_section);
        }
    }

    TRACE("making context current with draw_view %p draw_pbuffer %p read_view %p read_pbuffer %p format %u\n",
          context->draw_view, context->draw_pbuffer, context->read_view, context->read_pbuffer, context->format);

    make_context_current(context, FALSE);
    context->has_been_current = TRUE;
    NtCurrentTeb()->glContext = context;

    return TRUE;
}


/**********************************************************************
 *              macdrv_wglQueryPbufferARB
 *
 * WGL_ARB_pbuffer: wglQueryPbufferARB
 */
static BOOL macdrv_wglQueryPbufferARB(struct wgl_pbuffer *pbuffer, int iAttribute, int *piValue)
{
    CGLError err;
    GLsizei width;
    GLsizei height;
    GLenum target;
    GLenum internalFormat;
    GLint mipmap;

    TRACE("pbuffer %p iAttribute 0x%x piValue %p\n", pbuffer, iAttribute, piValue);

    err = CGLDescribePBuffer(pbuffer->pbuffer, &width, &height, &target, &internalFormat, &mipmap);
    if (err != kCGLNoError)
    {
        WARN("CGLDescribePBuffer failed; error %d %s\n", err, CGLErrorString(err));
        SetLastError(ERROR_INVALID_HANDLE);
        return GL_FALSE;
    }

    switch (iAttribute)
    {
        case WGL_PBUFFER_WIDTH_ARB:
            *piValue = width;
            break;
        case WGL_PBUFFER_HEIGHT_ARB:
            *piValue = height;
            break;
        case WGL_PBUFFER_LOST_ARB:
            /* Mac PBuffers can't be lost */
            *piValue = GL_FALSE;
            break;
        case WGL_TEXTURE_FORMAT_ARB:
            if (pbuffer->no_texture)
                *piValue = WGL_NO_TEXTURE_ARB;
            else switch (internalFormat)
            {
                case GL_RGBA:
                    *piValue = WGL_TEXTURE_RGBA_ARB;
                    break;
                case GL_RGB:
                default:
                    *piValue = WGL_TEXTURE_RGB_ARB;
                    break;
            }
            break;
        case WGL_TEXTURE_TARGET_ARB:
            if (pbuffer->no_texture)
                *piValue = WGL_NO_TEXTURE_ARB;
            else switch (target)
            {
                case GL_TEXTURE_CUBE_MAP:
                    *piValue = WGL_TEXTURE_CUBE_MAP_ARB;
                    break;
                case GL_TEXTURE_2D:
                    *piValue = WGL_TEXTURE_2D_ARB;
                    break;
                case GL_TEXTURE_RECTANGLE:
                default:
                    *piValue = WGL_TEXTURE_RECTANGLE_NV;
                    break;
            }
            break;
        case WGL_MIPMAP_TEXTURE_ARB:
            *piValue = (pbuffer->max_level > 0);
            break;
        case WGL_MIPMAP_LEVEL_ARB:
            *piValue = pbuffer->level;
            break;
        case WGL_CUBE_MAP_FACE_ARB:
            switch (pbuffer->face)
            {
                case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
                default:
                    *piValue = WGL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB;
                    break;
                case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
                    *piValue = WGL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB;
                    break;
                case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
                    *piValue = WGL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB;
                    break;
                case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
                    *piValue = WGL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB;
                    break;
                case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
                    *piValue = WGL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB;
                    break;
                case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
                    *piValue = WGL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB;
                    break;
            }
            break;
        default:
            WARN("invalid attribute 0x%x\n", iAttribute);
            SetLastError(ERROR_INVALID_DATA);
            return GL_FALSE;
    }

    return GL_TRUE;
}


/**********************************************************************
 *              macdrv_wglReleasePbufferDCARB
 *
 * WGL_ARB_pbuffer: wglReleasePbufferDCARB
 */
static int macdrv_wglReleasePbufferDCARB(struct wgl_pbuffer *pbuffer, HDC hdc)
{
    struct wgl_pbuffer *prev;

    TRACE("pbuffer %p hdc %p\n", pbuffer, hdc);

    EnterCriticalSection(&dc_pbuffers_section);

    prev = (struct wgl_pbuffer*)CFDictionaryGetValue(dc_pbuffers, hdc);
    if (prev)
    {
        if (prev != pbuffer)
            FIXME("hdc %p isn't associated with pbuffer %p\n", hdc, pbuffer);
        CGLReleasePBuffer(prev->pbuffer);
        HeapFree(GetProcessHeap(), 0, prev);
        CFDictionaryRemoveValue(dc_pbuffers, hdc);
    }
    else hdc = 0;

    LeaveCriticalSection(&dc_pbuffers_section);

    return hdc && DeleteDC(hdc);
}


/**********************************************************************
 *              macdrv_wglReleaseTexImageARB
 *
 * WGL_ARB_render_texture: wglReleaseTexImageARB
 */
static BOOL macdrv_wglReleaseTexImageARB(struct wgl_pbuffer *pbuffer, int iBuffer)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;
    CGLError err;

    TRACE("pbuffer %p iBuffer 0x%x; stub!\n", pbuffer, iBuffer);

    if (pbuffer->no_texture)
    {
        SetLastError(ERROR_INVALID_OPERATION);
        return GL_FALSE;
    }

    err = CGLTexImagePBuffer(context->cglcontext, pbuffer->pbuffer, GL_NONE);
    if (err != kCGLNoError)
    {
        WARN("CGLTexImagePBuffer failed with err %d %s\n", err, CGLErrorString(err));
        SetLastError(ERROR_INVALID_OPERATION);
        return GL_FALSE;
    }

    return GL_TRUE;
}


/**********************************************************************
 *              macdrv_wglSetPbufferAttribARB
 *
 * WGL_ARB_render_texture: wglSetPbufferAttribARB
 */
static BOOL macdrv_wglSetPbufferAttribARB(struct wgl_pbuffer *pbuffer, const int *piAttribList)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;

    TRACE("pbuffer %p piAttribList %p\n", pbuffer, piAttribList);

    for ( ; piAttribList && *piAttribList; piAttribList += 2)
    {
        int attr = piAttribList[0];
        int value = piAttribList[1];
        switch (attr)
        {
            case WGL_MIPMAP_LEVEL_ARB:
                TRACE("WGL_MIPMAP_LEVEL_ARB: %d\n", value);
                pbuffer->level = value;
                break;
            case WGL_CUBE_MAP_FACE_ARB:
                switch (value)
                {
                    case WGL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB:
                        TRACE("WGL_CUBE_MAP_FACE_ARB: WGL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB\n");
                        pbuffer->face = GL_TEXTURE_CUBE_MAP_POSITIVE_X;
                        break;
                    case WGL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB:
                        TRACE("WGL_CUBE_MAP_FACE_ARB: WGL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB\n");
                        pbuffer->face = GL_TEXTURE_CUBE_MAP_NEGATIVE_X;
                        break;
                    case WGL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB:
                        TRACE("WGL_CUBE_MAP_FACE_ARB: WGL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB\n");
                        pbuffer->face = GL_TEXTURE_CUBE_MAP_POSITIVE_Y;
                        break;
                    case WGL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB:
                        TRACE("WGL_CUBE_MAP_FACE_ARB: WGL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB\n");
                        pbuffer->face = GL_TEXTURE_CUBE_MAP_NEGATIVE_Y;
                        break;
                    case WGL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB:
                        TRACE("WGL_CUBE_MAP_FACE_ARB: WGL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB\n");
                        pbuffer->face = GL_TEXTURE_CUBE_MAP_POSITIVE_Z;
                        break;
                    case WGL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB:
                        TRACE("WGL_CUBE_MAP_FACE_ARB: WGL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB\n");
                        pbuffer->face = GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
                        break;
                    default:
                        WARN("unknown WGL_CUBE_MAP_FACE_ARB value 0x%x\n", value);
                        SetLastError(ERROR_INVALID_DATA);
                        return GL_FALSE;
                }
                break;
            default:
                WARN("invalide attribute 0x%x\n", attr);
                SetLastError(ERROR_INVALID_DATA);
                return GL_FALSE;
        }
    }

    if (context && context->draw_pbuffer == pbuffer)
        make_context_current(context, FALSE);

    return GL_TRUE;
}


/**********************************************************************
 *              macdrv_wglSetPixelFormatWINE
 *
 * WGL_WINE_pixel_format_passthrough: wglSetPixelFormatWINE
 */
static BOOL macdrv_wglSetPixelFormatWINE(HDC hdc, int fmt)
{
    return set_pixel_format(hdc, fmt, TRUE);
}


/**********************************************************************
 *              macdrv_wglSwapIntervalEXT
 *
 * WGL_EXT_swap_control: wglSwapIntervalEXT
 */
static BOOL macdrv_wglSwapIntervalEXT(int interval)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;
    long value;
    CGLError err;

    TRACE("interval %d\n", interval);

    if (interval < 0)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    if (!pixel_formats[context->format - 1].double_buffer)
        interval = 0;
    else if (interval > 1)
        interval = 1;

    value = interval;
    err = CGLSetParameter(context->cglcontext, kCGLCPSwapInterval, (GLint*)&value);
    if (err != kCGLNoError)
    {
        WARN("CGLSetParameter(kCGLCPSwapInterval) failed; error %d %s\n",
             err, CGLErrorString(err));
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }

    return TRUE;
}


static void register_extension(const char *ext)
{
    if (gl_info.wglExtensions[0])
        strcat(gl_info.wglExtensions, " ");
    strcat(gl_info.wglExtensions, ext);

    TRACE("'%s'\n", ext);
}

static void load_extensions(void)
{
    /*
     * ARB Extensions
     */
    register_extension("WGL_ARB_extensions_string");
    opengl_funcs.ext.p_wglGetExtensionsStringARB = macdrv_wglGetExtensionsStringARB;

    register_extension("WGL_ARB_make_current_read");
    opengl_funcs.ext.p_wglGetCurrentReadDCARB   = (void *)1;  /* never called */
    opengl_funcs.ext.p_wglMakeContextCurrentARB = macdrv_wglMakeContextCurrentARB;

    register_extension("WGL_ARB_pixel_format");
    opengl_funcs.ext.p_wglChoosePixelFormatARB      = macdrv_wglChoosePixelFormatARB;
    opengl_funcs.ext.p_wglGetPixelFormatAttribfvARB = macdrv_wglGetPixelFormatAttribfvARB;
    opengl_funcs.ext.p_wglGetPixelFormatAttribivARB = macdrv_wglGetPixelFormatAttribivARB;

    if (gluCheckExtension((GLubyte*)"GL_ARB_color_buffer_float", (GLubyte*)gl_info.glExtensions))
    {
        register_extension("WGL_ARB_pixel_format_float");
        register_extension("WGL_ATI_pixel_format_float");
    }

    if (gluCheckExtension((GLubyte*)"GL_ARB_multisample", (GLubyte*)gl_info.glExtensions))
        register_extension("WGL_ARB_multisample");

    if (gluCheckExtension((GLubyte*)"GL_ARB_framebuffer_sRGB", (GLubyte*)gl_info.glExtensions))
        register_extension("WGL_ARB_framebuffer_sRGB");

    if (gluCheckExtension((GLubyte*)"GL_APPLE_pixel_buffer", (GLubyte*)gl_info.glExtensions))
    {
        register_extension("WGL_ARB_pbuffer");
        opengl_funcs.ext.p_wglCreatePbufferARB    = macdrv_wglCreatePbufferARB;
        opengl_funcs.ext.p_wglDestroyPbufferARB   = macdrv_wglDestroyPbufferARB;
        opengl_funcs.ext.p_wglGetPbufferDCARB     = macdrv_wglGetPbufferDCARB;
        opengl_funcs.ext.p_wglQueryPbufferARB     = macdrv_wglQueryPbufferARB;
        opengl_funcs.ext.p_wglReleasePbufferDCARB = macdrv_wglReleasePbufferDCARB;

        register_extension("WGL_ARB_render_texture");
        opengl_funcs.ext.p_wglBindTexImageARB       = macdrv_wglBindTexImageARB;
        opengl_funcs.ext.p_wglReleaseTexImageARB    = macdrv_wglReleaseTexImageARB;
        opengl_funcs.ext.p_wglSetPbufferAttribARB   = macdrv_wglSetPbufferAttribARB;

        if (gluCheckExtension((GLubyte*)"GL_ARB_texture_rectangle", (GLubyte*)gl_info.glExtensions) ||
            gluCheckExtension((GLubyte*)"GL_EXT_texture_rectangle", (GLubyte*)gl_info.glExtensions))
            register_extension("WGL_NV_render_texture_rectangle");
    }

    /* TODO:
        WGL_ARB_create_context: wglCreateContextAttribsARB
        WGL_ARB_create_context_profile
     */

    /*
     * EXT Extensions
     */
    register_extension("WGL_EXT_extensions_string");
    opengl_funcs.ext.p_wglGetExtensionsStringEXT = macdrv_wglGetExtensionsStringEXT;

    register_extension("WGL_EXT_swap_control");
    opengl_funcs.ext.p_wglSwapIntervalEXT = macdrv_wglSwapIntervalEXT;
    opengl_funcs.ext.p_wglGetSwapIntervalEXT = macdrv_wglGetSwapIntervalEXT;

    /* Presumably identical to [W]GL_ARB_framebuffer_sRGB, above, but clients may
       check for either, so register them separately. */
    if (gluCheckExtension((GLubyte*)"GL_EXT_framebuffer_sRGB", (GLubyte*)gl_info.glExtensions))
        register_extension("WGL_EXT_framebuffer_sRGB");

    if (gluCheckExtension((GLubyte*)"GL_EXT_packed_float", (GLubyte*)gl_info.glExtensions))
        register_extension("WGL_EXT_pixel_format_packed_float");

    /*
     * WINE-specific WGL Extensions
     */

    /* In WineD3D we need the ability to set the pixel format more than once (e.g. after a device reset).
     * The default wglSetPixelFormat doesn't allow this, so add our own which allows it.
     */
    register_extension("WGL_WINE_pixel_format_passthrough");
    opengl_funcs.ext.p_wglSetPixelFormatWINE = macdrv_wglSetPixelFormatWINE;
}


static BOOL init_opengl(void)
{
    static int init_done;
    unsigned int i;
    char buffer[200];

    if (init_done) return (opengl_handle != NULL);
    init_done = 1;

    TRACE("()\n");

    dc_pbuffers = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    if (!dc_pbuffers)
    {
        WARN("CFDictionaryCreateMutable failed\n");
        return FALSE;
    }

    opengl_handle = wine_dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY|RTLD_LOCAL|RTLD_NOLOAD, buffer, sizeof(buffer));
    if (!opengl_handle)
    {
        ERR("Failed to load OpenGL: %s\n", buffer);
        ERR("OpenGL support is disabled.\n");
        return FALSE;
    }

    for (i = 0; i < sizeof(opengl_func_names)/sizeof(opengl_func_names[0]); i++)
    {
        if (!(((void **)&opengl_funcs.gl)[i] = wine_dlsym(opengl_handle, opengl_func_names[i], NULL, 0)))
        {
            ERR("%s not found in OpenGL, disabling.\n", opengl_func_names[i]);
            goto failed;
        }
    }

    /* redirect some standard OpenGL functions */
#define REDIRECT(func) \
    do { p##func = opengl_funcs.gl.p_##func; opengl_funcs.gl.p_##func = macdrv_##func; } while(0)
    REDIRECT(glCopyPixels);
    REDIRECT(glFlush);
    REDIRECT(glReadPixels);
    REDIRECT(glViewport);
#undef REDIRECT

    /* redirect some OpenGL extension functions */
#define REDIRECT(func) \
    do { if (opengl_funcs.ext.p_##func) { p##func = opengl_funcs.ext.p_##func; opengl_funcs.ext.p_##func = macdrv_##func; } } while(0)
    REDIRECT(glCopyColorTable);
#undef REDIRECT

    if (!init_gl_info())
        goto failed;

    if (gluCheckExtension((GLubyte*)"GL_APPLE_flush_render", (GLubyte*)gl_info.glExtensions))
        pglFlushRenderAPPLE = wine_dlsym(opengl_handle, "glFlushRenderAPPLE", NULL, 0);

    load_extensions();
    if (!init_pixel_formats())
        goto failed;

    return TRUE;

failed:
    wine_dlclose(opengl_handle, NULL, 0);
    opengl_handle = NULL;
    return FALSE;
}


/***********************************************************************
 *              sync_gl_view
 *
 * Synchronize the Mac GL view position with the Windows child window
 * position.
 */
void sync_gl_view(struct macdrv_win_data *data)
{
    RECT rect;

    TRACE("hwnd %p gl_view %p\n", data->hwnd, data->gl_view);

    if (!data->gl_view) return;

    if (get_gl_view_window_rect(data, NULL, &rect) && memcmp(&data->gl_rect, &rect, sizeof(rect)))
    {
        TRACE("Setting GL view %p frame to %s\n", data->gl_view, wine_dbgstr_rect(&rect));
        macdrv_set_view_window_and_frame(data->gl_view, NULL, cgrect_from_rect(rect));
        data->gl_rect = rect;
    }
}


static int get_dc_pixel_format(HDC hdc)
{
    int format;
    HWND hwnd;

    if ((hwnd = WindowFromDC(hdc)))
    {
        struct macdrv_win_data *data;

        if (!(data = get_win_data(hwnd)))
        {
            FIXME("DC for window %p of other process: not implemented\n", hwnd);
            return 0;
        }

        format = data->pixel_format;
        release_win_data(data);
    }
    else
    {
        struct wgl_pbuffer *pbuffer;

        EnterCriticalSection(&dc_pbuffers_section);
        pbuffer = (struct wgl_pbuffer*)CFDictionaryGetValue(dc_pbuffers, hdc);
        if (pbuffer)
            format = pbuffer->format;
        else
        {
            WARN("no window or pbuffer for DC %p\n", hdc);
            format = 0;
        }
        LeaveCriticalSection(&dc_pbuffers_section);
    }

    return format;
}


/**********************************************************************
 *              create_context
 */
static BOOL create_context(struct wgl_context *context, CGLContextObj share)
{
    const pixel_format *pf;
    CGLPixelFormatAttribute attribs[64];
    int n = 0;
    CGLPixelFormatObj pix;
    GLint virtualScreens;
    CGLError err;
    long swap_interval;

    pf = get_pixel_format(context->format, TRUE /* non-displayable */);
    if (!pf)
    {
        ERR("Invalid pixel format %d, expect problems!\n", context->format);
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return FALSE;
    }

    attribs[n++] = kCGLPFAMinimumPolicy;
    attribs[n++] = kCGLPFAClosestPolicy;

    if (pf->accelerated)
    {
        attribs[n++] = kCGLPFAAccelerated;
        attribs[n++] = kCGLPFANoRecovery;
    }
    else
    {
        attribs[n++] = kCGLPFARendererID;
        attribs[n++] = kCGLRendererGenericFloatID;
    }

    if (pf->double_buffer)
        attribs[n++] = kCGLPFADoubleBuffer;

    attribs[n++] = kCGLPFAAuxBuffers;
    attribs[n++] = pf->aux_buffers;

    attribs[n++] = kCGLPFAColorSize;
    attribs[n++] = color_modes[pf->color_mode].color_bits;
    attribs[n++] = kCGLPFAAlphaSize;
    attribs[n++] = color_modes[pf->color_mode].alpha_bits;
    if (color_modes[pf->color_mode].is_float)
        attribs[n++] = kCGLPFAColorFloat;

    attribs[n++] = kCGLPFADepthSize;
    attribs[n++] = pf->depth_bits;

    attribs[n++] = kCGLPFAStencilSize;
    attribs[n++] = pf->stencil_bits;

    if (pf->stereo)
        attribs[n++] = kCGLPFAStereo;

    if (pf->accum_mode)
    {
        attribs[n++] = kCGLPFAAccumSize;
        attribs[n++] = color_modes[pf->accum_mode - 1].color_bits;
    }

    if (pf->window)
        attribs[n++] = kCGLPFAWindow;
    if (pf->pbuffer)
        attribs[n++] = kCGLPFAPBuffer;

    if (pf->sample_buffers && pf->samples)
    {
        attribs[n++] = kCGLPFASampleBuffers;
        attribs[n++] = pf->sample_buffers;
        attribs[n++] = kCGLPFASamples;
        attribs[n++] = pf->samples;
    }

    if (pf->backing_store)
        attribs[n++] = kCGLPFABackingStore;

    attribs[n] = 0;

    err = CGLChoosePixelFormat(attribs, &pix, &virtualScreens);
    if (err != kCGLNoError || !pix)
    {
        WARN("CGLChoosePixelFormat() failed with error %d %s\n", err, CGLErrorString(err));
        return FALSE;
    }

    err = CGLCreateContext(pix, share, &context->cglcontext);
    CGLReleasePixelFormat(pix);
    if (err != kCGLNoError || !context->cglcontext)
    {
        context->cglcontext = NULL;
        WARN("CGLCreateContext() failed with error %d %s\n", err, CGLErrorString(err));
        return FALSE;
    }

    /* According to the WGL_EXT_swap_control docs, the default swap interval for
       a context is 1.  CGL contexts default to 0, so we need to set it.  This
       only make sense for double-buffered contexts, though.  In theory, for
       single-buffered contexts, there's no such thing as a swap.  But OS X
       will synchronize flushes of single-buffered contexts if this is set. */
    if (pf->double_buffer)
        swap_interval = 1;
    else
        swap_interval = 0;
    err = CGLSetParameter(context->cglcontext, kCGLCPSwapInterval, (GLint*)&swap_interval);
    if (err != kCGLNoError)
        WARN("CGLSetParameter(kCGLCPSwapInterval) failed with error %d %s; leaving un-vsynced\n", err, CGLErrorString(err));

    context->context = macdrv_create_opengl_context(context->cglcontext);
    CGLReleaseContext(context->cglcontext);
    if (!context->context)
    {
        WARN("macdrv_create_opengl_context() failed\n");
        return FALSE;
    }

    TRACE("created context %p/%p/%p\n", context, context->context, context->cglcontext);

    return TRUE;
}


/**********************************************************************
 *              macdrv_wglDescribePixelFormat
 */
int macdrv_wglDescribePixelFormat(HDC hdc, int fmt, UINT size, PIXELFORMATDESCRIPTOR *descr)
{
    int ret = nb_formats;
    const pixel_format *pf;
    const struct color_mode *mode;

    TRACE("hdc %p fmt %d size %u descr %p\n", hdc, fmt, size, descr);

    if (fmt <= 0 || fmt > ret) return ret;
    if (size < sizeof(*descr)) return 0;

    pf = &pixel_formats[fmt - 1];

    memset(descr, 0, sizeof(*descr));
    descr->nSize            = sizeof(*descr);
    descr->nVersion         = 1;

    descr->dwFlags          = PFD_SUPPORT_OPENGL;
    if (pf->window)         descr->dwFlags |= PFD_DRAW_TO_WINDOW;
    if (!pf->accelerated)   descr->dwFlags |= PFD_GENERIC_FORMAT;
    if (pf->double_buffer)  descr->dwFlags |= PFD_DOUBLEBUFFER;
    if (pf->stereo)         descr->dwFlags |= PFD_STEREO;
    if (pf->backing_store)  descr->dwFlags |= PFD_SWAP_COPY;

    descr->iPixelType       = PFD_TYPE_RGBA;

    mode = &color_modes[pf->color_mode];
    /* If the mode doesn't have alpha, return bits per pixel instead of color bits.
       On Windows, color bits sometimes exceeds r+g+b (e.g. it's 32 for an
       R8G8B8A0 pixel format).  If an app depends on that and expects that
       cColorBits >= 32 for such a pixel format, we need to accommodate that. */
    if (mode->alpha_bits)
        descr->cColorBits   = mode->color_bits;
    else
        descr->cColorBits   = mode->bits_per_pixel;
    descr->cRedBits         = mode->red_bits;
    descr->cRedShift        = mode->red_shift;
    descr->cGreenBits       = mode->green_bits;
    descr->cGreenShift      = mode->green_shift;
    descr->cBlueBits        = mode->blue_bits;
    descr->cBlueShift       = mode->blue_shift;
    descr->cAlphaBits       = mode->alpha_bits;
    descr->cAlphaShift      = mode->alpha_shift;

    if (pf->accum_mode)
    {
        mode = &color_modes[pf->accum_mode - 1];
        descr->cAccumBits       = mode->color_bits;
        descr->cAccumRedBits    = mode->red_bits;
        descr->cAccumGreenBits  = mode->green_bits;
        descr->cAccumBlueBits   = mode->blue_bits;
        descr->cAccumAlphaBits  = mode->alpha_bits;
    }

    descr->cDepthBits       = pf->depth_bits;
    descr->cStencilBits     = pf->stencil_bits;
    descr->cAuxBuffers      = pf->aux_buffers;
    descr->iLayerType       = PFD_MAIN_PLANE;
    return ret;
}

/***********************************************************************
 *              macdrv_wglCopyContext
 */
static BOOL macdrv_wglCopyContext(struct wgl_context *src, struct wgl_context *dst, UINT mask)
{
    CGLError err;

    TRACE("src %p dst %p mask %x\n", src, dst, mask);

    err = CGLCopyContext(src->cglcontext, dst->cglcontext, mask);
    if (err != kCGLNoError)
        WARN("CGLCopyContext() failed with err %d %s\n", err, CGLErrorString(err));
    return (err == kCGLNoError);
}

/***********************************************************************
 *              macdrv_wglCreateContext
 */
static struct wgl_context *macdrv_wglCreateContext(HDC hdc)
{
    int format;
    struct wgl_context *context;

    TRACE("hdc %p\n", hdc);

    format = get_dc_pixel_format(hdc);

    if (!is_valid_pixel_format(format))
    {
        ERR("Invalid pixel format %d, expect problems!\n", format);
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return NULL;
    }

    if (!(context = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context)))) return NULL;

    context->format = format;
    if (!create_context(context, NULL))
    {
        HeapFree(GetProcessHeap(), 0, context);
        return NULL;
    }

    return context;
}

/***********************************************************************
 *              macdrv_wglDeleteContext
 */
static void macdrv_wglDeleteContext(struct wgl_context *context)
{
    TRACE("deleting context %p/%p/%p\n", context, context->context, context->cglcontext);
    macdrv_dispose_opengl_context(context->context);
    HeapFree(GetProcessHeap(), 0, context);
}

/***********************************************************************
 *              macdrv_wglGetPixelFormat
 */
static int macdrv_wglGetPixelFormat(HDC hdc)
{
    int format;

    format = get_dc_pixel_format(hdc);

    if (!is_valid_pixel_format(format))  /* not set yet */
        format = 0;
    else if (!is_displayable_pixel_format(format))
    {
        /* Non-displayable formats can't be used with traditional WGL calls.
         * As has been verified on Windows GetPixelFormat doesn't fail but returns pixel format 1. */
        format = 1;
    }

    TRACE(" hdc %p -> %d\n", hdc, format);
    return format;
}

/***********************************************************************
 *              macdrv_wglGetProcAddress
 */
static PROC macdrv_wglGetProcAddress(const char *proc)
{
    void *ret;

    if (!strncmp(proc, "wgl", 3)) return NULL;
    ret = wine_dlsym(opengl_handle, proc, NULL, 0);
    if (ret)
    {
        if (TRACE_ON(wgl))
        {
            Dl_info info;
            if (dladdr(ret, &info))
                TRACE("%s -> %s from %s\n", proc, info.dli_sname, info.dli_fname);
            else
                TRACE("%s -> %p (no library info)\n", proc, ret);
        }
    }
    else
        WARN("failed to find proc %s\n", debugstr_a(proc));
    return ret;
}

/***********************************************************************
 *              macdrv_wglMakeCurrent
 */
static BOOL macdrv_wglMakeCurrent(HDC hdc, struct wgl_context *context)
{
    TRACE("hdc %p context %p/%p/%p\n", hdc, context, (context ? context->context : NULL),
          (context ? context->cglcontext : NULL));

    return macdrv_wglMakeContextCurrentARB(hdc, hdc, context);
}

/**********************************************************************
 *              macdrv_wglSetPixelFormat
 */
static BOOL macdrv_wglSetPixelFormat(HDC hdc, int fmt, const PIXELFORMATDESCRIPTOR *descr)
{
    return set_pixel_format(hdc, fmt, FALSE);
}

/***********************************************************************
 *              macdrv_wglShareLists
 */
static BOOL macdrv_wglShareLists(struct wgl_context *org, struct wgl_context *dest)
{
    macdrv_opengl_context saved_context;
    CGLContextObj saved_cglcontext;

    TRACE("org %p dest %p\n", org, dest);

    /* Sharing of display lists works differently in Mac OpenGL and WGL.  In Mac OpenGL it is done
     * at context creation time but in case of WGL it is done using wglShareLists.
     *
     * The approach is to create a Mac OpenGL context in wglCreateContext / wglCreateContextAttribsARB
     * and when a program requests sharing we recreate the destination context if it hasn't been made
     * current or when it hasn't shared display lists before.
     */

    if (dest->has_been_current)
    {
        WARN("could not share display lists, the destination context has been current already\n");
        return FALSE;
    }
    else if (dest->sharing)
    {
        WARN("could not share display lists because dest has already shared lists before\n");
        return FALSE;
    }

    /* Re-create the Mac context and share display lists */
    saved_context = dest->context;
    saved_cglcontext = dest->cglcontext;
    dest->context = NULL;
    dest->cglcontext = NULL;
    if (!create_context(dest, org->cglcontext))
    {
        dest->context = saved_context;
        dest->cglcontext = saved_cglcontext;
        return FALSE;
    }

    /* Implicitly disposes of saved_cglcontext. */
    macdrv_dispose_opengl_context(saved_context);

    TRACE("re-created OpenGL context %p/%p/%p sharing lists with context %p/%p/%p\n",
          dest, dest->context, dest->cglcontext, org, org->context, org->cglcontext);

    org->sharing = TRUE;
    dest->sharing = TRUE;

    return TRUE;
}

/**********************************************************************
 *              macdrv_wglSwapBuffers
 */
static BOOL macdrv_wglSwapBuffers(HDC hdc)
{
    struct wgl_context *context = NtCurrentTeb()->glContext;

    TRACE("hdc %p context %p/%p/%p\n", hdc, context, (context ? context->context : NULL),
          (context ? context->cglcontext : NULL));

    if (!context)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    macdrv_flush_opengl_context(context->context);
    return TRUE;
}

static struct opengl_funcs opengl_funcs =
{
    {
        macdrv_wglCopyContext,          /* p_wglCopyContext */
        macdrv_wglCreateContext,        /* p_wglCreateContext */
        macdrv_wglDeleteContext,        /* p_wglDeleteContext */
        macdrv_wglDescribePixelFormat,  /* p_wglDescribePixelFormat */
        macdrv_wglGetPixelFormat,       /* p_wglGetPixelFormat */
        macdrv_wglGetProcAddress,       /* p_wglGetProcAddress */
        macdrv_wglMakeCurrent,          /* p_wglMakeCurrent */
        macdrv_wglSetPixelFormat,       /* p_wglSetPixelFormat */
        macdrv_wglShareLists,           /* p_wglShareLists */
        macdrv_wglSwapBuffers,          /* p_wglSwapBuffers */
    }
};

/**********************************************************************
 *              macdrv_wine_get_wgl_driver
 */
struct opengl_funcs *macdrv_wine_get_wgl_driver(PHYSDEV dev, UINT version)
{
    if (version != WINE_WGL_DRIVER_VERSION)
    {
        ERR("version mismatch, opengl32 wants %u but macdrv has %u\n", version, WINE_WGL_DRIVER_VERSION);
        return NULL;
    }

    if (!init_opengl()) return (void *)-1;

    return &opengl_funcs;
}
