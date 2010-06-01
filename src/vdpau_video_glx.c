/*
 *  vdpau_video_glx.c - VDPAU backend for VA API (GLX rendering)
 *
 *  vdpau-video (C) 2009-2010 Splitted-Desktop Systems
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#define _GNU_SOURCE 1 /* RTLD_NEXT */
#include "sysdeps.h"
#include "vdpau_video.h"
#include "vdpau_video_glx.h"
#include "vdpau_video_x11.h"
#include "utils.h"
#include <dlfcn.h>
#include <GL/glext.h>
#include <GL/glxext.h>

#define DEBUG 1
#include "debug.h"

#if GLX_GLXEXT_VERSION < 18
typedef void (*PFNGLXBINDTEXIMAGEEXTPROC)(Display *, GLXDrawable, int, const int *);
typedef void (*PFNGLXRELEASETEXIMAGEEXTPROC)(Display *, GLXDrawable, int);
#endif

#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING GL_FRAMEBUFFER_BINDING_EXT
#endif


// OpenGL VTable
typedef enum {
    OPENGL_STATUS_NONE  = 0,
    OPENGL_STATUS_OK    = 1,
    OPENGL_STATUS_ERROR = -1
} OpenGLStatus;

struct opengl_data {
    OpenGLStatus                        gl_status;
    PFNGLXBINDTEXIMAGEEXTPROC           glx_bind_tex_image;
    PFNGLXRELEASETEXIMAGEEXTPROC        glx_release_tex_image;
    PFNGLGENFRAMEBUFFERSEXTPROC         gl_gen_framebuffers;
    PFNGLDELETEFRAMEBUFFERSEXTPROC      gl_delete_framebuffers;
    PFNGLBINDFRAMEBUFFEREXTPROC         gl_bind_framebuffer;
    PFNGLGENRENDERBUFFERSEXTPROC        gl_gen_renderbuffers;
    PFNGLDELETERENDERBUFFERSEXTPROC     gl_delete_renderbuffers;
    PFNGLBINDRENDERBUFFEREXTPROC        gl_bind_renderbuffer;
    PFNGLRENDERBUFFERSTORAGEEXTPROC     gl_renderbuffer_storage;
    PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC gl_framebuffer_renderbuffer;
    PFNGLFRAMEBUFFERTEXTURE2DEXTPROC    gl_framebuffer_texture_2d;
    PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC  gl_check_framebuffer_status;
};

// Lookup GLX function
typedef void (*GLFuncPtr)(void);
typedef GLFuncPtr (*GLXGetProcAddressProc)(const char *);

static GLFuncPtr get_proc_address_default(const char *name)
{
    return NULL;
}

static GLXGetProcAddressProc get_proc_address_func(void)
{
    GLXGetProcAddressProc get_proc_func;

    dlerror();
    get_proc_func = (GLXGetProcAddressProc)
        dlsym(RTLD_NEXT, "glXGetProcAddress");
    if (dlerror() == NULL)
        return get_proc_func;

    get_proc_func = (GLXGetProcAddressProc)
        dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    if (dlerror() == NULL)
        return get_proc_func;

    return get_proc_address_default;
}

static inline GLFuncPtr get_proc_address(const char *name)
{
    static GLXGetProcAddressProc get_proc_func = NULL;
    if (get_proc_func == NULL)
        get_proc_func = get_proc_address_func();
    return get_proc_func(name);
}

// Returns a string representation of an OpenGL error
static const char *gl_get_error_string(GLenum error)
{
    static const struct {
        GLenum val;
        const char *str;
    }
    gl_errors[] = {
        { GL_NO_ERROR,          "no error" },
        { GL_INVALID_ENUM,      "invalid enumerant" },
        { GL_INVALID_VALUE,     "invalid value" },
        { GL_INVALID_OPERATION, "invalid operation" },
        { GL_STACK_OVERFLOW,    "stack overflow" },
        { GL_STACK_UNDERFLOW,   "stack underflow" },
        { GL_OUT_OF_MEMORY,     "out of memory" },
#ifdef GL_INVALID_FRAMEBUFFER_OPERATION_EXT
        { GL_INVALID_FRAMEBUFFER_OPERATION_EXT, "invalid framebuffer operation" },
#endif
        { ~0, NULL }
    };

    int i;
    for (i = 0; gl_errors[i].str; i++) {
        if (gl_errors[i].val == error)
            return gl_errors[i].str;
    }
    return "unknown";
}

