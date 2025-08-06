#include "glsl/texcopy.frag.h"
#include "glsl/texcopy.vert.h"
#include "glsl/textshader.frag.h"
#include "glsl/textshader.vert.h"
#include "util/debug.h"

#include "scene.h"
#include "server/gl.h"
#include "server/ui.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <GLES2/gl2.h>
#include <ft2build.h>
#include <spng.h>
#include FT_FREETYPE_H

#include <bits/time.h>
#include <time.h>

struct vtx_shader {
    float src_pos[2];
    float dst_pos[2];
    float src_rgba[4];
    float dst_rgba[4];
};

struct scene_image {
    struct wl_list link; // scene.images
    struct scene *parent;

    size_t shader_index;

    GLuint tex, vbo;

    int32_t width, height;
};

struct scene_mirror {
    struct wl_list link; // scene.mirrors
    struct scene *parent;

    size_t shader_index;

    GLuint vbo;

    float src_rgba[4], dst_rgba[4];
};

struct scene_timer {
    struct wl_list link; // scene.text
    struct scene *parent;

    int32_t x, y;
    size_t last_time;
    size_t pause_time;
    bool paused;
    float rgba[4];
    size_t size;
    int decimals;
};

static void build_image(struct scene_image *out, struct scene *scene,
                        const struct scene_image_options *options, int32_t width, int32_t height);
static void build_mirror(struct scene_mirror *mirror, const struct scene_mirror_options *options,
                         struct scene *scene);
static void build_rect(struct vtx_shader out[static 6], const struct box *src,
                       const struct box *dst, const float src_rgba[static 4],
                       const float dst_rgba[static 4]);
static void draw_frame(struct scene *scene);
void draw_ttf_text(struct scene *scene, const char text[], float x, float y, size_t size,
                   const float color[4]);
void draw_timer(struct scene *scene, struct scene_timer *timer);
static void draw_image(struct scene *scene, struct scene_image *image);
static void draw_mirror(struct scene *scene, struct scene_mirror *mirror,
                        unsigned int capture_texture, int32_t width, int32_t height);
static void draw_vertex_list(struct scene_shader *shader, size_t num_vertices);
void scene_font_destroy(struct font_obj *font);

static void
on_gl_frame(struct wl_listener *listener, void *data) {
    struct scene *scene = wl_container_of(listener, scene, on_gl_frame);

    server_gl_with(scene->gl, true) {
        draw_frame(scene);
    }
}

static void
build_image(struct scene_image *out, struct scene *scene, const struct scene_image_options *options,
            int32_t width, int32_t height) {
    struct vtx_shader vertices[6] = {0};

    build_rect(vertices, &(struct box){0, 0, width, height}, &options->dst, (float[4]){0, 0, 0, 0},
               (float[4]){0, 0, 0, 0});

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &out->vbo);
        ww_assert(out->vbo != 0);

        gl_using_buffer(GL_ARRAY_BUFFER, out->vbo) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        }
    }
}

static void
build_mirror(struct scene_mirror *mirror, const struct scene_mirror_options *options,
             struct scene *scene) {
    struct vtx_shader vertices[6] = {0};

    build_rect(vertices, &options->src, &options->dst, options->src_rgba, mirror->dst_rgba);

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &mirror->vbo);
        ww_assert(mirror->vbo != 0);

        gl_using_buffer(GL_ARRAY_BUFFER, mirror->vbo) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
        }
    }
}

static void
build_rect(struct vtx_shader out[static 6], const struct box *s, const struct box *d,
           const float src_rgba[static 4], const float dst_rgba[static 4]) {
    const struct {
        float src[2];
        float dst[2];
    } data[] = {
        // top-left triangle
        {{s->x, s->y}, {d->x, d->y}},
        {{s->x + s->width, s->y}, {d->x + d->width, d->y}},
        {{s->x, s->y + s->height}, {d->x, d->y + d->height}},

        // bottom-right triangle
        {{s->x + s->width, s->y}, {d->x + d->width, d->y}},
        {{s->x, s->y + s->height}, {d->x, d->y + d->height}},
        {{s->x + s->width, s->y + s->height}, {d->x + d->width, d->y + d->height}},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(data); i++) {
        struct vtx_shader *vtx = &out[i];

        memcpy(vtx->src_pos, data[i].src, sizeof(vtx->src_pos));
        memcpy(vtx->dst_pos, data[i].dst, sizeof(vtx->dst_pos));
        memcpy(vtx->src_rgba, src_rgba, sizeof(vtx->src_rgba));
        memcpy(vtx->dst_rgba, dst_rgba, sizeof(vtx->dst_rgba));
    }
}

