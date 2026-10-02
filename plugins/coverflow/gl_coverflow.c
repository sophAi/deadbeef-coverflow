/*
    Cover Flow Plugin for DeaDBeeF (GTK3 / OpenGL)
    OpenGL 3D Renderer, Shader Pipeline, and Texture Management
*/

#include "gl_coverflow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------- Matrix Math Helpers ---------------- */

static void
mat4_identity (float *m) {
    memset (m, 0, 16 * sizeof (float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void
mat4_multiply (float *out, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[j * 4 + i] = a[0 * 4 + i] * b[j * 4 + 0] +
                           a[1 * 4 + i] * b[j * 4 + 1] +
                           a[2 * 4 + i] * b[j * 4 + 2] +
                           a[3 * 4 + i] * b[j * 4 + 3];
        }
    }
    memcpy (out, r, 16 * sizeof (float));
}

static void
mat4_perspective (float *m, float fov_rad, float aspect, float near_z, float far_z) {
    memset (m, 0, 16 * sizeof (float));
    float tan_half_fov = tanf (fov_rad / 2.0f);
    m[0] = 1.0f / (aspect * tan_half_fov);
    m[5] = 1.0f / tan_half_fov;
    m[10] = -(far_z + near_z) / (far_z - near_z);
    m[11] = -1.0f;
    m[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
}

static void
mat4_lookat (float *m,
             float eyeX, float eyeY, float eyeZ,
             float targetX, float targetY, float targetZ,
             float upX, float upY, float upZ) {
    float fX = targetX - eyeX;
    float fY = targetY - eyeY;
    float fZ = targetZ - eyeZ;
    float rlen = 1.0f / sqrtf (fX * fX + fY * fY + fZ * fZ);
    fX *= rlen; fY *= rlen; fZ *= rlen;

    float sX = fY * upZ - fZ * upY;
    float sY = fZ * upX - fX * upZ;
    float sZ = fX * upY - fY * upX;
    rlen = 1.0f / sqrtf (sX * sX + sY * sY + sZ * sZ);
    sX *= rlen; sY *= rlen; sZ *= rlen;

    float uX = sY * fZ - sZ * fY;
    float uY = sZ * fX - sX * fZ;
    float uZ = sX * fY - sY * fX;

    mat4_identity (m);
    m[0] = sX;  m[4] = sY;  m[8]  = sZ;
    m[1] = uX;  m[5] = uY;  m[9]  = uZ;
    m[2] = -fX; m[6] = -fY; m[10] = -fZ;

    float t[16];
    mat4_identity (t);
    t[12] = -eyeX;
    t[13] = -eyeY;
    t[14] = -eyeZ;

    mat4_multiply (m, m, t);
}

static void
mat4_translate (float *m, float x, float y, float z) {
    float t[16];
    mat4_identity (t);
    t[12] = x;
    t[13] = y;
    t[14] = z;
    mat4_multiply (m, m, t);
}

static void
mat4_rotate_y (float *m, float angle_rad) {
    float r[16];
    mat4_identity (r);
    float c = cosf (angle_rad);
    float s = sinf (angle_rad);
    r[0] = c;
    r[2] = -s;
    r[8] = s;
    r[10] = c;
    mat4_multiply (m, m, r);
}

static void
mat4_scale (float *m, float sx, float sy, float sz) {
    float s[16];
    mat4_identity (s);
    s[0] = sx;
    s[5] = sy;
    s[10] = sz;
    mat4_multiply (m, m, s);
}

/* ---------------- Shader Sources ---------------- */

static const char *vertex_shader_source =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec2 aTexCoord;\n"
    "\n"
    "uniform mat4 u_Projection;\n"
    "uniform mat4 u_View;\n"
    "uniform mat4 u_Model;\n"
    "\n"
    "out vec2 v_TexCoord;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = u_Projection * u_View * u_Model * vec4(aPos, 1.0);\n"
    "    v_TexCoord = aTexCoord;\n"
    "}\n";

static const char *fragment_shader_source =
    "#version 330 core\n"
    "in vec2 v_TexCoord;\n"
    "out vec4 FragColor;\n"
    "\n"
    "uniform sampler2D u_Texture;\n"
    "uniform float u_Alpha;\n"
    "uniform int u_IsReflection;\n"
    "uniform float u_ReflectionFade;\n"
    "uniform vec3 u_Tint;\n"
    "\n"
    "void main() {\n"
    "    vec4 texColor = texture(u_Texture, v_TexCoord);\n"
    "    if (texColor.a < 0.01) {\n"
    "        discard;\n"
    "    }\n"
    "    if (u_IsReflection == 1) {\n"
    "        float fade = clamp(v_TexCoord.y * u_ReflectionFade, 0.0, 0.45);\n"
    "        FragColor = vec4(texColor.rgb * u_Tint, texColor.a * u_Alpha * fade);\n"
    "    } else {\n"
    "        FragColor = vec4(texColor.rgb * u_Tint, texColor.a * u_Alpha);\n"
    "    }\n"
    "}\n";

/* ---------------- Procedural Default Texture ---------------- */

static GLuint
create_default_vinyl_texture (void) {
    const int size = 256;
    uint8_t *data = malloc (size * size * 4);
    if (!data) return 0;

    float center = size / 2.0f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int idx = (y * size + x) * 4;
            float dx = x - center;
            float dy = y - center;
            float dist = sqrtf (dx * dx + dy * dy);

            if (dist > center - 4.0f) {
                /* Outer dark edge */
                data[idx + 0] = 20;
                data[idx + 1] = 20;
                data[idx + 2] = 24;
                data[idx + 3] = 255;
            } else if (dist < 32.0f) {
                if (dist < 10.0f) {
                    /* Center spindle hole */
                    data[idx + 0] = 12;
                    data[idx + 1] = 12;
                    data[idx + 2] = 14;
                    data[idx + 3] = 255;
                } else {
                    /* Vinyl center label (cyan/indigo tint) */
                    data[idx + 0] = 52;
                    data[idx + 1] = 101;
                    data[idx + 2] = 164;
                    data[idx + 3] = 255;
                }
            } else {
                /* Vinyl grooves */
                float groove = sinf (dist * 0.9f) * 8.0f;
                uint8_t val = (uint8_t)fminf (255.0f, fmaxf (0.0f, 32.0f + groove));
                data[idx + 0] = val;
                data[idx + 1] = val + 2;
                data[idx + 2] = val + 6;
                data[idx + 3] = 255;
            }
        }
    }

    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap (GL_TEXTURE_2D);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    free (data);
    return tex;
}

