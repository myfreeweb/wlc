#include "wlc_internal.h"
#include "gles2.h"
#include "render.h"

#include "context/egl.h"
#include "context/context.h"
#include "compositor/view.h"
#include "compositor/surface.h"
#include "compositor/buffer.h"
#include "compositor/output.h"
#include "shell/xdg-surface.h"
#include "xwayland/xwm.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <dlfcn.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <wayland-server.h>

static GLubyte cursor_palette[];

enum program_type {
   PROGRAM_RGB,
   PROGRAM_RGBA,
   PROGRAM_CURSOR,
   PROGRAM_LAST,
};

enum {
   UNIFORM_WIDTH,
   UNIFORM_HEIGHT,
   UNIFORM_DIM,
   UNIFORM_LAST,
};

enum {
   TEXTURE_BLACK,
   TEXTURE_CURSOR,
   TEXTURE_LAST
};

static const char *uniform_names[UNIFORM_LAST] = {
   "width",
   "height",
   "dim"
};

struct ctx {
   struct wlc_context *context;
   const char *extensions;

   struct ctx_program *program;

   struct ctx_program {
      GLuint obj;
      GLuint uniforms[UNIFORM_LAST];
   } programs[PROGRAM_LAST];

   struct wlc_size resolution;

   GLuint textures[TEXTURE_LAST];

   struct {
      // EGL surfaces
      PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
   } api;
};

struct paint {
   struct wlc_geometry visible;
   float dim;
   enum program_type program;
   bool filter;
};

static struct {
   struct {
      void *handle;

      GLenum (*glGetError)(void);
      const GLubyte* (*glGetString)(GLenum);
      void (*glEnable)(GLenum);
      void (*glClear)(GLbitfield);
      void (*glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
      void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
      void (*glBlendFunc)(GLenum, GLenum);
      GLuint (*glCreateShader)(GLenum);
      void (*glShaderSource)(GLuint, GLsizei count, const GLchar **string, const GLint *length);
      void (*glCompileShader)(GLuint);
      void (*glGetShaderiv)(GLuint, GLenum, GLint*);
      void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
      GLuint (*glCreateProgram)(void);
      void (*glAttachShader)(GLuint, GLuint);
      void (*glLinkProgram)(GLuint);
      void (*glUseProgram)(GLuint);
      void (*glGetProgramiv)(GLuint, GLenum, GLint*);
      void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
      void (*glBindAttribLocation)(GLuint, GLuint, const GLchar*);
      GLint (*glGetUniformLocation)(GLuint, const GLchar *name);
      void (*glUniform1f)(GLint, GLfloat);
      void (*glEnableVertexAttribArray)(GLuint);
      void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*);
      void (*glDrawArrays)(GLenum, GLint, GLsizei);
      void (*glGenTextures)(GLsizei, GLuint*);
      void (*glDeleteTextures)(GLsizei, GLuint*);
      void (*glBindTexture)(GLenum, GLuint);
      void (*glActiveTexture)(GLenum);
      void (*glTexParameteri)(GLenum, GLenum, GLenum);
      void (*glPixelStorei)(GLenum, GLint);
      void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
      void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);

      PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
   } api;
} gl;