static int gl_do_check_error(int report)
{
    GLenum error;
    int is_error = 0;
#if DEBUG
    while ((error = glGetError()) != GL_NO_ERROR) {
        if (report)
            vdpau_error_message("glError: %s caught\n",
                                gl_get_error_string(error));
        is_error = 1;
    }
#endif
    return is_error;
}

static inline void gl_purge_errors(void)
{
    gl_do_check_error(0);
}

static inline int gl_check_error(void)
{
    return gl_do_check_error(1);
}

// glGetFloatv() wrapper
static int gl_get_current_color(float color[4])
{
    gl_purge_errors();
    glGetFloatv(GL_CURRENT_COLOR, color);
    if (gl_check_error())
        return -1;
    return 0;
}

// glGetIntegerv() wrapper
static int gl_get_param(GLenum param, unsigned int *pval)
{
    GLint val;

    gl_purge_errors();
    glGetIntegerv(param, &val);
    if (gl_check_error())
        return -1;
    if (pval)
        *pval = val;
    return 0;
}

// glGetTexLevelParameteriv() wrapper
static int gl_get_texture_param(GLenum param, unsigned int *pval)
{
    GLint val;

    gl_purge_errors();
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, param, &val);
    if (gl_check_error())
        return -1;
    if (pval)
        *pval = val;
    return 0;
}

// Returns OpenGL data, creating them whenever necessary
static inline opengl_data_t *get_gl_data(vdpau_driver_data_t *driver_data)
{
    opengl_data_t *gl_data = driver_data->gl_data;

    if (!gl_data) {
        gl_data = calloc(1, sizeof(*gl_data));
        if (gl_data)
            driver_data->gl_data = gl_data;
    }
    ASSERT(gl_data);
    return gl_data;
}

// Check for OpenGL extensions (TFP, FBO)
static int check_extensions(vdpau_driver_data_t *driver_data)
{
    VADriverContextP const ctx = driver_data->va_context;
    const char *gl_extensions;
    const char *glx_extensions;

    gl_extensions  = (const char *)glGetString(GL_EXTENSIONS);
    glx_extensions = glXQueryExtensionsString(driver_data->x11_dpy, driver_data->x11_screen);

    if (!find_string("GL_ARB_texture_non_power_of_two", gl_extensions, " "))
        return -1;
    if (!find_string("GLX_EXT_texture_from_pixmap", glx_extensions, " "))
        return -1;
    if (!find_string("GL_ARB_framebuffer_object", gl_extensions, " ") &&
        !find_string("GL_EXT_framebuffer_object", gl_extensions, " "))
        return -1;
    return 0;
}

// Load OpenGL functions that implement TFP and FBO extensions
static int load_extensions(vdpau_driver_data_t *driver_data)
{
    opengl_data_t * const gl_data = get_gl_data(driver_data);

    gl_data->glx_bind_tex_image = (PFNGLXBINDTEXIMAGEEXTPROC)
        get_proc_address("glXBindTexImageEXT");
    if (gl_data->glx_bind_tex_image == NULL)
        return -1;
    gl_data->glx_release_tex_image = (PFNGLXRELEASETEXIMAGEEXTPROC)
        get_proc_address("glXReleaseTexImageEXT");
    if (gl_data->glx_release_tex_image == NULL)
        return -1;
    gl_data->gl_gen_framebuffers = (PFNGLGENFRAMEBUFFERSEXTPROC)
        get_proc_address("glGenFramebuffersEXT");
    if (gl_data->gl_gen_framebuffers == NULL)
        return -1;
    gl_data->gl_delete_framebuffers = (PFNGLDELETEFRAMEBUFFERSEXTPROC)
        get_proc_address("glDeleteFramebuffersEXT");
    if (gl_data->gl_delete_framebuffers == NULL)
        return -1;
    gl_data->gl_bind_framebuffer = (PFNGLBINDFRAMEBUFFEREXTPROC)
        get_proc_address("glBindFramebufferEXT");
    if (gl_data->gl_bind_framebuffer == NULL)
        return -1;
    gl_data->gl_gen_renderbuffers = (PFNGLGENRENDERBUFFERSEXTPROC)
        get_proc_address("glGenRenderbuffersEXT");
    if (gl_data->gl_gen_renderbuffers == NULL)
        return -1;
    gl_data->gl_delete_renderbuffers = (PFNGLDELETERENDERBUFFERSEXTPROC)
        get_proc_address("glDeleteRenderbuffersEXT");
    if (gl_data->gl_delete_renderbuffers == NULL)
        return -1;
    gl_data->gl_bind_renderbuffer = (PFNGLBINDRENDERBUFFEREXTPROC)
        get_proc_address("glBindRenderbufferEXT");
    if (gl_data->gl_bind_renderbuffer == NULL)
        return -1;
    gl_data->gl_renderbuffer_storage = (PFNGLRENDERBUFFERSTORAGEEXTPROC)
        get_proc_address("glRenderbufferStorageEXT");
    if (gl_data->gl_renderbuffer_storage == NULL)
        return -1;
    gl_data->gl_framebuffer_renderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC)
        get_proc_address("glFramebufferRenderbufferEXT");
    if (gl_data->gl_framebuffer_renderbuffer == NULL)
        return -1;
    gl_data->gl_framebuffer_texture_2d = (PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)
        get_proc_address("glFramebufferTexture2DEXT");
    if (gl_data->gl_framebuffer_texture_2d == NULL)
        return -1;
    gl_data->gl_check_framebuffer_status = (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC)
        get_proc_address("glCheckFramebufferStatusEXT");
    if (gl_data->gl_check_framebuffer_status == NULL)
        return -1;
    return 0;
}