static void
draw_image(struct scene *scene, struct scene_image *image) {
    // The OpenGL context must be current.
    server_gl_shader_use(scene->shaders.data[image->shader_index].shader);
    glUniform2f(scene->shaders.data[image->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[image->shader_index].shader_u_src_size, image->width,
                image->height);

    gl_using_buffer(GL_ARRAY_BUFFER, image->vbo) {
        gl_using_texture(GL_TEXTURE_2D, image->tex) {
            // Each image has 6 vertices in its vertex buffer.
            draw_vertex_list(&scene->shaders.data[image->shader_index], 6);
        }
    }
}

static void
draw_frame(struct scene *scene) {
    // The OpenGL context must be current.

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0, 0, 0, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, scene->ui->width, scene->ui->height);

    GLuint capture_texture = server_gl_get_capture(scene->gl);
    bool have_mirrors = (capture_texture != 0 && !wl_list_empty(&scene->mirrors));
    bool have_images = !wl_list_empty(&scene->images);
    bool have_text = util_debug_enabled || !wl_list_empty(&scene->text);
    bool have_timer = !wl_list_empty(&scene->timer);

    if (!have_mirrors && !have_images && !have_text && !have_timer) {
        scene->skipped_frames++;

        if (scene->skipped_frames > 1) {
            return;
        }
    } else {
        scene->skipped_frames = 0;
    }

    // Draw all mirrors using their respective shaders.
    if (capture_texture != 0) {
        int32_t width, height;
        server_gl_get_capture_size(scene->gl, &width, &height);
        struct scene_mirror *mirror;
        wl_list_for_each (mirror, &scene->mirrors, link) {
            draw_mirror(scene, mirror, capture_texture, width, height);
        }
    }

    // Draw all images using their respective shaders.
    struct scene_image *image;
    wl_list_for_each (image, &scene->images, link) {
        draw_image(scene, image);
    }

    struct scene_text *text;
    wl_list_for_each (text, &scene->text, link) {
        draw_ttf_text(scene, text->text, (float)text->x, (float)text->y, text->size, text->rgba);
    }

    struct scene_timer *timer;
    wl_list_for_each (timer, &scene->timer, link) {
        draw_timer(scene, timer);
    }

    glUseProgram(0);
    server_gl_swap_buffers(scene->gl);
}

