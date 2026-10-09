/*
 * gles - EGL and OpenGL ES 2 through the host: a window surface, a shader
 * program, a triangle with one colour per corner, and a look at the pixels
 * that come out.
 */
#define EGL_NO_X11
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void fail(const char *what)
{
	printf("FAIL: %s (EGL error %#x)\n", what, (unsigned)eglGetError());
	failures++;
}

static GLuint compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		printf("shader: %s\n", log);
		fail("shader compilation");
	}
	return shader;
}

static int close_to(const unsigned char *pixel, int r, int g, int b)
{
	return abs(pixel[0] - r) <= 4 && abs(pixel[1] - g) <= 4 && abs(pixel[2] - b) <= 4;
}

int main(int argc, char **argv)
{
	/* gles [frames]: more frames make something to look at on a screen. */
	int frames = argc > 1 ? atoi(argv[1]) : 4;
	float step = frames > 16 ? 0.03f : 0.5f;
	static const char vertex_source[] =
		"attribute vec2 position;\n"
		"attribute vec3 colour;\n"
		"uniform float angle;\n"
		"varying vec3 shade;\n"
		"void main() {\n"
		"	float c = cos(angle), s = sin(angle);\n"
		"	gl_Position = vec4(position.x * c - position.y * s,\n"
		"			   position.x * s + position.y * c, 0.0, 1.0);\n"
		"	shade = colour;\n"
		"}\n";
	static const char fragment_source[] =
		"precision mediump float;\n"
		"varying vec3 shade;\n"
		"void main() { gl_FragColor = vec4(shade, 1.0); }\n";
	static const GLfloat vertices[] = {
		/* x, y, then r, g, b */
		-0.8f, -0.8f, 1.0f, 0.0f, 0.0f,
		0.8f,  -0.8f, 0.0f, 1.0f, 0.0f,
		0.0f,  0.8f,  0.0f, 0.0f, 1.0f,
	};
	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0, count = 0, width = 0, height = 0;
	EGLConfig config;
	EGLSurface surface;
	EGLContext context;
	GLuint program, buffer;
	GLint position, colour, angle, linked = 0;
	unsigned char pixel[4];
	const char *extensions;

	if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
		fail("eglInitialize");
		printf("FAIL\n");
		return 1;
	}
	printf("EGL %d.%d, %s\n", major, minor, eglQueryString(display, EGL_VENDOR));
	if (!eglBindAPI(EGL_OPENGL_ES_API))
		fail("eglBindAPI");
	if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count < 1) {
		fail("eglChooseConfig");
		printf("FAIL\n");
		return 1;
	}
	surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(display, surface, surface, context)) {
		fail("surface, context or eglMakeCurrent");
		printf("FAIL\n");
		return 1;
	}
	eglQuerySurface(display, surface, EGL_WIDTH, &width);
	eglQuerySurface(display, surface, EGL_HEIGHT, &height);
	printf("%s, %s, %dx%d\n", (const char *)glGetString(GL_RENDERER),
	       (const char *)glGetString(GL_VERSION), width, height);
	if (width < 16 || height < 16)
		fail("surface size");

	program = glCreateProgram();
	glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
	glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked)
		fail("program link");
	glUseProgram(program);
	position = glGetAttribLocation(program, "position");
	colour = glGetAttribLocation(program, "colour");
	angle = glGetUniformLocation(program, "angle");

	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (void *)0);
	glVertexAttribPointer(colour, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
			      (void *)(2 * sizeof(GLfloat)));
	glEnableVertexAttribArray(position);
	glEnableVertexAttribArray(colour);

	/* A few frames turning, the last one upright: that is the one checked. */
	for (int frame = frames > 0 ? frames - 1 : 0; frame >= 0; frame--) {
		glViewport(0, 0, width, height);
		glClearColor(0.1f, 0.2f, 0.4f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glUniform1f(angle, frame * step);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		if (frame == 0) {
			/* The middle of the screen is half blue, a quarter red and
			 * a quarter green; the corner is the clear colour. */
			glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
			printf("centre: %d %d %d\n", pixel[0], pixel[1], pixel[2]);
			if (!close_to(pixel, 64, 64, 128))
				fail("colour in the triangle");
			glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
			printf("corner: %d %d %d\n", pixel[0], pixel[1], pixel[2]);
			if (!close_to(pixel, 26, 51, 102))
				fail("clear colour");
		}
		if (!eglSwapBuffers(display, surface))
			fail("eglSwapBuffers");
	}
	if (glGetError() != GL_NO_ERROR)
		fail("a GL error was raised");

	/* An extension function, which only eglGetProcAddress() can give. */
	extensions = (const char *)glGetString(GL_EXTENSIONS);
	if (extensions && strstr(extensions, "GL_OES_vertex_array_object")) {
		void (*gen)(GLsizei, GLuint *) =
			(void (*)(GLsizei, GLuint *))eglGetProcAddress("glGenVertexArraysOES");
		void (*del)(GLsizei, const GLuint *) =
			(void (*)(GLsizei, const GLuint *))eglGetProcAddress("glDeleteVertexArraysOES");
		GLuint array = 0;

		if (!gen || !del) {
			fail("eglGetProcAddress");
		} else {
			gen(1, &array);
			printf("vertex array %u from an extension function\n", array);
			if (!array || glGetError() != GL_NO_ERROR)
				fail("glGenVertexArraysOES");
			del(1, &array);
		}
	}
	if ((void *)eglGetProcAddress("glClear") != (void *)glClear)
		fail("eglGetProcAddress of a core function");

	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglDestroySurface(display, surface);
	eglTerminate(display);
	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