// Ensure GLX TFP and FBO extensions are available
static int
ensure_extensions(vdpau_driver_data_t *driver_data)
{
    opengl_data_t * const gl_data = get_gl_data(driver_data);

    if (gl_data->gl_status == OPENGL_STATUS_NONE) {
        gl_data->gl_status = OPENGL_STATUS_ERROR;
        if (check_extensions(driver_data) < 0)
            return -1;
        if (load_extensions(driver_data) < 0)
            return -1;
        gl_data->gl_status = OPENGL_STATUS_OK;
    }

    if (gl_data->gl_status != OPENGL_STATUS_OK)
        return -1;
    return 0;
}

// OpenGL texture state
typedef struct opengl_texture_state {
    int    was_enabled;
    int    was_bound;
    GLenum target;
    GLuint old_texture;
} opengl_texture_state_t;

// Bind texture, preserve previous texture state
static int
bind_texture(opengl_texture_state_t *ts, GLenum target, GLuint texture)
{
    ts->target      = target;
    ts->old_texture = 0;
    ts->was_bound   = 0;
    ts->was_enabled = glIsEnabled(target);
    if (!ts->was_enabled)
        glEnable(target);

    GLenum texture_binding;
    switch (target) {
    case GL_TEXTURE_1D:
        texture_binding = GL_TEXTURE_BINDING_1D;
        break;
    case GL_TEXTURE_2D:
        texture_binding = GL_TEXTURE_BINDING_2D;
        break;
    case GL_TEXTURE_3D:
        texture_binding = GL_TEXTURE_BINDING_3D;
        break;
    case GL_TEXTURE_RECTANGLE_ARB:
        texture_binding = GL_TEXTURE_BINDING_RECTANGLE_ARB;
        break;
    default:
        ASSERT(!target);
        return -1;
    }

    if (ts->was_enabled && gl_get_param(texture_binding, &ts->old_texture) < 0)
        return -1;

    ts->was_bound = texture == ts->old_texture;
    if (!ts->was_bound) {
        gl_purge_errors();
        glBindTexture(target, texture);
        if (gl_check_error())
            return -1;
    }
    return 0;
}

// Unbind texture, restore previous texture state
static void
unbind_texture(opengl_texture_state_t *ts)
{
    if (!ts->was_bound && ts->old_texture)
        glBindTexture(ts->target, ts->old_texture);
    if (!ts->was_enabled)
        glDisable(ts->target);
}

