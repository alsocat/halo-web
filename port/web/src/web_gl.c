/*
WEB_GL.C

The OpenGL ES helpers that the Android host provides to the renderer
(port/linux/src/xgpu.h), on WebGL 2.

WebGL names its extensions without the GL_ prefix, sometimes differently
(WEBGL_compressed_texture_s3tc), and turns each one on only when asked: a
query here enables it. WebGL cannot wait on the GPU, and keeps a buffer that
queued draws read intact when it is written, so the frame fences have
nothing to do.
*/

#include <string.h>

#include <emscripten/html5_webgl.h>
#include <GLES3/gl3.h>

#include "platform.h"
#include "xgpu.h"

/* the WebGL extensions that give an OpenGL ES one */
static const char *const extension_names[][2] = {
	{ "GL_EXT_texture_compression_s3tc", "WEBGL_compressed_texture_s3tc" },
	{ "GL_EXT_texture_filter_anisotropic", "EXT_texture_filter_anisotropic" },
};

int host_gl_has_extension(const char *name)
{
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_get_current_context();
	unsigned index;

	if (!context)
		return 0;
	for (index = 0; index < sizeof(extension_names) / sizeof(*extension_names); index++)
	{
		if (!strcmp(name, extension_names[index][0]))
			return emscripten_webgl_enable_extension(context, extension_names[index][1]) ? 1 : 0;
	}
	/* (no copy image, border clamp or atomic counters in WebGL 2) */
	return 0;
}

unsigned int host_gl_read_buffer_word(unsigned int buffer, unsigned int offset)
{
	/* only for atomic counters, which WebGL 2 lacks */
	(void)buffer; (void)offset;
	return 0;
}

void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{
	glBufferSubData((GLenum)target, (GLintptr)offset, (GLsizeiptr)size, data);
}

void host_gl_fence_frame(unsigned int slot)
{
	(void)slot;
}

void host_gl_wait_frame(unsigned int slot)
{
	(void)slot;
}