static void
draw_mirror(struct scene *scene, struct scene_mirror *mirror, unsigned int capture_texture,
            int32_t width, int32_t height) {
    // The OpenGL context must be current.
    server_gl_shader_use(scene->shaders.data[mirror->shader_index].shader);
    glUniform2f(scene->shaders.data[mirror->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[mirror->shader_index].shader_u_src_size, width, height);

    gl_using_buffer(GL_ARRAY_BUFFER, mirror->vbo) {
        gl_using_texture(GL_TEXTURE_2D, capture_texture) {
            // Each mirror has 6 vertices in its vertex buffer.
            draw_vertex_list(&scene->shaders.data[mirror->shader_index], 6);
        }
    }
}

size_t
decode_utf8(const char *s, uint32_t *codepoint) {
    const unsigned char *p = (const unsigned char *)s;

    if (p[0] < 0x80) {
        *codepoint = p[0];
        return 1;
    }

    if ((p[0] & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) != 0x80)
            goto fail;
        uint32_t cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        if (cp < 0x80)
            goto fail; // Overlong
        *codepoint = cp;
        return 2;
    }

    if ((p[0] & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
            goto fail;
        uint32_t cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
            goto fail; // Overlong or surrogate
        *codepoint = cp;
        return 3;
    }

    if ((p[0] & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
            goto fail;
        uint32_t cp =
            ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF)
            goto fail; // Overlong or out of range
        *codepoint = cp;
        return 4;
    }

fail:
    //ww_log(LOG_WARN, "UTF-8 decode fail at byte 0x%02X", p[0]);
    *codepoint = 0xFFFD;
    return 1;
}

struct font_char
build_glyph(struct scene *scene, const u_int32_t c, size_t font_height) {
    FT_Set_Pixel_Sizes(scene->font.face, 0, font_height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (FT_Load_Char(scene->font.face, c, FT_LOAD_RENDER)) {
        ww_log(LOG_ERROR, "Failed to load char");
        exit(EXIT_FAILURE);
    }
    // generate texture
    GLuint texture;
    glGenTextures(1, &texture);
    gl_using_texture(GL_TEXTURE_2D, texture) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, (int)scene->font.face->glyph->bitmap.width,
                     (int)scene->font.face->glyph->bitmap.rows, 0, GL_RED, GL_UNSIGNED_BYTE,
                     scene->font.face->glyph->bitmap.buffer);
        // set texture options
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    struct font_char ch;

    ch.texture = texture;
    ch.width = (int)scene->font.face->glyph->bitmap.width;
    ch.height = (int)scene->font.face->glyph->bitmap.rows;
    ch.bearingX = scene->font.face->glyph->bitmap_left;
    ch.bearingY = scene->font.face->glyph->bitmap_top;
    ch.advance = scene->font.face->glyph->advance.x;
    ch.character = c;

    return ch;
}

void
load_ttf_char(struct scene *scene, const u_int32_t c, size_t font_height) {
    // check if a font_size_obj exists
    int size_index = -1;
    for (int i = 0; i < (signed int)scene->font.fonts_len; ++i) {
        if (scene->font.fonts[i].font_height == font_height) {
            size_index = i;
            break;
        }
    }
    if (size_index == -1) {
        // make a new font_size_obj
        size_index = scene->font.fonts_len;
        struct font_size_obj *tmp =
            realloc(scene->font.fonts, sizeof(struct font_size_obj) * (scene->font.fonts_len + 1));
        if (!tmp) {
            exit(EXIT_FAILURE);
        }
        scene->font.fonts = tmp;
        scene->font.fonts[size_index].font_height = font_height;
        scene->font.fonts[size_index].chars_len = 0;
        scene->font.fonts[size_index].chars = NULL;
        scene->font.fonts_len++;
    }

    // add new char
    struct font_char *tmp =
        realloc(scene->font.fonts[size_index].chars,
                sizeof(struct font_char) * (scene->font.fonts[size_index].chars_len + 1));
    if (!tmp) {
        exit(EXIT_FAILURE);
    }
    scene->font.fonts[size_index].chars = tmp;
    scene->font.fonts[size_index].chars[scene->font.fonts[size_index].chars_len] =
        build_glyph(scene, c, font_height);
    scene->font.fonts[size_index].chars_len++;
}

struct font_char
get_ttf_char(struct scene *scene, const u_int32_t c, size_t font_height) {
    for (size_t i = 0; i < scene->font.fonts_len; ++i) {
        if (scene->font.fonts[i].font_height == font_height) {
            struct font_size_obj *fs = &scene->font.fonts[i];

            for (size_t j = 0; j < fs->chars_len; ++j) {
                if (fs->chars[j].character == c) {
                    return fs->chars[j];
                }
            }

            load_ttf_char(scene, c, font_height);
            return fs->chars[fs->chars_len - 1];
        }
    }

    load_ttf_char(scene, c, font_height);

    // this is probably a bad idea
    return get_ttf_char(scene, c, font_height);
}

void
draw_ttf_text(struct scene *scene, const char text[], float x, float y, size_t size,
              const float color[4]) {
    y = (float)scene->ui->height - y - size;

    glUseProgram(scene->font.shaderProgram);

    const GLint srcSizeLoc = glGetUniformLocation(scene->font.shaderProgram, "u_src_size");
    if (srcSizeLoc != -1) {
        glUniform2f(srcSizeLoc, 1.0f, 1.0f);
    }

    const GLint dstSizeLoc = glGetUniformLocation(scene->font.shaderProgram, "u_dst_size");
    if (dstSizeLoc != -1) {
        glUniform2f(dstSizeLoc, (float)scene->ui->width, (float)scene->ui->height);
    }

    const GLint texLoc = glGetUniformLocation(scene->font.shaderProgram, "textTexture");
    if (texLoc != -1) {
        glUniform1i(texLoc, 0);
    }

    const GLint colorLoc = glGetUniformLocation(scene->font.shaderProgram, "textColor");
    if (colorLoc != -1) {
        glUniform4fv(colorLoc, 1, color);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glActiveTexture(GL_TEXTURE0);

    GLint loc = glGetAttribLocation(scene->font.shaderProgram, "vertex");

    gl_using_buffer(GL_ARRAY_BUFFER, scene->font.VBO) {
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
        glEnableVertexAttribArray(loc);

        float current_x = x;
        float current_y = y;

        const char *ptr = text;
        while (*ptr != '\0') {
            uint32_t codepoint;
            size_t bytes = decode_utf8(ptr, &codepoint);
            ptr += bytes;

            if (codepoint == '\n') {
                current_x = x;
                current_y -= size;
                continue;
            }

            const struct font_char ch = get_ttf_char(scene, codepoint, size);

            const float xpos = current_x + (float)ch.bearingX;
            const float ypos = current_y - (float)(ch.height - ch.bearingY);
            const float w = (float)ch.width;
            const float h = (float)ch.height;

            const float vertices[6][4] = {
                {xpos, ypos + h, 0.0f, 0.0f}, // top-left
                {xpos, ypos, 0.0f, 1.0f},     // bottom-left
                {xpos + w, ypos, 1.0f, 1.0f}, // bottom-right

                {xpos, ypos + h, 0.0f, 0.0f},    // top-left
                {xpos + w, ypos, 1.0f, 1.0f},    // bottom-right
                {xpos + w, ypos + h, 1.0f, 0.0f} // top-right
            };

            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

            gl_using_texture(GL_TEXTURE_2D, ch.texture) {
                GLenum error = glGetError();
                if (error != GL_NO_ERROR) {
                    ww_log(LOG_ERROR, "OpenGL error before drawing char U+%04X: %d", codepoint,
                           error);
                }

                glDrawArrays(GL_TRIANGLES, 0, 6);

                error = glGetError();
                if (error != GL_NO_ERROR) {
                    ww_log(LOG_ERROR, "OpenGL error after drawing char U+%04X: %d", codepoint,
                           error);
                }
            }

            current_x += (float)(ch.advance >> 6);
        }

        glDisableVertexAttribArray(loc);
    }
}

float
get_ttf_text_advance(struct scene *scene, const char text[], size_t size) {
    float total_advance = 0.0f;
    const char *ptr = text;

    while (*ptr != '\0') {
        uint32_t codepoint;
        size_t bytes = decode_utf8(ptr, &codepoint);
        ptr += bytes;

        if (codepoint == '\n') {
            break;
        }

        const struct font_char ch = get_ttf_char(scene, codepoint, size);
        total_advance += (float)(ch.advance >> 6);
    }

    return total_advance;
}

void
format_time(uint64_t ms, char *out, size_t out_size, int digits) {
    if (digits < 0)
        digits = 0;
    if (digits > 6)
        digits = 6;

    unsigned int total_seconds = ms / 1000;
    unsigned int minutes = total_seconds / 60;
    unsigned int seconds = total_seconds % 60;

    double fractional = (ms % 1000) / 1000.0;

    if (digits == 0) {
        snprintf(out, out_size, "%u:%02u", minutes, seconds);
    } else {
        double frac_scaled = fractional * pow(10, digits);
        unsigned int frac = (unsigned int)(frac_scaled + 0.5);

        char format[20];
        snprintf(format, sizeof(format), "%%u:%%02u.%%0%uu", digits);
        snprintf(out, out_size, format, minutes, seconds, frac);
    }
}

void
draw_timer(struct scene *scene, struct scene_timer *timer) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t current = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    char text[16];
    if (timer->paused) {
        format_time(timer->pause_time - timer->last_time, text, sizeof(text), timer->decimals);
    } else {
        format_time(current - timer->last_time, text, sizeof(text), timer->decimals);
    }
    draw_ttf_text(scene, text, (float)timer->x, (float)timer->y, timer->size, timer->rgba);
}

void
scene_timer_toggle_pause(struct scene_timer *timer) {
    struct timespec now;
    if (timer->paused) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t current = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;

        // Adjust `last_time` forward by pause duration
        uint64_t pause_duration = current - timer->pause_time;
        timer->last_time += pause_duration;

        timer->paused = false;
    } else {
        timer->paused = true;
        clock_gettime(CLOCK_MONOTONIC, &now);
        timer->pause_time = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    }
}

