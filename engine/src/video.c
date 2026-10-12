/* GPU video: the game frame goes through a shader chain drawn at the display's native resolution.
 *
 *   pixel  pixel perfect: square pixels, integer scale, no blending (default)
 *   crt    CRT-Lottes: scanlines, shadow mask and bloom
 *
 * Both use the largest integer scale of the game's base resolution that fits the window. Frames that
 * the core renders at a multiple of the base (HD Mode 7) are detected from their height, so the
 * on-screen size doesn't jump when a scene switches to HD.
 *
 * Shaders use the RetroArch GLSL conventions (VERTEX/FRAGMENT in one file, COMPAT_* macros), so the
 * same files run on desktop OpenGL 3.2 and on OpenGL ES 3 (Android).
 */

#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#define GLSL_VERSION "#version 150\n"
#else
#include <GLES3/gl3.h>
#define GLSL_VERSION "#version 300 es\n"
#endif

#define MAX_PASSES 2
#define BASE_HEIGHT 224

typedef struct {
	GLuint program;
	GLint mvp, output_size, texture_size, input_size, frame_count, texture, original, original_size;
} Shader;

typedef struct {
	const char *file;
	bool linear;   /* sampling filter for this pass's input */
	bool to_screen;
} Pass;

typedef struct {
	const char *id, *name;
	Pass passes[MAX_PASSES];
	int count;
	Shader shaders[MAX_PASSES];
	bool ready;
} Profile;

static Profile profiles[] = {
	{ "pixel", "Pixel perfect", { { "pixel.glsl", false, true } }, 1, { { 0 } }, false },
	{ "crt", "CRT", { { "crt-lottes.glsl", false, true } }, 1, { { 0 } }, false },
};
#define PROFILES (int)(sizeof profiles / sizeof profiles[0])
#define PIXEL 0

static SDL_Window *window;
static int current;
static GLuint source, intermediate, framebuffer, vao_screen, vao_offscreen;
static int source_w, source_h, source_format = -1, intermediate_w, intermediate_h;
static unsigned frame_count;
static int pixel_format = VIDEO_0RGB1555;
static char capture_path[4096];

static void write_capture(int w, int h);

static char *read_text(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *text = malloc(size + 1);
	size_t got = fread(text, 1, size, f);
	text[got] = 0;
	fclose(f);
	return text;
}

static GLuint compile(GLenum type, const char *define, const char *body, const char *name)
{
	const char *sources[] = { GLSL_VERSION, define, body };
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 3, sources, NULL);
	glCompileShader(shader);
	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(shader, sizeof log, NULL, log);
		fprintf(stderr, "shader %s: %s\n", name, log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static bool load_shader(Shader *s, const char *dir, const char *file)
{
	char path[4096];
	snprintf(path, sizeof path, "%s/%s", dir, file);
	char *body = read_text(path);
	if (!body) {
		fprintf(stderr, "shader não encontrado: %s\n", path);
		return false;
	}
	GLuint vs = compile(GL_VERTEX_SHADER, "#define VERTEX\n", body, file);
	GLuint fs = compile(GL_FRAGMENT_SHADER, "#define FRAGMENT\n", body, file);
	free(body);
	if (!vs || !fs)
		return false;

	s->program = glCreateProgram();
	glAttachShader(s->program, vs);
	glAttachShader(s->program, fs);
	glBindAttribLocation(s->program, 0, "VertexCoord");
	glBindAttribLocation(s->program, 1, "TexCoord");
	glLinkProgram(s->program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok;
	glGetProgramiv(s->program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetProgramInfoLog(s->program, sizeof log, NULL, log);
		fprintf(stderr, "shader %s: %s\n", file, log);
		return false;
	}
	s->mvp = glGetUniformLocation(s->program, "MVPMatrix");
	s->output_size = glGetUniformLocation(s->program, "OutputSize");
	s->texture_size = glGetUniformLocation(s->program, "TextureSize");
	s->input_size = glGetUniformLocation(s->program, "InputSize");
	s->frame_count = glGetUniformLocation(s->program, "FrameCount");
	s->texture = glGetUniformLocation(s->program, "Texture");
	s->original = glGetUniformLocation(s->program, "PassPrev2Texture");
	s->original_size = glGetUniformLocation(s->program, "PassPrev2TextureSize");
	return true;
}

static GLuint make_quad(bool to_screen)
{
	/* Screen passes put the first texture row at the top; offscreen passes keep it at row 0,
	 * so every intermediate texture has the same layout as the source frame. */
	float t0 = to_screen ? 1.0f : 0.0f, t1 = to_screen ? 0.0f : 1.0f;
	const float vertices[] = {
		-1, -1, 0, 1, 0, t0, 0, 0,
		 1, -1, 0, 1, 1, t0, 0, 0,
		-1,  1, 0, 1, 0, t1, 0, 0,
		 1,  1, 0, 1, 1, t1, 0, 0,
	};
	GLuint vao, vbo;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)(4 * sizeof(float)));
	return vao;
}

static GLuint make_texture(void)
{
	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return texture;
}

