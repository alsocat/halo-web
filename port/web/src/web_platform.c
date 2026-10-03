/*
WEB_PLATFORM.C

The page around the game (port/web/shell): its settings, its files, its
canvas and its frames.

- The settings the browser decides (port_config.c's environment overrides):
  the data and save roots, and no self-updater, internet play or UPnP.
- The files: /data/maps is the server's maps (web_maps_backend.cpp); /saves,
  where z:\ and u:\ and config.toml live, is the browser's private storage
  (OPFS), or memory where the browser has none.
- The canvas: the page tells its size in the device's pixels
  (web_set_display_size), which the renderer takes as the display
  (sdl_platform.c platform_screen_mode), so the game draws the page's shape
  at its resolution.
- The frames: the game's thread never returns to the browser, which shows a
  worker's canvas only when it does. Each frame is taken off the canvas as an
  ImageBitmap instead and posted to the page, which shows it on a canvas of
  its own. The game waits while the page is two frames behind, which paces
  it to the page's animation frames, as vsync would.
*/

#include <stdio.h>
#include <stdlib.h>


#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>
#include <GLES3/gl3.h>

#include "platform.h"
#include "posix.h"

/* web_maps_backend.cpp */
backend_t web_maps_backend_create(void);

static int display_width, display_height;
static int frames_pending;

/* ---------- settings */

__attribute__((constructor(102)))
static void web_settings(void)
{
	setenv("HALO_DATA_ROOT", "/data", 0);
	setenv("HALO_SAVE_ROOT", "/saves", 0);
	setenv("HALO_UPDATE_AUTO", "false", 0);
	/* internet play needs UDP; the page's own network (posix_net_web.c)
	is system link */
	setenv("HALO_NET_ONLINE", "false", 0);
	setenv("HALO_NET_ALLOW_UPNP", "false", 0);
	/* the game's log goes to the browser's console, not over the game
	(the page's ?debug shows it all on screen too: port/web/shell/page.js) */
	setenv("HALO_CONSOLE_LOG", "none", 0);
}

/* ---------- files */

void web_platform_initialize(void)
{
	static int initialized;
	backend_t storage;

	if (initialized)
		return;
	initialized = 1;
	posix_make_directory("/data");
	if (wasmfs_create_directory("/data/maps", 0555, web_maps_backend_create()) != 0)
		platform_log("the server's maps are not available");
	storage = wasmfs_create_opfs_backend();
	if (!storage || wasmfs_create_directory("/saves", 0777, storage) != 0)
	{
		platform_log("this browser has no storage for the game: saved games last until the page closes");
		posix_make_directory("/saves");
	}
}

/* ---------- the canvas */

/* the page's canvas, in the device's pixels (from the page's thread) */
EMSCRIPTEN_KEEPALIVE void web_set_display_size(int width, int height)
{
	__atomic_store_n(&display_width, width, __ATOMIC_RELEASE);
	__atomic_store_n(&display_height, height, __ATOMIC_RELEASE);
}

BOOL web_display_size(long *width, long *height)
{
	int display_width_now = __atomic_load_n(&display_width, __ATOMIC_ACQUIRE);
	int display_height_now = __atomic_load_n(&display_height, __ATOMIC_ACQUIRE);

	if (display_width_now <= 0 || display_height_now <= 0)
		return FALSE;
	*width = display_width_now;
	*height = display_height_now;
	return TRUE;
}

/* ---------- frames */

/* posts the canvas's frame to the page's haloFrame (Emscripten's
"call handler" message, 9 in libpthread.js), which lowers *pending once it
has shown it */
#ifdef HALO_WEB_JSPI
/* the JSPI engine (halo-jspi.js): the game's thread waits for its next
animation frame, returning to the browser, which shows the canvas then
(the canvas is the page's own, handed to this thread): no copy of the
frame, and the frames follow the display's refresh, as vsync would */
EM_ASYNC_JS(void, web_wait_animation_frame, (void), {
	await new Promise((resolve) => {
		if (self.requestAnimationFrame) self.requestAnimationFrame(() => resolve());
		else setTimeout(resolve, 0);
	});
});
#endif

/* the page's count of frames shown (page.js haloFrames), every 30 frames */
EM_JS(void, web_post_frame_count, (int frames), {
	postMessage({ cmd: 9, handler: 'haloFrames', args: [frames] });
});

/* ?HALO_WEB_PROFILE=1: WebGL calls counted (port/web/shell/pre.js) */
EM_JS(void, web_profile_frame, (int start), {
	if (start && self.haloProfileGL) self.haloProfileGL();
	if (self.haloProfileFrame) self.haloProfileFrame();
});