void
scene_timer_reset(struct scene_timer *timer) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t current = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;

    timer->last_time = current;
    timer->paused = false;
    timer->pause_time = 0;
}

static void
draw_vertex_list(struct scene_shader *shader, size_t num_vertices) {
    // The OpenGL context must be current, a texture must be bound to copy from, a vertex buffer
    // with data must be bound, and a valid shader must be in use.

    glVertexAttribPointer(SHADER_SRC_POS_ATTRIB_LOC, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, src_pos));
    glVertexAttribPointer(SHADER_DST_POS_ATTRIB_LOC, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, dst_pos));
    glVertexAttribPointer(SHADER_SRC_RGBA_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, src_rgba));
    glVertexAttribPointer(SHADER_DST_RGBA_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, dst_rgba));

    glEnableVertexAttribArray(SHADER_SRC_POS_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_DST_POS_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_SRC_RGBA_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_DST_RGBA_ATTRIB_LOC);

    glDrawArrays(GL_TRIANGLES, 0, num_vertices);

    glDisableVertexAttribArray(SHADER_SRC_POS_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_DST_POS_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_SRC_RGBA_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_DST_RGBA_ATTRIB_LOC);
}

static bool
image_load(struct scene_image *out, struct scene *scene, void *pngbuf, size_t pngbuf_size) {
    int err;

    struct spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) {
        ww_log(LOG_ERROR, "failed to create spng context");
        return false;
    }
    spng_set_image_limits(ctx, scene->image_max_size, scene->image_max_size);

    // Decode the PNG.
    err = spng_set_png_buffer(ctx, pngbuf, pngbuf_size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to set png buffer: %s\n", spng_strerror(err));
        goto fail_spng_set_png_buffer;
    }

    struct spng_ihdr ihdr;
    err = spng_get_ihdr(ctx, &ihdr);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get image header: %s\n", spng_strerror(err));
        goto fail_spng_get_ihdr;
    }

    size_t decode_size;
    err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &decode_size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get image size: %s\n", spng_strerror(err));
        goto fail_spng_decoded_image_size;
    }

    char *decode_buf = malloc(decode_size);
    check_alloc(decode_buf);

    err = spng_decode_image(ctx, decode_buf, decode_size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to decode image: %s\n", spng_strerror(err));
        goto fail_spng_decode_image;
    }

    out->width = ihdr.width;
    out->height = ihdr.height;

    // Upload the decoded image data to a new OpenGL texture.
    server_gl_with(scene->gl, false) {
        glGenTextures(1, &out->tex);
        gl_using_texture(GL_TEXTURE_2D, out->tex) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ihdr.width, ihdr.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, decode_buf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
    }

    free(decode_buf);
    spng_ctx_free(ctx);

    return true;