bool video_init(SDL_Window *w, const char *shader_dir, const char *profile)
{
	current = PIXEL;
	window = w;
	SDL_GLContext context = SDL_GL_CreateContext(window);
	if (!context) {
		fprintf(stderr, "OpenGL: %s\n", SDL_GetError());
		return false;
	}

	for (int i = 0; i < PROFILES; i++) {
		Profile *p = &profiles[i];
		p->ready = shader_dir != NULL;
		for (int j = 0; j < p->count && p->ready; j++)
			p->ready = load_shader(&p->shaders[j], shader_dir, p->passes[j].file);
		if (profile && !strcmp(profile, p->id))
			current = i;
	}
	if (!profiles[PIXEL].ready) {
		fprintf(stderr, "os shaders não carregaram\n");
		return false;
	}
	if (!profiles[current].ready)
		current = PIXEL;

	vao_screen = make_quad(true);
	vao_offscreen = make_quad(false);
	source = make_texture();
	intermediate = make_texture();
	glGenFramebuffers(1, &framebuffer);
	return true;
}

static void upload(const void *data, unsigned width, unsigned height, size_t pitch, int format)
{
	GLenum gl_format = GL_BGRA, type = GL_UNSIGNED_INT_8_8_8_8_REV;
	int bytes = 4;
	if (format == VIDEO_RGB565)
		gl_format = GL_RGB, type = GL_UNSIGNED_SHORT_5_6_5, bytes = 2;
	else if (format == VIDEO_0RGB1555)
		type = GL_UNSIGNED_SHORT_1_5_5_5_REV, bytes = 2;

	glBindTexture(GL_TEXTURE_2D, source);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / bytes));
	if ((int)width != source_w || (int)height != source_h || format != source_format) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, gl_format, type, data);
		source_w = width, source_h = height, source_format = format;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, gl_format, type, data);
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void set_filter(GLuint texture, bool linear)
{
	glBindTexture(GL_TEXTURE_2D, texture);
	GLint filter = linear ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

static void draw(const Shader *s, GLuint input, int in_w, int in_h, int out_w, int out_h, bool to_screen)
{
	static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	glUseProgram(s->program);
	glUniformMatrix4fv(s->mvp, 1, GL_FALSE, identity);
	glUniform2f(s->output_size, (float)out_w, (float)out_h);
	glUniform2f(s->texture_size, (float)in_w, (float)in_h);
	glUniform2f(s->input_size, (float)in_w, (float)in_h);
	glUniform1i(s->frame_count, (GLint)frame_count);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, input);
	glUniform1i(s->texture, 0);
	if (s->original >= 0) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, source);
		glUniform1i(s->original, 1);
		glUniform2f(s->original_size, (float)source_w, (float)source_h);
		glActiveTexture(GL_TEXTURE0);
	}
	glBindVertexArray(to_screen ? vao_screen : vao_offscreen);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void video_frame(const void *data, unsigned width, unsigned height, size_t pitch)
{
	frame_count++;
	upload(data, width, height, pitch, pixel_format);
}

void video_present(void)
{
	if (!source_w)
		return;
	unsigned width = source_w, height = source_h;
	int w, h;
	SDL_GL_GetDrawableSize(window, &w, &h);
	int hd = (int)height >= 2 * BASE_HEIGHT ? (int)(height + BASE_HEIGHT / 2) / BASE_HEIGHT : 1;
	int base_w = width / hd, base_h = height / hd;
	int scale = w / base_w < h / base_h ? w / base_w : h / base_h;
	int dst_w, dst_h;
	if (scale >= 1) {
		dst_w = base_w * scale, dst_h = base_h * scale;
	} else {  /* window smaller than the game: shrink, keeping the proportion */
		float fit = (float)w / base_w < (float)h / base_h ? (float)w / base_w : (float)h / base_h;
		dst_w = (int)(base_w * fit), dst_h = (int)(base_h * fit);
	}

	Profile *p = &profiles[current];

	GLuint input = source;
	int in_w = width, in_h = height;
	for (int i = 0; i < p->count; i++) {
		const Pass *pass = &p->passes[i];
		set_filter(input, pass->linear);
		if (pass->to_screen) {
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, w, h);
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			glViewport((w - dst_w) / 2, (h - dst_h) / 2, dst_w, dst_h);
			draw(&p->shaders[i], input, in_w, in_h, dst_w, dst_h, true);
		} else {
			if (intermediate_w != in_w || intermediate_h != in_h) {
				glBindTexture(GL_TEXTURE_2D, intermediate);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, in_w, in_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
				intermediate_w = in_w, intermediate_h = in_h;
			}
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, intermediate, 0);
			glViewport(0, 0, in_w, in_h);
			draw(&p->shaders[i], input, in_w, in_h, in_w, in_h, false);
			input = intermediate;
		}
	}
	if (capture_path[0]) {
		write_capture(w, h);
		capture_path[0] = 0;
	}
	SDL_GL_SwapWindow(window);
}

static void write_capture(int w, int h)
{
	unsigned char *pixels = malloc((size_t)w * h * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels);
	FILE *f = fopen(capture_path, "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int y = h - 1; y >= 0; y--)
			fwrite(pixels + (size_t)y * w * 3, 1, (size_t)w * 3, f);
		fclose(f);
	}
	free(pixels);
}

void video_capture_next(const char *path)
{
	snprintf(capture_path, sizeof capture_path, "%s", path);
}

bool video_set_vsync(bool on)
{
	return SDL_GL_SetSwapInterval(on ? 1 : 0) == 0;
}

void video_set_format(int format)
{
	pixel_format = format;
}

void video_next_profile(void)
{
	do
		current = (current + 1) % PROFILES;
	while (!profiles[current].ready);
}

const char *video_profile_name(void)
{
	return profiles[current].name;
}