/* ---------------- Renderer Lifecycle ---------------- */

void
gl_coverflow_init (gl_coverflow_renderer_t *r) {
    memset (r, 0, sizeof (*r));

    /* Compile vertex shader */
    GLuint vs = glCreateShader (GL_VERTEX_SHADER);
    glShaderSource (vs, 1, &vertex_shader_source, NULL);
    glCompileShader (vs);

    GLint success;
    glGetShaderiv (vs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog (vs, 512, NULL, infoLog);
        fprintf (stderr, "CoverFlow VS Error: %s\n", infoLog);
    }

    /* Compile fragment shader */
    GLuint fs = glCreateShader (GL_FRAGMENT_SHADER);
    glShaderSource (fs, 1, &fragment_shader_source, NULL);
    glCompileShader (fs);
    glGetShaderiv (fs, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog (fs, 512, NULL, infoLog);
        fprintf (stderr, "CoverFlow FS Error: %s\n", infoLog);
    }

    /* Link program */
    r->shader_program = glCreateProgram ();
    glAttachShader (r->shader_program, vs);
    glAttachShader (r->shader_program, fs);
    glLinkProgram (r->shader_program);
    glDeleteShader (vs);
    glDeleteShader (fs);

    /* Uniform locations */
    r->u_projection      = glGetUniformLocation (r->shader_program, "u_Projection");
    r->u_view            = glGetUniformLocation (r->shader_program, "u_View");
    r->u_model           = glGetUniformLocation (r->shader_program, "u_Model");
    r->u_texture         = glGetUniformLocation (r->shader_program, "u_Texture");
    r->u_alpha           = glGetUniformLocation (r->shader_program, "u_Alpha");
    r->u_is_reflection   = glGetUniformLocation (r->shader_program, "u_IsReflection");
    r->u_reflection_fade = glGetUniformLocation (r->shader_program, "u_ReflectionFade");
    r->u_tint            = glGetUniformLocation (r->shader_program, "u_Tint");

    /* Quad geometry: Main cover (6 vertices) + Reflection (6 vertices) */
    static const float vertices[] = {
        /* Main Cover: Position (x,y,z), TexCoord (u,v) */
        -1.0f,  2.0f, 0.0f,   0.0f, 0.0f,
        -1.0f,  0.0f, 0.0f,   0.0f, 1.0f,
         1.0f,  0.0f, 0.0f,   1.0f, 1.0f,

        -1.0f,  2.0f, 0.0f,   0.0f, 0.0f,
         1.0f,  0.0f, 0.0f,   1.0f, 1.0f,
         1.0f,  2.0f, 0.0f,   1.0f, 0.0f,

        /* Reflection: Mirrored vertically below ground plane */
        -1.0f, -0.02f, 0.0f,  0.0f, 1.0f,
        -1.0f, -2.02f, 0.0f,  0.0f, 0.0f,
         1.0f, -2.02f, 0.0f,  1.0f, 0.0f,

        -1.0f, -0.02f, 0.0f,  0.0f, 1.0f,
         1.0f, -2.02f, 0.0f,  1.0f, 0.0f,
         1.0f, -0.02f, 0.0f,  1.0f, 1.0f
    };

    glGenVertexArrays (1, &r->vao);
    glGenBuffers (1, &r->vbo);

    glBindVertexArray (r->vao);
    glBindBuffer (GL_ARRAY_BUFFER, r->vbo);
    glBufferData (GL_ARRAY_BUFFER, sizeof (vertices), vertices, GL_STATIC_DRAW);

    /* Attribute 0: Position */
    glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof (float), (void *)0);
    glEnableVertexAttribArray (0);

    /* Attribute 1: TexCoord */
    glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof (float), (void *)(3 * sizeof (float)));
    glEnableVertexAttribArray (1);

    glBindBuffer (GL_ARRAY_BUFFER, 0);
    glBindVertexArray (0);

    r->default_texture_id = create_default_vinyl_texture ();
    r->initialized = TRUE;
}