// Create Pixmaps for GLX texture-from-pixmap extension
static int
create_tfp_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    VADriverContextP const ctx        = driver_data->va_context;
    const unsigned int     width      = obj_glx_surface->width;
    const unsigned int     height     = obj_glx_surface->height;
    Pixmap                 pixmap     = None;
    GLXFBConfig           *fbconfig   = NULL;
    GLXPixmap              glx_pixmap = None;
    Window                 root_window;
    XWindowAttributes      wattr;
    int                   *attrib;
    int                    n_fbconfig_attribs, x, y, status;
    unsigned int           border_width, depth, dummy;

    root_window = RootWindow(driver_data->x11_dpy, driver_data->x11_screen);
    XGetWindowAttributes(driver_data->x11_dpy, root_window, &wattr);
    pixmap = XCreatePixmap(driver_data->x11_dpy, root_window,
                           width, height, wattr.depth);
    if (!pixmap)
        return -1;
    obj_glx_surface->pixmap = pixmap;

    x11_trap_errors();
    status = XGetGeometry(driver_data->x11_dpy,
                          (Drawable)pixmap,
                          &root_window,
                          &x,
                          &y,
                          &dummy,
                          &dummy,
                          &border_width,
                          &depth);
    if (x11_untrap_errors() != 0 || status == 0)
        return -1;
    if (depth != 24 && depth != 32)
        return -1;

    int fbconfig_attribs[32] = {
        GLX_DRAWABLE_TYPE,      GLX_PIXMAP_BIT,
        GLX_DOUBLEBUFFER,       GL_TRUE,
        GLX_RENDER_TYPE,        GLX_RGBA_BIT,
        GLX_X_RENDERABLE,       GL_TRUE,
        GLX_Y_INVERTED_EXT,     GL_TRUE,
        GLX_RED_SIZE,           8,
        GLX_GREEN_SIZE,         8,
        GLX_BLUE_SIZE,          8,
        GL_NONE,
    };
    for (attrib = fbconfig_attribs; *attrib != GL_NONE; attrib += 2)
        ;
    *attrib++ = GLX_DEPTH_SIZE;                         *attrib++ = depth;
    if (depth == 32) {
        *attrib++ = GLX_ALPHA_SIZE;                     *attrib++ = 8;
        *attrib++ = GLX_BIND_TO_TEXTURE_RGBA_EXT;       *attrib++ = GL_TRUE;
    }
    else {
        *attrib++ = GLX_BIND_TO_TEXTURE_RGB_EXT;        *attrib++ = GL_TRUE;
    }
    *attrib++ = GL_NONE;

    fbconfig = glXChooseFBConfig(driver_data->x11_dpy, driver_data->x11_screen, fbconfig_attribs, &n_fbconfig_attribs);
    if (fbconfig == NULL)
        return -1;

    int pixmap_attribs[10] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_MIPMAP_TEXTURE_EXT, GL_FALSE,
        GL_NONE,
    };
    for (attrib = pixmap_attribs; *attrib != GL_NONE; attrib += 2)
        ;
    *attrib++ = GLX_TEXTURE_FORMAT_EXT;
    if (depth == 32)
        *attrib++ = GLX_TEXTURE_FORMAT_RGBA_EXT;
    else
        *attrib++ = GLX_TEXTURE_FORMAT_RGB_EXT;
    *attrib++ = GL_NONE;

    x11_trap_errors();
    glx_pixmap = glXCreatePixmap(
        driver_data->x11_dpy,
        fbconfig[0],
        pixmap,
        pixmap_attribs
    );
    free(fbconfig);
    if (x11_untrap_errors() != 0)
        return -1;
    obj_glx_surface->glx_pixmap = glx_pixmap;

    glGenTextures(1, &obj_glx_surface->pix_texture);
    glBindTexture(GL_TEXTURE_2D, obj_glx_surface->pix_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return 0;
}

// Destroy Pixmaps used for TFP
static void
destroy_tfp_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    VADriverContextP const ctx = driver_data->va_context;

    if (obj_glx_surface->glx_pixmap) {
        glXDestroyPixmap(driver_data->x11_dpy, obj_glx_surface->glx_pixmap);
        obj_glx_surface->glx_pixmap = None;
    }

    if (obj_glx_surface->pixmap) {
        XFreePixmap(driver_data->x11_dpy, obj_glx_surface->pixmap);
        obj_glx_surface->pixmap = None;
    }
}

// Bind GLX Pixmap to texture
static int
bind_pixmap(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    VADriverContextP const ctx = driver_data->va_context;
    opengl_data_t * const gl_data = get_gl_data(driver_data);

    if (obj_glx_surface->is_bound)
        return 0;

    glBindTexture(GL_TEXTURE_2D, obj_glx_surface->pix_texture);

    x11_trap_errors();
    gl_data->glx_bind_tex_image(
        driver_data->x11_dpy,
        obj_glx_surface->glx_pixmap,
        GLX_FRONT_LEFT_EXT,
        NULL
    );
    XSync(driver_data->x11_dpy, False);
    if (x11_untrap_errors() != 0) {
        vdpau_error_message("failed to bind pixmap\n");
        return -1;
    }

    obj_glx_surface->is_bound = 1;
    return 0;
}