EM_JS(void, web_post_frame, (int *pending), {
	var canvas = GLctx && GLctx.canvas;
	if (!canvas || !canvas.transferToImageBitmap) {
		Atomics.sub(HEAP32, pending >> 2, 1);
		return;
	}
	var bitmap = canvas.transferToImageBitmap();
	postMessage({ cmd: 9, handler: 'haloFrame', args: [bitmap, pending] }, [bitmap]);
});

/* ?HALO_WEB_TIMING=1: where each frame's time goes, every 5 seconds: the
game's own work (from the last frame's end to this one's present), taking
the frame to the page, and waiting for the page to show the last ones */
static void web_frame_timing(double started, double posted, double waited)
{
	static int enabled = -1;
	static double last_end, window_start, game, post, wait;
	static unsigned long frames;

	if (enabled < 0)
	{
		const char *setting = getenv("HALO_WEB_TIMING");

		enabled = setting && (*setting == '1' || *setting == '2' || *setting == '3');
	}
	if (!enabled)
		return;
	if (last_end)
	{
		game += started - last_end;
		post += posted - started;
		wait += waited - posted;
		frames++;
	}
	else
	{
		window_start = waited;
	}
	last_end = waited;
	if (waited - window_start >= 5000.0 && frames)
	{
		platform_log("frames: %.1f fps; per frame %.2f ms game, %.2f ms to the page, %.2f ms waiting for it",
			frames * 1000.0 / (waited - window_start), game / frames, post / frames, wait / frames);
		window_start = waited;
		game = post = wait = 0.0;
		frames = 0;
	}
}

void web_present_frame(void)
{
	long width, height;
	int pending, canvas_width, canvas_height;

	/* the frame opaque: the game leaves its alpha as the render targets had
	it (0), which some browsers show as nothing over the page's black (the
	renderer resets the GL state it caches after a frame: d3d8_gl.c) */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	double started = emscripten_get_now(), posted, waited;
	static int profile = -1;
	static int finish = -1;
	static double finish_total;
	static unsigned long finish_frames;

	/* ?HALO_WEB_TIMING=2: how long the browser's GPU process takes to run a
	frame's commands (glFinish waits for them) */
	if (finish < 0)
	{
		const char *setting = getenv("HALO_WEB_TIMING");

		finish = setting && *setting == '2';
	}
	if (finish)
	{
		double before = emscripten_get_now();

		glFinish();
		finish_total += emscripten_get_now() - before;
		if (++finish_frames == 150)
		{
			platform_log("the GPU process runs a frame in %.2f ms", finish_total / finish_frames);
			finish_total = 0.0;
			finish_frames = 0;
		}
		started = emscripten_get_now();
	}

	if (profile < 0)
	{
		const char *setting = getenv("HALO_WEB_PROFILE");

		profile = setting && *setting == '1';
		if (profile)
			web_profile_frame(1);
	}
	if (profile)
		web_profile_frame(0);

	{
		static int frames_shown;

		if (++frames_shown % 30 == 1)
			web_post_frame_count(frames_shown);
	}
#ifdef HALO_WEB_JSPI
	glFlush();
	posted = emscripten_get_now();
	web_wait_animation_frame();
	waited = emscripten_get_now();
	web_frame_timing(started, posted, waited);
	if (web_display_size(&width, &height) &&
		emscripten_get_canvas_element_size("#canvas", &canvas_width, &canvas_height) == EMSCRIPTEN_RESULT_SUCCESS &&
		(canvas_width != width || canvas_height != height))
	{
		emscripten_set_canvas_element_size("#canvas", (int)width, (int)height);
	}
	(void)pending;
	return;
#endif
	/* ?HALO_WEB_TIMING=3: no frames to the page (nothing is shown), to
	measure the game and the GPU process alone */
	if (finish < 0 || !getenv("HALO_WEB_TIMING") || *getenv("HALO_WEB_TIMING") != '3')
	{
		__atomic_add_fetch(&frames_pending, 1, __ATOMIC_SEQ_CST);
		web_post_frame(&frames_pending);
	}
	else
	{
		glFlush();
	}
	posted = emscripten_get_now();
	while ((pending = __atomic_load_n(&frames_pending, __ATOMIC_ACQUIRE)) >= 2)
		emscripten_futex_wait(&frames_pending, (uint32_t)pending, 100.0);
	waited = emscripten_get_now();
	web_frame_timing(started, posted, waited);
	/* the next frame at the page's size */
	if (web_display_size(&width, &height) &&
		emscripten_get_canvas_element_size("#canvas", &canvas_width, &canvas_height) == EMSCRIPTEN_RESULT_SUCCESS &&
		(canvas_width != width || canvas_height != height))
	{
		emscripten_set_canvas_element_size("#canvas", (int)width, (int)height);
	}
}

/* ---------- events */

void web_process_queued_calls(void)
{
	emscripten_current_thread_process_queued_calls();
}