static bool
gles2_load(void)
{
   const char *lib = "libGLESv2.so", *func = NULL;

   if (!(gl.api.handle = dlopen(lib, RTLD_LAZY))) {
      wlc_log(WLC_LOG_WARN, "%s", dlerror());
      return false;
   }

#define load(x) (gl.api.x = dlsym(gl.api.handle, (func = #x)))

   if (!(load(glGetError)))
      goto function_pointer_exception;
   if (!load(glGetString))
      goto function_pointer_exception;
   if (!load(glEnable))
      goto function_pointer_exception;
   if (!load(glClear))
      goto function_pointer_exception;
   if (!load(glClearColor))
      goto function_pointer_exception;
   if (!load(glViewport))
      goto function_pointer_exception;
   if (!load(glBlendFunc))
      goto function_pointer_exception;
   if (!(load(glCreateShader)))
      goto function_pointer_exception;
   if (!(load(glShaderSource)))
      goto function_pointer_exception;
   if (!(load(glCompileShader)))
      goto function_pointer_exception;
   if (!(load(glGetShaderiv)))
      goto function_pointer_exception;
   if (!(load(glGetShaderInfoLog)))
      goto function_pointer_exception;
   if (!(load(glCreateProgram)))
      goto function_pointer_exception;
   if (!(load(glAttachShader)))
      goto function_pointer_exception;
   if (!(load(glLinkProgram)))
      goto function_pointer_exception;
   if (!(load(glUseProgram)))
      goto function_pointer_exception;
   if (!(load(glGetProgramiv)))
      goto function_pointer_exception;
   if (!(load(glGetProgramInfoLog)))
      goto function_pointer_exception;
   if (!(load(glEnableVertexAttribArray)))
      goto function_pointer_exception;
   if (!(load(glBindAttribLocation)))
      goto function_pointer_exception;
   if (!(load(glGetUniformLocation)))
      goto function_pointer_exception;
   if (!(load(glUniform1f)))
      goto function_pointer_exception;
   if (!(load(glVertexAttribPointer)))
      goto function_pointer_exception;
   if (!(load(glDrawArrays)))
      goto function_pointer_exception;
   if (!(load(glGenTextures)))
      goto function_pointer_exception;
   if (!(load(glDeleteTextures)))
      goto function_pointer_exception;
   if (!(load(glBindTexture)))
      goto function_pointer_exception;
   if (!(load(glActiveTexture)))
      goto function_pointer_exception;
   if (!(load(glTexParameteri)))
      goto function_pointer_exception;
   if (!(load(glPixelStorei)))
      goto function_pointer_exception;
   if (!(load(glTexImage2D)))
      goto function_pointer_exception;
   if (!(load(glReadPixels)))
      goto function_pointer_exception;

   // Needed for EGL hw surfaces
   load(glEGLImageTargetTexture2DOES);

#undef load

   return true;

function_pointer_exception:
   wlc_log(WLC_LOG_WARN, "Could not load function '%s' from '%s'", func, lib);
   return false;
}

static const char*
gl_error_string(const GLenum error)
{
    switch (error) {
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";
    }

    return "UNKNOWN GL ERROR";
}

static
void gl_call(const char *func, uint32_t line, const char *glfunc)
{
   GLenum error;
   if ((error = gl.api.glGetError()) == GL_NO_ERROR)
      return;

   wlc_log(WLC_LOG_ERROR, "gles2: function %s at line %u: %s == %s", func, line, glfunc, gl_error_string(error));
}

#ifndef __STRING
#  define __STRING(x) #x
#endif

#define GL_CALL(x) x; gl_call(__PRETTY_FUNCTION__, __LINE__, __STRING(x))

static bool
has_extension(const struct ctx *context, const char *extension)
{
   assert(context && extension);

   if (!context->extensions)
      return false;

   size_t len = strlen(extension), pos;
   const char *s = context->extensions;
   while ((pos = strcspn(s, " ")) != 0) {
      size_t next = pos + (s[pos] != 0);

      if (!strncmp(s, extension, len))
         return true;

      s += next;
   }

   return false;
}

static void
set_program(struct ctx *context, const enum program_type type)
{
   assert(context);

   if (&context->programs[type] == context->program)
      return;

   context->program = &context->programs[type];
   GL_CALL(gl.api.glUseProgram(context->program->obj));
}