// Release GLX Pixmap from texture
static int
unbind_pixmap(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    VADriverContextP const ctx = driver_data->va_context;
    opengl_data_t * const gl_data = get_gl_data(driver_data);

    if (!obj_glx_surface->is_bound)
        return 0;

    x11_trap_errors();
    gl_data->glx_release_tex_image(
        driver_data->x11_dpy,
        obj_glx_surface->glx_pixmap,
        GLX_FRONT_LEFT_EXT
    );
    XSync(driver_data->x11_dpy, False);
    if (x11_untrap_errors() != 0) {
        vdpau_error_message("failed to release pixmap\n");
        return -1;
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    obj_glx_surface->is_bound = 0;
    return 0;
}

// Render GLX Pixmap to texture
static void
render_pixmap(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    const unsigned int w = obj_glx_surface->width;
    const unsigned int h = obj_glx_surface->height;
    float old_color[4];

    gl_get_current_color(old_color);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    {
        glTexCoord2f(0.0f, 0.0f); glVertex2i(0, 0);
        glTexCoord2f(0.0f, 1.0f); glVertex2i(0, h);
        glTexCoord2f(1.0f, 1.0f); glVertex2i(w, h);
        glTexCoord2f(1.0f, 0.0f); glVertex2i(w, 0);
    }
    glEnd();
    glColor4fv(old_color);
}

// Create offscreen surface
static int
create_fbo_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    opengl_data_t * const gl_data = get_gl_data(driver_data);
    GLuint fbo;
    GLenum status;

    gl_data->gl_gen_framebuffers(1, &fbo);
    gl_data->gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, fbo);
    gl_data->gl_framebuffer_texture_2d(
        GL_FRAMEBUFFER_EXT,
        GL_COLOR_ATTACHMENT0_EXT,
        GL_TEXTURE_2D, obj_glx_surface->texture,
        0
    );

    status = gl_data->gl_check_framebuffer_status(GL_DRAW_FRAMEBUFFER_EXT);
    gl_data->gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
        return -1;

    obj_glx_surface->fbo = fbo;
    return 0;
}

// Destroy offscreen surface
static void
destroy_fbo_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    opengl_data_t * const gl_data = get_gl_data(driver_data);

    if (obj_glx_surface->fbo) {
        gl_data->gl_delete_framebuffers(1, &obj_glx_surface->fbo);
        obj_glx_surface->fbo = 0;
    }
}

// Setup matrices to match the FBO texture dimensions
static void
fbo_enter(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    opengl_data_t * const gl_data = get_gl_data(driver_data);
    const unsigned int width  = obj_glx_surface->width;
    const unsigned int height = obj_glx_surface->height;

    gl_data->gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, obj_glx_surface->fbo);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glViewport(0, 0, width, height);
    glTranslatef(-1.0f, -1.0f, 0.0f);
    glScalef(2.0f / width, 2.0f / height, 1.0f);
}

// Restore original OpenGL matrices
static void
fbo_leave(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    opengl_data_t * const gl_data = get_gl_data(driver_data);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    gl_data->gl_bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
}

// Destroy VA/GLX surface
static void
destroy_surface(vdpau_driver_data_t *driver_data, VASurfaceID surface)
{
    object_glx_surface_p obj_glx_surface = VDPAU_GLX_SURFACE(surface);

    unbind_pixmap(driver_data, obj_glx_surface);
    destroy_fbo_surface(driver_data, obj_glx_surface);
    destroy_tfp_surface(driver_data, obj_glx_surface);

    object_heap_free(&driver_data->glx_surface_heap,
                     (object_base_p)obj_glx_surface);
}

// Check internal texture format is supported
static int
is_supported_internal_format(GLenum format)
{
    /* XXX: we don't support other textures than RGBA */
    switch (format) {
    case 4:
    case GL_RGBA:
    case GL_RGBA8:
        return 1;
    }
    return 0;
}

// Create VA/GLX surface
static VASurfaceID
create_surface(vdpau_driver_data_t *driver_data, GLenum target, GLuint texture)
{
    VASurfaceID surface = VA_INVALID_SURFACE;
    object_glx_surface_p obj_glx_surface;
    opengl_texture_state_t ts;
    unsigned int internal_format, border_width, width, height;
    int is_error = 1;

    if (bind_texture(&ts, target, texture) < 0)
        goto end;

    surface = object_heap_allocate(&driver_data->glx_surface_heap);
    if (surface == VA_INVALID_SURFACE)
        goto end;

    obj_glx_surface = VDPAU_GLX_SURFACE(surface);
    if (!obj_glx_surface)
        goto end;

    obj_glx_surface->gl_context         = NULL;
    obj_glx_surface->target             = target;
    obj_glx_surface->texture            = texture;
    obj_glx_surface->va_surface         = VA_INVALID_SURFACE;
    obj_glx_surface->is_bound           = 0;
    obj_glx_surface->pixmap             = None;
    obj_glx_surface->glx_pixmap         = None;
    obj_glx_surface->fbo                = 0;

    if (gl_get_texture_param(GL_TEXTURE_INTERNAL_FORMAT, &internal_format) < 0)
        goto end;
    if (!is_supported_internal_format(internal_format))
        goto end;

    /* Check texture dimensions */
    if (gl_get_texture_param(GL_TEXTURE_BORDER, &border_width) < 0)
        goto end;
    if (gl_get_texture_param(GL_TEXTURE_WIDTH, &width) < 0)
        goto end;
    if (gl_get_texture_param(GL_TEXTURE_HEIGHT, &height) < 0)
        goto end;

    width  -= 2 * border_width;
    height -= 2 * border_width;
    if (width == 0 || height == 0)
        goto end;

    obj_glx_surface->width  = width;
    obj_glx_surface->height = height;

    /* Create Pixmaps for TFP */
    if (create_tfp_surface(driver_data, obj_glx_surface) < 0)
        goto end;

    is_error = 0;
end:
    unbind_texture(&ts);
    if (is_error && surface != VA_INVALID_SURFACE) {
        destroy_surface(driver_data, surface);
        surface = VA_INVALID_SURFACE;
    }
    return surface;
}