void
gl_coverflow_cleanup (gl_coverflow_renderer_t *r) {
    if (!r->initialized) return;

    if (r->default_texture_id) {
        glDeleteTextures (1, &r->default_texture_id);
        r->default_texture_id = 0;
    }
    if (r->vao) {
        glDeleteVertexArrays (1, &r->vao);
        r->vao = 0;
    }
    if (r->vbo) {
        glDeleteBuffers (1, &r->vbo);
        r->vbo = 0;
    }
    if (r->shader_program) {
        glDeleteProgram (r->shader_program);
        r->shader_program = 0;
    }
    r->initialized = FALSE;
}

void
gl_coverflow_resize (gl_coverflow_renderer_t *r, int width, int height) {
    r->width = width > 0 ? width : 1;
    r->height = height > 0 ? height : 1;
    r->aspect_ratio = (float)r->width / (float)r->height;

    glViewport (0, 0, r->width, r->height);

    /* Perspective projection: FOV 42 degrees */
    mat4_perspective (r->mat_proj, 42.0f * (float)M_PI / 180.0f, r->aspect_ratio, 0.1f, 100.0f);

    /* Camera view: slightly above ground, looking towards center */
    mat4_lookat (r->mat_view,
                 0.0f, 0.95f, 4.4f,  /* eye position */
                 0.0f, 0.85f, 0.0f,  /* target lookat */
                 0.0f, 1.0f,  0.0f); /* up vector */
}

