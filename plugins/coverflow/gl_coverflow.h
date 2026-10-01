/*
    Cover Flow Plugin for DeaDBeeF (GTK3 / OpenGL)
    OpenGL 3D Renderer and Texture Management
*/

#ifndef __GL_COVERFLOW_H__
#define __GL_COVERFLOW_H__

#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include "album_manager.h"

typedef struct {
    GLuint shader_program;
    GLint  u_projection;
    GLint  u_view;
    GLint  u_model;
    GLint  u_texture;
    GLint  u_alpha;
    GLint  u_is_reflection;
    GLint  u_reflection_fade;
    GLint  u_tint;

    GLuint vao;
    GLuint vbo;
    GLuint default_texture_id;

    int width;
    int height;
    float aspect_ratio;

    float mat_proj[16];
    float mat_view[16];

    gboolean initialized;
} gl_coverflow_renderer_t;

void   gl_coverflow_init (gl_coverflow_renderer_t *r);
void   gl_coverflow_cleanup (gl_coverflow_renderer_t *r);
void   gl_coverflow_resize (gl_coverflow_renderer_t *r, int width, int height);
void   gl_coverflow_render (gl_coverflow_renderer_t *r, album_manager_t *mgr, float current_pos);
GLuint gl_coverflow_load_texture_from_file (const char *filepath, int *out_w, int *out_h);
void   gl_coverflow_delete_texture (GLuint *tex_id);

#endif /* __GL_COVERFLOW_H__ */