struct _GLContextState {
    Display            *display;
    Window              window;
    GLXContext          context;
};

static void
gl_destroy_context(GLContextState *cs)
{
    if (!cs)
        return;

    if (cs->display && cs->context) {
        glXDestroyContext(cs->display, cs->context);
        cs->display = NULL;
        cs->context = NULL;
    }
    free(cs);
}

static GLContextState *
gl_create_context(Display *dpy, int screen, GLContextState *parent)
{
    GLContextState *cs;
    GLXFBConfig *fbconfigs = NULL;
    int fbconfig_id, val, n, n_fbconfigs;
    Status status;

    static GLint fbconfig_attrs[] = {
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER,  True,
        GLX_RED_SIZE,      8,
        GLX_GREEN_SIZE,    8, 
        GLX_BLUE_SIZE,     8,
        None
    };

    cs = malloc(sizeof(*cs));
    if (!cs)
        goto error;

    cs->display = dpy;
    cs->window  = parent ? parent->window : None;
    cs->context = NULL;

    if (parent && parent->context) {
        status = glXQueryContext(
            parent->display,
            parent->context,
            GLX_FBCONFIG_ID, &fbconfig_id
        );
        if (status != Success)
            goto error;

        fbconfigs = glXGetFBConfigs(dpy, screen, &n_fbconfigs);
        if (!fbconfigs)
            goto error;

        /* Find out a GLXFBConfig compatible with the parent context */
        for (n = 0; n < n_fbconfigs; n++) {
            status = glXGetFBConfigAttrib(
                dpy,
                fbconfigs[n],
                GLX_FBCONFIG_ID, &val
            );
            if (status == Success && val == fbconfig_id)
                break;
        }
        if (n == n_fbconfigs)
            goto error;
    }
    else {
        fbconfigs = glXChooseFBConfig(dpy, screen, fbconfig_attrs, &n_fbconfigs);
        if (!fbconfigs)
            goto error;

        /* Select the first one */
        n = 0;
    }

    cs->context = glXCreateNewContext(
        dpy,
        fbconfigs[n],
        GLX_RGBA_TYPE,
        parent ? parent->context : NULL,
        True
    );
    if (cs->context)
        goto end;

error:
    gl_destroy_context(cs);
    cs = NULL;
end:
    if (fbconfigs)
        XFree(fbconfigs);
    return cs;
}

static void
gl_get_current_context(GLContextState *cs)
{
    cs->display = glXGetCurrentDisplay();
    cs->window  = glXGetCurrentDrawable();
    cs->context = glXGetCurrentContext();
}

static int
gl_set_current_context(GLContextState *new_cs, GLContextState *old_cs)
{
    /* If display is NULL, this could be that new_cs was retrieved from
       gl_get_current_context() with none set previously. If that case,
       the other fields are also NULL and we don't return an error */
    if (!new_cs->display)
        return !new_cs->window && !new_cs->context;

    if (old_cs) {
        if (old_cs == new_cs)
            return 1;
        gl_get_current_context(old_cs);
        if (old_cs->display == new_cs->display &&
            old_cs->window  == new_cs->window  &&
            old_cs->context == new_cs->context)
            return 1;
    }
    return glXMakeCurrent(new_cs->display, new_cs->window, new_cs->context);
}