/* ---------------- Render Single Cover Instance ---------------- */

static inline void
get_album_aspect_scale (const coverflow_album_t *al, float *out_sx, float *out_sy) {
    float w = al ? al->tex_width : 0;
    float h = al ? al->tex_height : 0;
    if (w > 0.0f && h > 0.0f) {
        if (w >= h) {
            *out_sx = 1.0f;
            *out_sy = h / w;
        } else {
            *out_sx = w / h;
            *out_sy = 1.0f;
        }
    } else {
        *out_sx = 1.0f;
        *out_sy = 1.0f;
    }
}

static void
draw_cover_quad (gl_coverflow_renderer_t *r, float x, float z, float angle_rad, float tint, GLuint tex_id, float sx, float sy) {
    float model[16];
    mat4_identity (model);
    mat4_translate (model, x, 0.0f, z);
    mat4_rotate_y (model, angle_rad);
    mat4_scale (model, sx, sy, 1.0f);

    glUniformMatrix4fv (r->u_model, 1, GL_FALSE, model);
    glUniform3f (r->u_tint, tint, tint, tint);

    glBindTexture (GL_TEXTURE_2D, tex_id);

    /* 1. Draw Main Cover Quad (First 6 vertices) */
    glUniform1i (r->u_is_reflection, 0);
    glUniform1f (r->u_alpha, 1.0f);
    glDepthMask (GL_TRUE);
    glDrawArrays (GL_TRIANGLES, 0, 6);

    /* 2. Draw Reflection Quad (Second 6 vertices) */
    glUniform1i (r->u_is_reflection, 1);
    glUniform1f (r->u_reflection_fade, 0.55f);
    glDepthMask (GL_FALSE); /* Keep reflection transparent */
    glDrawArrays (GL_TRIANGLES, 6, 6);
    glDepthMask (GL_TRUE);
}

/* ---------------- Main Render Loop ---------------- */

