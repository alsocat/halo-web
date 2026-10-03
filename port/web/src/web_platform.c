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
EM_JS(void, web_post_frame, (int *pending), {
	var canvas = GLctx && GLctx.canvas;
	if (!canvas || !canvas.transferToImageBitmap) {
		Atomics.sub(HEAP32, pending >> 2, 1);
		return;
	}
	var bitmap = canvas.transferToImageBitmap();
	postMessage({ cmd: 9, handler: 'haloFrame', args: [bitmap, pending] }, [bitmap]);
});

void web_present_frame(void)
{
	long width, height;
	int pending, canvas_width, canvas_height;

	static unsigned long frames;
	int waits = 0;

	__atomic_add_fetch(&frames_pending, 1, __ATOMIC_SEQ_CST);
	web_post_frame(&frames_pending);
	while ((pending = __atomic_load_n(&frames_pending, __ATOMIC_ACQUIRE)) >= 2)
	{
		emscripten_futex_wait(&frames_pending, (uint32_t)pending, 100.0);
		if (++waits == 10)
			platform_log("DEBUG frame %lu: the page has not shown the last frames (%d pending)", frames, pending);
	}
	if (++frames % 300 == 0)
		platform_log("DEBUG frame %lu", frames);
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