// vaCreateSurfaceGLX
VAStatus
vdpau_CreateSurfaceGLX(
    VADriverContextP    ctx,
    unsigned int        target,
    unsigned int        texture,
    void              **gl_surface
)
{
    VDPAU_DRIVER_DATA_INIT;

    if (!gl_surface)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Make sure we have the necessary GLX extensions */
    if (ensure_extensions(driver_data) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    /* Make sure it is a valid GL texture object */
    /* XXX: we only support 2D textures */
    if (target != GL_TEXTURE_2D)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!glIsTexture(texture))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    GLContextState old_cs, *new_cs;
    gl_get_current_context(&old_cs);
    new_cs = gl_create_context(driver_data->x11_dpy, driver_data->x11_screen, &old_cs);
    if (!new_cs)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (!gl_set_current_context(new_cs, NULL))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    VASurfaceID surface = create_surface(driver_data, target, texture);
    if (surface == VA_INVALID_SURFACE)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    object_glx_surface_p obj_glx_surface = VDPAU_GLX_SURFACE(surface);
    *gl_surface = obj_glx_surface;
    obj_glx_surface->gl_context = new_cs;

    gl_set_current_context(&old_cs, NULL);
    return VA_STATUS_SUCCESS;
}

// vaDestroySurfaceGLX
VAStatus
vdpau_DestroySurfaceGLX(
    VADriverContextP ctx,
    void            *gl_surface
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_glx_surface_p obj_glx_surface = gl_surface;
    if (!obj_glx_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    GLContextState old_cs, *new_cs = obj_glx_surface->gl_context;
    if (!gl_set_current_context(new_cs, &old_cs))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    destroy_surface(driver_data, obj_glx_surface->base.id);

    gl_destroy_context(new_cs);
    gl_set_current_context(&old_cs, NULL);
    return VA_STATUS_SUCCESS;
}

// Forward declarations
static VAStatus
deassociate_glx_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
);

// vaAssociateSurfaceGLX
static VAStatus
associate_glx_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface,
    object_surface_p     obj_surface,
    unsigned int         flags
)
{
    /* XXX: optimise case where we are associating the same VA surface
       as before an no changed occurred to it */
    VAStatus va_status;
    va_status = deassociate_glx_surface(driver_data, obj_glx_surface);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    /* Render to Pixmap */
    VARectangle src_rect, dst_rect;
    src_rect.x      = 0;
    src_rect.y      = 0;
    src_rect.width  = obj_surface->width;
    src_rect.height = obj_surface->height;
    dst_rect.x      = 0;
    dst_rect.y      = 0;
    dst_rect.width  = obj_glx_surface->width;
    dst_rect.height = obj_glx_surface->height;
    va_status = put_surface(
        driver_data,
        obj_surface->base.id,
        obj_glx_surface->pixmap,
        obj_glx_surface->width,
        obj_glx_surface->height,
        &src_rect,
        &dst_rect,
        flags | VA_CLEAR_DRAWABLE
    );
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    /* Force rendering of fields now */
    if ((flags ^ (VA_TOP_FIELD|VA_BOTTOM_FIELD)) != 0) {
        object_output_p obj_output;
        obj_output = output_surface_lookup(
            obj_surface,
            obj_glx_surface->pixmap
        );
        ASSERT(obj_output);
        if (obj_output && obj_output->fields) {
            va_status = queue_surface(driver_data, obj_surface, obj_output);
            if (va_status != VA_STATUS_SUCCESS)
                return va_status;
        }
    }

    obj_glx_surface->va_surface = obj_surface->base.id;
    return VA_STATUS_SUCCESS;
}