void
gl_coverflow_render (gl_coverflow_renderer_t *r, album_manager_t *mgr, float current_pos) {
    if (!r->initialized) return;

    /* Subtle dark background */
    glClearColor (0.05f, 0.05f, 0.06f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!mgr || mgr->count == 0) {
        return;
    }

    glEnable (GL_DEPTH_TEST);
    glDepthFunc (GL_LEQUAL);

    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram (r->shader_program);

    glUniformMatrix4fv (r->u_projection, 1, GL_FALSE, r->mat_proj);
    glUniformMatrix4fv (r->u_view, 1, GL_FALSE, r->mat_view);
    glUniform1i (r->u_texture, 0);
    glActiveTexture (GL_TEXTURE0);

    glBindVertexArray (r->vao);

    int count = mgr->count;
    int center_idx = (int)roundf (current_pos);
    int visible_range = 14;

    int min_idx = center_idx - visible_range;
    if (min_idx < 0) min_idx = 0;
    int max_idx = center_idx + visible_range;
    if (max_idx >= count) max_idx = count - 1;

    /* Render order: Far Left -> Near Left, Far Right -> Near Right, Center Cover LAST
       This ensures transparent reflection and edges blend correctly. */

    /* 1. Left side covers: from far left inwards */
    for (int i = min_idx; i < center_idx; i++) {
        float delta = (float)i - current_pos;
        float angle, x, z, tint;

        if (delta <= -1.0f) {
            angle = 60.0f * (float)M_PI / 180.0f;
            x = -1.35f + (delta + 1.0f) * 0.38f;
            z = -0.75f + (delta + 1.0f) * 0.05f;
            float dist_fade = (-delta - 1.0f) * 0.02f;
            if (dist_fade > 0.12f) dist_fade = 0.12f;
            tint = 0.46f - dist_fade;
        } else {
            /* Smooth transition into center */
            float t = (delta + 1.0f); // 0.0 to 1.0
            angle = (1.0f - t) * (60.0f * (float)M_PI / 180.0f);
            x = -1.35f * (1.0f - t) + delta * 1.35f * t;
            z = -0.75f * (1.0f - t);
            tint = 0.46f + 0.54f * t;
        }

        coverflow_album_t *al = &mgr->albums[i];
        float sx, sy;
        get_album_aspect_scale (al, &sx, &sy);
        GLuint tex = al->texture_id ? al->texture_id : r->default_texture_id;
        draw_cover_quad (r, x, z, angle, tint, tex, sx, sy);
    }

    /* 2. Right side covers: from far right inwards */
    for (int i = max_idx; i > center_idx; i--) {
        float delta = (float)i - current_pos;
        float angle, x, z, tint;

        if (delta >= 1.0f) {
            angle = -60.0f * (float)M_PI / 180.0f;
            x = 1.35f + (delta - 1.0f) * 0.38f;
            z = -0.75f - (delta - 1.0f) * 0.05f;
            float dist_fade = (delta - 1.0f) * 0.02f;
            if (dist_fade > 0.12f) dist_fade = 0.12f;
            tint = 0.46f - dist_fade;
        } else {
            /* Smooth transition into center */
            float t = (1.0f - delta); // 0.0 to 1.0
            angle = -(1.0f - t) * (60.0f * (float)M_PI / 180.0f);
            x = 1.35f * (1.0f - t) + delta * 1.35f * t;
            z = -0.75f * (1.0f - t);
            tint = 0.46f + 0.54f * t;
        }

        coverflow_album_t *al = &mgr->albums[i];
        float sx, sy;
        get_album_aspect_scale (al, &sx, &sy);
        GLuint tex = al->texture_id ? al->texture_id : r->default_texture_id;
        draw_cover_quad (r, x, z, angle, tint, tex, sx, sy);
    }

    /* 3. Center cover */
    if (center_idx >= 0 && center_idx < count) {
        float delta = (float)center_idx - current_pos;
        float angle = -delta * (60.0f * (float)M_PI / 180.0f);
        float x = delta * 1.35f;
        float z = -fabsf (delta) * 0.75f;
        float tint = 1.0f - fabsf (delta) * 0.54f;

        coverflow_album_t *al = &mgr->albums[center_idx];
        float sx, sy;
        get_album_aspect_scale (al, &sx, &sy);
        GLuint tex = al->texture_id ? al->texture_id : r->default_texture_id;
        draw_cover_quad (r, x, z, angle, tint, tex, sx, sy);
    }

    glBindVertexArray (0);
    glUseProgram (0);
}

/* ---------------- Texture Loading from Image File ---------------- */

GLuint
gl_coverflow_load_texture_from_file (const char *filepath, int *out_w, int *out_h) {
    if (!filepath || !*filepath) return 0;

    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale (filepath, 512, 512, TRUE, &error);
    if (!pixbuf) {
        if (error) g_error_free (error);
        return 0;
    }

    int width = gdk_pixbuf_get_width (pixbuf);
    int height = gdk_pixbuf_get_height (pixbuf);
    int n_channels = gdk_pixbuf_get_n_channels (pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride (pixbuf);
    const guchar *pixels = gdk_pixbuf_get_pixels (pixbuf);

    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, rowstride / n_channels);

    GLenum format = (n_channels == 4) ? GL_RGBA : GL_RGB;

    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexImage2D (GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, pixels);

    glGenerateMipmap (GL_TEXTURE_2D);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);

    g_object_unref (pixbuf);

    if (out_w) *out_w = width;
    if (out_h) *out_h = height;

    return tex;
}

void
gl_coverflow_delete_texture (GLuint *tex_id) {
    if (tex_id && *tex_id) {
        glDeleteTextures (1, tex_id);
        *tex_id = 0;
    }
}