fail_spng_decode_image:
    free(decode_buf);

fail_spng_decoded_image_size:
fail_spng_get_ihdr:
fail_spng_set_png_buffer:
    spng_ctx_free(ctx);

    return false;
}

static void
image_release(struct scene_image *image) {
    wl_list_remove(&image->link);
    wl_list_init(&image->link);

    if (image->parent) {
        server_gl_with(image->parent->gl, false) {
            glDeleteTextures(1, &image->tex);
            glDeleteBuffers(1, &image->vbo);
        }
    }

    image->parent = NULL;
}

static void
mirror_release(struct scene_mirror *mirror) {
    wl_list_remove(&mirror->link);
    wl_list_init(&mirror->link);

    if (mirror->parent) {
        server_gl_with(mirror->parent->gl, false) {
            glDeleteBuffers(1, &mirror->vbo);
        }
    }

    mirror->parent = NULL;
}

static void
text_release(struct scene_text *text) {
    wl_list_remove(&text->link);
    wl_list_init(&text->link);

    text->parent = NULL;
}

static void
timer_release(struct scene_timer *timer) {
    wl_list_remove(&timer->link);
    wl_list_init(&timer->link);

    timer->parent = NULL;
}

static int
shader_find_index(struct scene *scene, const char *key) {
    if (key == NULL) {
        return 0;
    }
    for (size_t i = 1; i < scene->shaders.count; i++) {
        if (strcmp(scene->shaders.data[i].name, key) == 0) {
            return i;
        }
    }
    ww_log(LOG_WARN, "shader %s not found, falling back to default", key);
    return 0;
}