VAStatus
vdpau_AssociateSurfaceGLX(
    VADriverContextP ctx,
    void            *gl_surface,
    VASurfaceID      surface,
    unsigned int     flags
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_glx_surface_p obj_glx_surface = gl_surface;
    if (!obj_glx_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_surface_p obj_surface = VDPAU_SURFACE(surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    GLContextState old_cs;
    if (!gl_set_current_context(obj_glx_surface->gl_context, &old_cs))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    VAStatus va_status;
    va_status = associate_glx_surface(
        driver_data,
        obj_glx_surface,
        obj_surface,
        flags
    );

    gl_set_current_context(&old_cs, NULL);
    return va_status;
}

// vaDeassociateSurfaceGLX
static VAStatus
deassociate_glx_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    if (unbind_pixmap(driver_data, obj_glx_surface) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    obj_glx_surface->va_surface = VA_INVALID_SURFACE;
    return VA_STATUS_SUCCESS;
}

VAStatus
vdpau_DeassociateSurfaceGLX(
    VADriverContextP ctx,
    void            *gl_surface
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_glx_surface_p obj_glx_surface = gl_surface;
    if (!obj_glx_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    GLContextState old_cs;
    if (!gl_set_current_context(obj_glx_surface->gl_context, &old_cs))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    VAStatus va_status;
    va_status = deassociate_glx_surface(driver_data, obj_glx_surface);

    gl_set_current_context(&old_cs, NULL);
    return va_status;
}

// vaSyncSurfaceGLX
static inline VAStatus
sync_glx_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    object_surface_p obj_surface = VDPAU_SURFACE(obj_glx_surface->va_surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    return sync_surface(driver_data, obj_surface);
}

VAStatus
vdpau_SyncSurfaceGLX(
    VADriverContextP ctx,
    void            *gl_surface
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_glx_surface_p obj_glx_surface = gl_surface;
    if (!obj_glx_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    GLContextState old_cs;
    if (!gl_set_current_context(obj_glx_surface->gl_context, &old_cs))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    VAStatus va_status;
    va_status = sync_glx_surface(driver_data, obj_glx_surface);

    gl_set_current_context(&old_cs, NULL);
    return va_status;
}

// vaBeginRenderSurfaceGLX
static inline VAStatus
begin_render_glx_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    VAStatus va_status = sync_glx_surface(driver_data, obj_glx_surface);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    if (bind_pixmap(driver_data, obj_glx_surface) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    return VA_STATUS_SUCCESS;
}

VAStatus
vdpau_BeginRenderSurfaceGLX(
    VADriverContextP ctx,
    void            *gl_surface
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_glx_surface_p obj_glx_surface = gl_surface;
    if (!obj_glx_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    GLContextState old_cs;
    if (!gl_set_current_context(obj_glx_surface->gl_context, &old_cs))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    VAStatus va_status;
    va_status = begin_render_glx_surface(driver_data, obj_glx_surface);

    gl_set_current_context(&old_cs, NULL);
    return va_status;
}

// vaEndRenderSurfaceGLX
static inline VAStatus
end_render_glx_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface
)
{
    if (unbind_pixmap(driver_data, obj_glx_surface) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    return VA_STATUS_SUCCESS;
}

VAStatus
vdpau_EndRenderSurfaceGLX(
    VADriverContextP ctx,
    void            *gl_surface
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_glx_surface_p obj_glx_surface = gl_surface;
    if (!obj_glx_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    GLContextState old_cs;
    if (!gl_set_current_context(obj_glx_surface->gl_context, &old_cs))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    VAStatus va_status;
    va_status = end_render_glx_surface(driver_data, obj_glx_surface);

    gl_set_current_context(&old_cs, NULL);
    return va_status;
}

// vaCopySurfaceGLX
static VAStatus
copy_glx_surface(
    vdpau_driver_data_t *driver_data,
    object_glx_surface_p obj_glx_surface,
    object_surface_p     obj_surface,
    unsigned int         flags
)
{
    /* Create framebuffer surface */
    if (obj_glx_surface->fbo == 0) {
        if (create_fbo_surface(driver_data, obj_glx_surface) < 0)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    ASSERT(obj_glx_surface->fbo > 0);

    /* Associate VA surface */
    VAStatus va_status;
    va_status = associate_glx_surface(
        driver_data,
        obj_glx_surface,
        obj_surface,
        flags
    );
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    /* Make sure binding succeeds, if texture was not already bound */
    opengl_texture_state_t ts;
    if (bind_texture(&ts, obj_glx_surface->target, obj_glx_surface->texture) < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    /* Render to FBO */
    fbo_enter(driver_data, obj_glx_surface);
    va_status = begin_render_glx_surface(driver_data, obj_glx_surface);
    if (va_status == VA_STATUS_SUCCESS) {
        render_pixmap(driver_data, obj_glx_surface);
        va_status = end_render_glx_surface(driver_data, obj_glx_surface);
    }
    fbo_leave(driver_data, obj_glx_surface);
    unbind_texture(&ts);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    va_status = deassociate_glx_surface(driver_data, obj_glx_surface);
    if (va_status != VA_STATUS_SUCCESS)
        return va_status;

    return VA_STATUS_SUCCESS;
}

VAStatus
vdpau_CopySurfaceGLX(
    VADriverContextP ctx,
    void            *gl_surface,
    VASurfaceID      surface,
    unsigned int     flags
)
{
    VDPAU_DRIVER_DATA_INIT;

    object_glx_surface_p obj_glx_surface = gl_surface;
    if (!obj_glx_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    object_surface_p obj_surface = VDPAU_SURFACE(surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    GLContextState old_cs;
    if (!gl_set_current_context(obj_glx_surface->gl_context, &old_cs))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    VAStatus va_status;
    va_status = copy_glx_surface(
        driver_data,
        obj_glx_surface,
        obj_surface,
        flags
    );

    gl_set_current_context(&old_cs, NULL);
    return va_status;
}