static GLuint
create_shader(const char *source, GLenum shader_type)
{
   assert(source);

   GLuint shader = gl.api.glCreateShader(shader_type);
   assert(shader != 0);

   GL_CALL(gl.api.glShaderSource(shader, 1, (const char **)&source, NULL));
   GL_CALL(gl.api.glCompileShader(shader));

   GLint status;
   GL_CALL(gl.api.glGetShaderiv(shader, GL_COMPILE_STATUS, &status));
   if (!status) {
      GLsizei len;
      char log[1024];
      GL_CALL(gl.api.glGetShaderInfoLog(shader, sizeof(log), &len, log));
      wlc_log(WLC_LOG_ERROR, "Compiling %s: %*s\n", (shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment"), len, log);
      abort();
   }

   return shader;
}

static struct ctx*
create_context(void)
{
   static const char *vert_shader_text =
      "precision mediump float;\n"
      "uniform float width;\n"
      "uniform float height;\n"
      "mat4 ortho = mat4("
      "  2.0/width,  0,       0, 0,"
      "     0,   -2.0/height, 0, 0,"
      "     0,       0,      -1, 0,"
      "    -1,       1,       0, 1"
      ");\n"
      "attribute vec4 pos;\n"
      "attribute vec2 uv;\n"
      "varying vec2 v_uv;\n"
      "void main() {\n"
      "  gl_Position = ortho * pos;\n"
      "  v_uv = uv;\n"
      "}\n";

   // TODO: Implement different shaders for different textures
   static const char *frag_shader_cursor_text =
      "precision highp float;\n"
      "uniform sampler2D texture0;\n"
      "varying vec2 v_uv;\n"
      "void main() {\n"
      "  vec4 palette[3];\n"
      "  palette[0] = vec4(0.0, 0.0, 0.0, 1.0);\n"
      "  palette[1] = vec4(1.0, 1.0, 1.0, 1.0);\n"
      "  palette[2] = vec4(0.0, 0.0, 0.0, 0.0);\n"
      "  gl_FragColor = palette[int(texture2D(texture0, v_uv).r * 256.0)];\n"
      "}\n";

   static const char *frag_shader_rgb_text =
      "precision mediump float;\n"
      "uniform sampler2D texture0;\n"
      "uniform float dim;\n"
      "varying vec2 v_uv;\n"
      "void main() {\n"
      "  gl_FragColor = vec4(texture2D(texture0, v_uv).rgb * dim, 1.0);\n"
      "}\n";

   static const char *frag_shader_rgba_text =
      "precision mediump float;\n"
      "uniform sampler2D texture0;\n"
      "uniform float dim;\n"
      "varying vec2 v_uv;\n"
      "void main() {\n"
      "  vec4 col = texture2D(texture0, v_uv);\n"
      "  gl_FragColor = vec4(col.rgb * dim, col.a);\n"
      "}\n";

   const struct {
      const char *vert;
      const char *frag;
   } map[PROGRAM_LAST] = {
      { vert_shader_text, frag_shader_rgb_text }, // PROGRAM_RGB
      { vert_shader_text, frag_shader_rgba_text }, // PROGRAM_RGBA
      { vert_shader_text, frag_shader_cursor_text }, // PROGRAM_CURSOR
   };

   struct ctx *context;
   if (!(context = calloc(1, sizeof(struct ctx))))
      return NULL;

   context->extensions = (const char*)GL_CALL(gl.api.glGetString(GL_EXTENSIONS));

   for (int i = 0; i < PROGRAM_LAST; ++i) {
      GLuint vert = create_shader(map[i].vert, GL_VERTEX_SHADER);
      GLuint frag = create_shader(map[i].frag, GL_FRAGMENT_SHADER);
      context->programs[i].obj = gl.api.glCreateProgram();
      GL_CALL(gl.api.glAttachShader(context->programs[i].obj, vert));
      GL_CALL(gl.api.glAttachShader(context->programs[i].obj, frag));
      GL_CALL(gl.api.glLinkProgram(context->programs[i].obj));

      GLint status;
      GL_CALL(gl.api.glGetProgramiv(context->programs[i].obj, GL_LINK_STATUS, &status));
      if (!status) {
         GLsizei len;
         char log[1024];
         GL_CALL(gl.api.glGetProgramInfoLog(context->programs[i].obj, sizeof(log), &len, log));
         wlc_log(WLC_LOG_ERROR, "Linking:\n%*s\n", len, log);
         abort();
      }

      set_program(context, i);
      GL_CALL(gl.api.glBindAttribLocation(context->programs[i].obj, 0, "pos"));
      GL_CALL(gl.api.glBindAttribLocation(context->programs[i].obj, 1, "uv"));

      for (int u = 0; u < UNIFORM_LAST; ++u) {
         context->programs[i].uniforms[u] = GL_CALL(gl.api.glGetUniformLocation(context->programs[i].obj, uniform_names[u]));
      }
   }

   if (has_extension(context, "GL_OES_EGL_image_external"))
      context->api.glEGLImageTargetTexture2DOES = gl.api.glEGLImageTargetTexture2DOES;

   struct {
      GLenum format;
      GLuint w, h;
      GLenum type;
      const void *data;
   } images[TEXTURE_LAST] = {
      { GL_LUMINANCE, 1, 1, GL_UNSIGNED_BYTE, (GLubyte[]){ 0 } }, // TEXTURE_BLACK
      { GL_LUMINANCE, 14, 14, GL_UNSIGNED_BYTE, cursor_palette }, // TEXTURE_CURSOR
   };

   GL_CALL(gl.api.glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
   GL_CALL(gl.api.glGenTextures(TEXTURE_LAST, context->textures));

   for (uint32_t i = 0; i < TEXTURE_LAST; ++i) {
      GL_CALL(gl.api.glBindTexture(GL_TEXTURE_2D, context->textures[i]));
      GL_CALL(gl.api.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
      GL_CALL(gl.api.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
      GL_CALL(gl.api.glTexImage2D(GL_TEXTURE_2D, 0, images[i].format, images[i].w, images[i].h, 0, images[i].format, images[i].type, images[i].data));
   }

   GL_CALL(gl.api.glEnableVertexAttribArray(0));
   GL_CALL(gl.api.glEnableVertexAttribArray(1));

   GL_CALL(gl.api.glEnable(GL_BLEND));
   GL_CALL(gl.api.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
   GL_CALL(gl.api.glClearColor(0.2, 0.2, 0.2, 1));
   return context;
}

static bool
bind(struct ctx *context, struct wlc_output *output)
{
   assert(context && output);

   if (!wlc_context_bind(output->context))
      return false;

   if (!wlc_size_equals(&context->resolution, &output->resolution)) {
      for (int i = 0; i < PROGRAM_LAST; ++i) {
         set_program(context, i);
         GL_CALL(gl.api.glUniform1f(context->program->uniforms[UNIFORM_WIDTH], output->resolution.w));
         GL_CALL(gl.api.glUniform1f(context->program->uniforms[UNIFORM_HEIGHT], output->resolution.h));
      }

      GL_CALL(gl.api.glViewport(0, 0, output->resolution.w, output->resolution.h));
      context->resolution = output->resolution;
   }

   return true;
}

static void
surface_gen_textures(struct wlc_surface *surface, const int num_textures)
{
   assert(surface);

   for (int i = 0; i < num_textures; ++i) {
      if (surface->textures[i])
         continue;

      GL_CALL(gl.api.glGenTextures(1, &surface->textures[i]));
      GL_CALL(gl.api.glBindTexture(GL_TEXTURE_2D, surface->textures[i]));
      GL_CALL(gl.api.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
      GL_CALL(gl.api.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
   }
}

static void
surface_flush_textures(struct wlc_surface *surface)
{
   assert(surface);

   for (int i = 0; i < 3; ++i) {
      if (surface->textures[i]) {
         GL_CALL(gl.api.glDeleteTextures(1, &surface->textures[i]));
      }
   }

   memset(surface->textures, 0, sizeof(surface->textures));
}

static void
surface_flush_images(struct wlc_context *context, struct wlc_surface *surface)
{
   assert(surface);

   for (int i = 0; i < 3; ++i) {
      if (surface->images[i])
         wlc_context_destroy_image(context, surface->images[i]);
   }

   memset(surface->images, 0, sizeof(surface->images));
}

static void
surface_destroy(struct ctx *context, struct wlc_surface *surface)
{
   assert(context && surface);

   if (!surface->output)
      return;

   if (!wlc_context_bind(surface->output->context))
      return;

   surface_flush_textures(surface);
   surface_flush_images(surface->output->context, surface);
   wlc_dlog(WLC_DBG_RENDER, "-> Destroyed surface");

   if (surface->output->context != context->context)
      wlc_context_bind(context->context);
}

static bool
shm_attach(struct wlc_surface *surface, struct wlc_buffer *buffer, struct wl_shm_buffer *shm_buffer)
{
   assert(surface && buffer && shm_buffer);

   buffer->shm_buffer = shm_buffer;
   buffer->size.w = wl_shm_buffer_get_width(shm_buffer);
   buffer->size.h = wl_shm_buffer_get_height(shm_buffer);

   int pitch;
   GLenum gl_format, gl_pixel_type;
   switch (wl_shm_buffer_get_format(shm_buffer)) {
      case WL_SHM_FORMAT_XRGB8888:
         // gs->shader = &gr->texture_shader_rgbx;
         pitch = wl_shm_buffer_get_stride(shm_buffer) / 4;
         gl_format = GL_BGRA_EXT;
         gl_pixel_type = GL_UNSIGNED_BYTE;
         surface->format = SURFACE_RGBA;
         break;
      case WL_SHM_FORMAT_ARGB8888:
         // gs->shader = &gr->texture_shader_rgba;
         pitch = wl_shm_buffer_get_stride(shm_buffer) / 4;
         gl_format = GL_BGRA_EXT;
         gl_pixel_type = GL_UNSIGNED_BYTE;
         surface->format = SURFACE_RGBA;
         break;
      case WL_SHM_FORMAT_RGB565:
         // gs->shader = &gr->texture_shader_rgbx;
         pitch = wl_shm_buffer_get_stride(shm_buffer) / 2;
         gl_format = GL_RGB;
         gl_pixel_type = GL_UNSIGNED_SHORT_5_6_5;
         surface->format = SURFACE_RGB;
         break;
      default:
         /* unknown shm buffer format */
         return false;
   }

   if (surface->view && surface->view->x11_window)
      surface->format = wlc_x11_window_get_surface_format(surface->view->x11_window);

   surface_gen_textures(surface, 1);
   GL_CALL(gl.api.glBindTexture(GL_TEXTURE_2D, surface->textures[0]));
   GL_CALL(gl.api.glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, pitch));
   GL_CALL(gl.api.glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0));
   GL_CALL(gl.api.glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0));
   wl_shm_buffer_begin_access(buffer->shm_buffer);
   void *data = wl_shm_buffer_get_data(buffer->shm_buffer);
   GL_CALL(gl.api.glTexImage2D(GL_TEXTURE_2D, 0, gl_format, pitch, buffer->size.h, 0, gl_format, gl_pixel_type, data));
   wl_shm_buffer_end_access(buffer->shm_buffer);
   return true;
}

static bool
egl_attach(struct ctx *context, struct wlc_surface *surface, struct wlc_buffer *buffer, uint32_t format)
{
   assert(context && surface && buffer);

   buffer->legacy_buffer = (struct wlc_buffer*)buffer->resource;
   wlc_context_query_buffer(context->context, buffer->legacy_buffer, EGL_WIDTH, (EGLint*)&buffer->size.w);
   wlc_context_query_buffer(context->context, buffer->legacy_buffer, EGL_HEIGHT, (EGLint*)&buffer->size.h);
   wlc_context_query_buffer(context->context, buffer->legacy_buffer, EGL_WAYLAND_Y_INVERTED_WL, (EGLint*)&buffer->y_inverted);

   int num_planes;
   GLenum target = GL_TEXTURE_2D;
   switch (format) {
      case EGL_TEXTURE_RGB:
      case EGL_TEXTURE_RGBA:
      default:
         num_planes = 1;
         surface->format = SURFACE_RGBA;
         break;
      case 0x31DA:
         num_planes = 1;
         // gs->target = GL_TEXTURE_EXTERNAL_OES;
         // gs->shader = &gr->texture_shader_egl_external;
         break;
      case EGL_TEXTURE_Y_UV_WL:
         num_planes = 2;
         // gs->shader = &gr->texture_shader_y_uv;
         break;
      case EGL_TEXTURE_Y_U_V_WL:
         num_planes = 3;
         // gs->shader = &gr->texture_shader_y_u_v;
         break;
      case EGL_TEXTURE_Y_XUXV_WL:
         num_planes = 2;
         // gs->shader = &gr->texture_shader_y_xuxv;
         break;
   }

   if (surface->view && surface->view->x11_window)
      surface->format = wlc_x11_window_get_surface_format(surface->view->x11_window);

   if (num_planes > 3) {
      wlc_log(WLC_LOG_WARN, "planes > 3 in egl surfaces not supported, nor should be possible");
      return false;
   }

   surface_flush_images(context->context, surface);
   surface_gen_textures(surface, num_planes);

   for (int i = 0; i < num_planes; ++i) {
      EGLint attribs[] = { EGL_WAYLAND_PLANE_WL, i, EGL_NONE };
      if (!(surface->images[i] = wlc_context_create_image(context->context, EGL_WAYLAND_BUFFER_WL, buffer->legacy_buffer, attribs)))
         return false;

      GL_CALL(gl.api.glActiveTexture(GL_TEXTURE0 + i));
      GL_CALL(gl.api.glBindTexture(target, surface->textures[i]));
      GL_CALL(context->api.glEGLImageTargetTexture2DOES(target, surface->images[i]));
   }

   return true;
}

static bool
surface_attach(struct ctx *context, struct wlc_surface *surface, struct wlc_buffer *buffer)
{
   assert(context && surface);

   if (!buffer || !buffer->resource) {
      surface_destroy(context, surface);
      return true;
   }

   if (!wlc_context_bind(context->context))
      return false;

   int format;
   bool attached = false;
   struct wl_shm_buffer *shm_buffer = wl_shm_buffer_get(buffer->resource);
   if (shm_buffer) {
      attached = shm_attach(surface, buffer, shm_buffer);
   } else if (context->api.glEGLImageTargetTexture2DOES && wlc_context_query_buffer(context->context, (void*)buffer->resource, EGL_TEXTURE_FORMAT, &format)) {
      attached = egl_attach(context, surface, buffer, format);
   } else {
      /* unknown buffer */
      wlc_log(WLC_LOG_WARN, "Unknown buffer");
   }

   if (attached)
      wlc_dlog(WLC_DBG_RENDER, "-> Attached surface with buffer of size (%ux%u)", buffer->size.w, buffer->size.h);

   return attached;
}

static void
texture_paint(struct ctx *context, GLuint *textures, GLuint nmemb, struct wlc_geometry *geometry, struct paint *settings)
{
   const GLint vertices[8] = {
      geometry->origin.x + geometry->size.w, geometry->origin.y,
      geometry->origin.x,                    geometry->origin.y,
      geometry->origin.x + geometry->size.w, geometry->origin.y + geometry->size.h,
      geometry->origin.x,                    geometry->origin.y + geometry->size.h,
   };

   const GLint coords[8] = {
      1, 0,
      0, 0,
      1, 1,
      0, 1
   };

   set_program(context, settings->program);

   if (settings->dim > 0.0f) {
      GL_CALL(gl.api.glUniform1f(context->program->uniforms[UNIFORM_DIM], settings->dim));
   }

   for (GLuint i = 0; i < nmemb; ++i) {
      if (!textures[i])
         break;

      GL_CALL(gl.api.glActiveTexture(GL_TEXTURE0 + i));
      GL_CALL(gl.api.glBindTexture(GL_TEXTURE_2D, textures[i]));
   }

   if (settings->filter) {
      GL_CALL(gl.api.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
      GL_CALL(gl.api.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
   } else {
      GL_CALL(gl.api.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
      GL_CALL(gl.api.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
   }

   GL_CALL(gl.api.glVertexAttribPointer(0, 2, GL_INT, GL_FALSE, 0, vertices));
   GL_CALL(gl.api.glVertexAttribPointer(1, 2, GL_INT, GL_FALSE, 0, coords));
   GL_CALL(gl.api.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
}

static void
surface_paint_internal(struct ctx *context, struct wlc_surface *surface, struct wlc_geometry *geometry, struct paint *settings)
{
   assert(context && surface && geometry && settings);

   if (!surface->output || surface->output->context != context->context) {
      wlc_log(WLC_LOG_ERROR, "Trying to paint surface with wrong context or none (%p != %p)", context->context, (surface->output ? surface->output->context : NULL));
      return;
   }

   if (!wlc_size_equals(&surface->size, &geometry->size)) {
      if (wlc_geometry_equals(&settings->visible, geometry)) {
         settings->filter = true;
      } else {
         // black borders are requested
         texture_paint(context, &context->textures[TEXTURE_BLACK], 1, geometry, settings);
         memcpy(geometry, &settings->visible, sizeof(struct wlc_geometry));
      }
   }

   texture_paint(context, surface->textures, 3, geometry, settings);
}

static void
surface_paint(struct ctx *context, struct wlc_surface *surface, struct wlc_origin *pos)
{
   struct paint settings;
   memset(&settings, 0, sizeof(settings));
   settings.dim = 1.0f;
   settings.program = (enum program_type)surface->format;
   surface_paint_internal(context, surface, &(struct wlc_geometry){ { pos->x, pos->y }, { surface->size.w, surface->size.h } }, &settings);
}

static void
view_paint(struct ctx *context, struct wlc_view *view)
{
   assert(context && view);

   struct paint settings;
   memset(&settings, 0, sizeof(settings));
   settings.dim = ((view->commit.state & WLC_BIT_ACTIVATED) || (view->type & WLC_BIT_UNMANAGED) ? 1.0f : 0.5f);
   settings.program = (enum program_type)view->surface->format;

   struct wlc_geometry geometry;
   wlc_view_get_bounds(view, &geometry, &settings.visible);
   surface_paint_internal(context, view->surface, &geometry, &settings);
}

static void
pointer_paint(struct ctx *context, struct wlc_origin *pos)
{
   assert(context);
   struct paint settings;
   memset(&settings, 0, sizeof(settings));
   settings.program = PROGRAM_CURSOR;
   struct wlc_geometry g = { *pos, { 14, 14 } };
   texture_paint(context, &context->textures[TEXTURE_CURSOR], 1, &g, &settings);
}

static void
read_pixels(struct ctx *context, struct wlc_geometry *geometry, void *out_data)
{
   assert(context && geometry && out_data);
   GL_CALL(gl.api.glReadPixels(geometry->origin.x, geometry->origin.y, geometry->size.w, geometry->size.h, GL_RGBA, GL_UNSIGNED_BYTE, out_data));
}

static void
swap(struct ctx *context)
{
   assert(context);
   wlc_context_swap(context->context);
}

static void
clear(struct ctx *context)
{
   assert(context);
   GL_CALL(gl.api.glClear(GL_COLOR_BUFFER_BIT));
}

static void
terminate(struct ctx *context)
{
   assert(context);

   // FIXME: Free gl resources here

   free(context);
}

static void
unload_egl(void)
{
   if (gl.api.handle)
      dlclose(gl.api.handle);

   memset(&gl, 0, sizeof(gl));
}

void*
wlc_gles2_new(struct wlc_context *context, struct wlc_render_api *api)
{
   assert(api);

   if (!gl.api.handle && !gles2_load()) {
      unload_egl();
      return NULL;
   }

   if (!wlc_context_bind(context))
      return NULL;

   struct ctx *gl;
   if (!(gl = create_context()))
      return NULL;

   gl->context = context;

   api->terminate = terminate;
   api->bind = bind;
   api->surface_destroy = surface_destroy;
   api->surface_attach = surface_attach;
   api->view_paint = view_paint;
   api->surface_paint = surface_paint;
   api->pointer_paint = pointer_paint;
   api->read_pixels = read_pixels;
   api->clear = clear;
   api->swap = swap;

   wlc_log(WLC_LOG_INFO, "GLES2 renderer initialized");
   return gl;
}

// 0 == black, 1 == white, 2 == transparent
static GLubyte cursor_palette[] = {
  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x02, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x02, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
  0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
  0x01, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02,
  0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02, 0x02,
  0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x00, 0x01, 0x02, 0x02, 0x02, 0x02,
  0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02
};