static bool
shader_create(struct server_gl *gl, struct scene_shader *data, char *name, const char *vertex,
              const char *fragment) {
    data->name = name;
    data->shader = server_gl_compile(gl, vertex ? vertex : WAYWALL_GLSL_TEXCOPY_VERT_H,
                                     fragment ? fragment : WAYWALL_GLSL_TEXCOPY_FRAG_H);
    if (!data->shader) {
        return false;
    }

    data->shader_u_src_size = glGetUniformLocation(data->shader->program, "u_src_size");
    data->shader_u_dst_size = glGetUniformLocation(data->shader->program, "u_dst_size");

    return true;
}

struct scene *
scene_create(struct config *cfg, struct server_gl *gl, struct server_ui *ui) {
    struct scene *scene = zalloc(1, sizeof(*scene));

    scene->gl = gl;
    scene->ui = ui;

    // Initialize OpenGL resources.
    server_gl_with(scene->gl, false) {
        GLint tex_size;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tex_size);

        scene->image_max_size = (uint32_t)tex_size;
        ww_log(LOG_INFO, "max image size: %" PRIu32 "x%" PRIu32 "\n", scene->image_max_size,
               scene->image_max_size);

        scene->shaders.count = cfg->shaders.count + 1;
        scene->shaders.data = malloc(sizeof(struct scene_shader) * scene->shaders.count);
        if (!shader_create(scene->gl, &scene->shaders.data[0], strdup("default"), NULL, NULL)) {
            ww_log(LOG_ERROR, "error creating default shader");
            server_gl_exit(scene->gl);
            goto fail_compile_texture_copy;
        }
        for (size_t i = 0; i < cfg->shaders.count; i++) {
            if (!shader_create(scene->gl, &scene->shaders.data[i + 1],
                               strdup(cfg->shaders.data[i].name), cfg->shaders.data[i].vertex,
                               cfg->shaders.data[i].fragment)) {
                ww_log(LOG_ERROR, "error creating %s shader", cfg->shaders.data[i].name);
                server_gl_exit(scene->gl);
                goto fail_compile_texture_copy;
            }
            ww_log(LOG_INFO, "created %s shader", cfg->shaders.data[i].name);
        }

        // Initialize vertex buffers.
        glGenBuffers(1, &scene->buffers.debug);

        // TTF font loading
        if (FT_Init_FreeType(&scene->font.ft)) {
            ww_log(LOG_ERROR, "Failed to init freetype.");
            exit(1);
        }

        if (FT_New_Face(scene->font.ft, cfg->theme.font_path, 0, &scene->font.face)) {
            ww_log(LOG_ERROR, "Failed to load freetype face.");
            exit(1);
        }

        glGenBuffers(1, &scene->font.VBO);

        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        const char *vertSource = WAYWALL_GLSL_TEXTSHADER_VERT_H;
        glShaderSource(vertexShader, 1, &vertSource, NULL);
        glCompileShader(vertexShader);

        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        const char *fragSource = WAYWALL_GLSL_TEXTSHADER_FRAG_H;
        glShaderSource(fragmentShader, 1, &fragSource, NULL);
        glCompileShader(fragmentShader);

        scene->font.shaderProgram = glCreateProgram();
        glAttachShader(scene->font.shaderProgram, vertexShader);
        glAttachShader(scene->font.shaderProgram, fragmentShader);
        glLinkProgram(scene->font.shaderProgram);
    }

    scene->on_gl_frame.notify = on_gl_frame;
    wl_signal_add(&gl->events.frame, &scene->on_gl_frame);

    wl_list_init(&scene->images);
    wl_list_init(&scene->mirrors);
    wl_list_init(&scene->text);
    wl_list_init(&scene->timer);

    return scene;

fail_compile_texture_copy:
    free(scene);

    return NULL;
}

void
scene_destroy(struct scene *scene) {
    struct scene_image *image, *image_tmp;
    wl_list_for_each_safe (image, image_tmp, &scene->images, link) {
        image_release(image);
    }

    struct scene_mirror *mirror, *mirror_tmp;
    wl_list_for_each_safe (mirror, mirror_tmp, &scene->mirrors, link) {
        mirror_release(mirror);
    }

    struct scene_text *text, *text_tmp;
    wl_list_for_each_safe (text, text_tmp, &scene->text, link) {
        text_release(text);
    }

    server_gl_with(scene->gl, false) {
        for (size_t i = 0; i < scene->shaders.count; i++) {
            server_gl_shader_destroy(scene->shaders.data[i].shader);
            free(scene->shaders.data[i].name);
        }

        glDeleteBuffers(1, &scene->buffers.debug);
        glDeleteTextures(1, &scene->buffers.font_tex);
    }
    free(scene->shaders.data);

    scene_font_destroy(&scene->font);

    wl_list_remove(&scene->on_gl_frame.link);

    free(scene);
}

