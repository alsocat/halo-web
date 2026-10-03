/*
WEB_STUBS.C

The desktop features of the Linux platform layer that a page has no use
for: the self-updater (the server has the newest build), importing a disc
image (the server has the maps) and its file dialog, and an MSVC intrinsic
that clang only knows on x86.
*/

#include <stdio.h>

#include <SDL3/SDL.h>

#include "xiso.h"

void updater_start(void)
{
}

void updater_poll(SDL_Window *window)
{
	(void)window;
}

int xiso_extract_maps(const char *image_path, const char *destination, xiso_progress_proc progress, void *context,
	char *error, int error_size)
{
	(void)image_path; (void)destination; (void)progress; (void)context;
	snprintf(error, (size_t)error_size, "the server provides the maps");
	return 0;
}

/* no file dialog in a page: the choice is always none */
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
	const SDL_DialogFileFilter *filters, int nfilters, const char *default_location, bool allow_many)
{
	static const char *const none[] = { NULL };

	(void)window; (void)filters; (void)nfilters; (void)default_location; (void)allow_many;
	callback(userdata, none, -1);
}

/* a compiler barrier (scenario.c): a call is one already */
void _ReadWriteBarrier(void)
{
}
