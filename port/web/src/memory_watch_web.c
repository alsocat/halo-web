/*
MEMORY_WATCH_WEB.C

Write tracking for guest memory that the renderer caches
(port/linux/src/memory_watch.c's interface), without page protection,
which WebAssembly does not have. Pages get a new generation when the
platform layer knows they are written: file reads into them
(memory_watch_prepare_write), allocation and freeing (memory_watch_forget)
and Direct3D's resource registration and locks (d3d8_gl.c, d3d8_resources.c,
under HALO_WEB).
*/

#include "platform.h"

#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)

static unsigned long page_generation[WATCH_PAGE_COUNT];
static unsigned long watch_serial = 1;

static BOOL page_range(unsigned long address, unsigned long size, unsigned long *first, unsigned long *last)
{
	unsigned long start, end;

	if (!size || address < PLATFORM_CONTIGUOUS_BASE ||
		address - PLATFORM_CONTIGUOUS_BASE >= PLATFORM_CONTIGUOUS_SIZE)
		return FALSE;
	start = address - PLATFORM_CONTIGUOUS_BASE;
	end = start + size - 1;
	if (end >= PLATFORM_CONTIGUOUS_SIZE || end < start)
		end = PLATFORM_CONTIGUOUS_SIZE - 1;
	*first = start / WATCH_PAGE_SIZE;
	*last = end / WATCH_PAGE_SIZE;
	return TRUE;
}

static void mark_written(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, generation;

	if (!page_range(address, size, &first, &last))
		return;
	generation = __atomic_add_fetch(&watch_serial, 1, __ATOMIC_SEQ_CST);
	for (page = first; page <= last; page++)
		__atomic_store_n(&page_generation[page], generation, __ATOMIC_RELEASE);
}

void memory_watch_initialize(void)
{
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	(void)address; (void)size;
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, newest = 0;

	if (!page_range(address, size, &first, &last))
		return 0;
	for (page = first; page <= last; page++)
	{
		unsigned long generation = __atomic_load_n(&page_generation[page], __ATOMIC_ACQUIRE);

		if (generation > newest)
			newest = generation;
	}
	return newest;
}

unsigned long memory_watch_serial(void)
{
	return __atomic_load_n(&watch_serial, __ATOMIC_ACQUIRE);
}

void memory_watch_prepare_write(void *address, unsigned long size)
{
	mark_written((unsigned long)address, size);
}

void memory_watch_forget(void *address, unsigned long size)
{
	mark_written((unsigned long)address, size);
}