struct scene_image *
scene_add_image(struct scene *scene, const struct scene_image_options *options, void *pngbuf,
                size_t pngbuf_size) {
    struct scene_image *image = zalloc(1, sizeof(*image));

    image->parent = scene;

    // Load the PNG into an OpenGL texture.
    if (!image_load(image, scene, pngbuf, pngbuf_size)) {
        free(image);
        return NULL;
    }

    // Find correct shader for this image
    image->shader_index = shader_find_index(scene, options->shader_name);

    // Build a vertex buffer containing the data for this image.
    build_image(image, scene, options, image->width, image->height);

    wl_list_insert(&scene->images, &image->link);

    return image;
}

struct scene_mirror *
scene_add_mirror(struct scene *scene, const struct scene_mirror_options *options) {
    struct scene_mirror *mirror = zalloc(1, sizeof(*mirror));

    mirror->parent = scene;
    memcpy(mirror->src_rgba, options->src_rgba, sizeof(mirror->src_rgba));
    memcpy(mirror->dst_rgba, options->dst_rgba, sizeof(mirror->dst_rgba));

    // Find correct shader for this mirror
    mirror->shader_index = shader_find_index(scene, options->shader_name);

    wl_list_insert(&scene->mirrors, &mirror->link);

    build_mirror(mirror, options, scene);

    return mirror;
}

struct scene_text *
scene_add_text(struct scene *scene, const char *data, const struct scene_text_options *options) {
    struct scene_text *text = zalloc(1, sizeof(*text));

    text->parent = scene;
    text->x = options->x;
    text->y = options->y;
    text->text = strdup(data);
    text->rgba[0] = options->rgba[0];
    text->rgba[1] = options->rgba[1];
    text->rgba[2] = options->rgba[2];
    text->rgba[3] = options->rgba[3];
    text->size = options->size;

    wl_list_insert(&scene->text, &text->link);

    return text;
}

struct scene_timer *
scene_add_timer(struct scene *scene, const struct scene_timer_options *options) {
    struct scene_timer *timer = zalloc(1, sizeof(*timer));

    timer->parent = scene;
    timer->x = options->x;
    timer->y = options->y;
    timer->rgba[0] = options->rgba[0];
    timer->rgba[1] = options->rgba[1];
    timer->rgba[2] = options->rgba[2];
    timer->rgba[3] = options->rgba[3];
    timer->size = options->size;
    timer->paused = false;
    timer->decimals = options->decimals;

    wl_list_insert(&scene->timer, &timer->link);

    return timer;
}

void
scene_image_destroy(struct scene_image *image) {
    image_release(image);
    free(image);
}

void
scene_mirror_destroy(struct scene_mirror *mirror) {
    mirror_release(mirror);
    free(mirror);
}

void
scene_text_destroy(struct scene_text *text) {
    text_release(text);
    free(text->text);
    free(text);
}

void
scene_timer_destroy(struct scene_timer *timer) {
    timer_release(timer);
    free(timer);
}

void
scene_font_destroy(struct font_obj *font) {
    if (!font)
        return;

    for (size_t i = 0; i < font->fonts_len; ++i) {
        struct font_size_obj *size_obj = &font->fonts[i];
        for (size_t j = 0; j < size_obj->chars_len; ++j) {
            glDeleteTextures(1, &size_obj->chars[j].texture);
        }
        free(size_obj->chars);
    }

    free(font->fonts);
    font->fonts = NULL;
    font->fonts_len = 0;

    if (font->VBO) {
        glDeleteBuffers(1, &font->VBO);
        font->VBO = 0;
    }

    if (font->shaderProgram) {
        glDeleteProgram(font->shaderProgram);
        font->shaderProgram = 0;
    }

    if (font->face) {
        FT_Done_Face(font->face);
        font->face = NULL;
    }
    if (font->ft) {
        FT_Done_FreeType(font->ft);
        font->ft = NULL;
    }
}
