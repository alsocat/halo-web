/*
GAME_STATE.C

symbols in this file:
001AF250 0010:
	_dummy (0000)
001AF260 0010:
	_code_001af260 (0000)
001AF270 0010:
	_code_001af270 (0000)
001AF280 0020:
	_code_001af280 (0000)
001AF2A0 0010:
	_game_state_dispose (0000)
001AF2B0 00b0:
	_game_state_initialize_for_new_map (0000)
001AF360 0010:
	_game_state_dispose_from_old_map (0000)
001AF370 0020:
	_game_state_save (0000)
001AF390 0040:
	_game_state_revert (0000)
001AF3D0 0040:
	_game_state_save_to_persistent_storage (0000)
001AF410 0070:
	_game_state_test_persistent_storage (0000)
001AF480 0050:
	_game_state_save_core (0000)
001AF4D0 0020:
	_game_state_reverted (0000)
001AF4F0 0160:
	_game_state_header_valid (0000)
001AF650 0070:
	_game_state_allocation_record (0000)
001AF6C0 0020:
	_code_001af6c0 (0000)
001AF6E0 0110:
	_game_state_malloc (0000)
001AF7F0 0110:
	_game_state_gpu_malloc (0000)
001AF900 0040:
	_game_state_data_new (0000)
001AF940 0040:
	_game_state_memory_pool_new (0000)
001AF980 0050:
	_game_state_lruv_cache_new (0000)
001AF9D0 00f0:
	_game_state_try_and_load_from_persistent_storage (0000)
001AFAC0 00a0:
	_game_state_load_core (0000)
001AFB60 0050:
	_game_state_initialize (0000)
002A7E7C 0013:
	??_C@_0BD@BHHOKLIG@error?5writing?5?8?$CFs?8?$AA@ (0000)
002A7E90 000b:
	??_C@_0L@MGAFKEJL@saved?5?8?$CFs?8?$AA@ (0000)
002A7E9C 0025:
	??_C@_0CF@NHHJOMMA@checksum?5from?5map?5file?5doesn?8t?5m@ (0000)
002A7EC4 0021:
	??_C@_0CB@ELFCALCI@expected?5?$CD?$CFd?5players?5but?5got?5?$CD?$CFd@ (0000)
002A7EE8 001d:
	??_C@_0BN@CGADBPLM@allocation?5checksum?5mismatch?$AA@ (0000)
002A7F08 001b:
	??_C@_0BL@PFDNBAEN@expected?5?$CC?$CFs?$CC?5but?5got?5?$CC?$CFs?$CC?$AA@ (0000)
002A7F24 001f:
	??_C@_0BP@FJPMMOPC@expected?5build?5?$CD?$CFd?5but?5got?5?$CD?$CFd?$AA@ (0000)
002A7F44 0028:
	??_C@_0CI@KECEBGOP@c?3?2halo?2SOURCE?2saved?5games?2game_@ (0000)
002A7F6C 0013:
	??_C@_0BD@JHLLDANP@?$CF?540s?$CF?520s?$CF?510d?$CFs?6?$AA@ (0000)
002A7F80 0011:
	??_C@_0BB@DCECPIDG@d?3?2gamestate?4txt?$AA@ (0000)
002A7F98 0041:
	??_C@_0EB@JIJEJKFB@game_state_globals?4cpu_allocatio@ (0000)
002A7FDC 001b:
	??_C@_0BL@IDCLCKDD@?$CBgame_state_globals?4locked?$AA@ (0000)
002A7FF8 000a:
	??_C@_09KFGLKCMD@?$CB?$CIsize?$CG3?$CJ?$AA@ (0000)
002A8008 0041:
	??_C@_0EB@OBCPKDFA@game_state_globals?4gpu_allocatio@ (0000)
002A804C 000b:
	??_C@_0L@CIHKLGI@data?5array?$AA@ (0000)
002A8058 000b:
	??_C@_0L@HOFDEGMG@lruv?5cache?$AA@ (0000)
002A8064 0013:
	??_C@_0BD@OOCHHAFI@couldn?8t?5open?5?8?$CFs?8?$AA@ (0000)
002A8078 000c:
	??_C@_0M@NNNFMOGK@loaded?5?8?$CFs?8?$AA@ (0000)
00316840 003c:
	_data_00316840 (0000)
004D27B0 0020:
	_bss_004d27b0 (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include <stdlib.h>
#include <string.h>
#include "cseries_windows.h"
#include "real_math.h"
#include "console.h"
#include "game_state.h"
#include "game.h"
#include "tag_files.h"
#include "cache_files.h"
#include "scenario.h"
#include "main.h"
#include "objects.h"
#include "memory/data.h"
#include "cutscene/cinematics.h"
#include "units/units.h"
#include "game_sound.h"
#include "sound_manager.h"
#include "observer.h"
#include "players.h"
#include "player_queues_new.h"
#include "rasterizer.h"
#include "recorded_animations.h"
#include "ai_debug.h"
#include "structures.h"
#include "director.h"
#include "hud_messaging.h"
#include "crc.h"
#include "data.h"
#include "lruv_cache.h"
#include "memory_pool.h"

/* ---------- constants */

enum
{
	/* the native builds' larger game state (halo_port_capacity.h) */
	GAME_STATE_CPU_SIZE = HALO_PORT_GAME_STATE_CPU_SIZE,
	GAME_STATE_GPU_SIZE = HALO_PORT_GAME_STATE_GPU_SIZE,
	GAME_STATE_SIZE = GAME_STATE_CPU_SIZE+GAME_STATE_GPU_SIZE
};

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes */

void dummy(
	void);
static boolean game_state_header_valid(
	struct game_state_header *header,
	boolean halt_on_error);
static void game_state_allocation_record(
	const char *name,
	const char *type,
	long size,
	boolean gpu);
#ifdef HALO_WEB
static void game_state_web_record(const char *name, byte *address, long size);
#endif
static void game_state_set_revert_time(
	void);

/* ---------- globals */

boolean recover_saved_games_hack;

static FILE* bss_004d27b0;

static struct
{
	void *base_address; // 0x0
	long cpu_allocation_size; // 0x4
	long gpu_allocation_size; // 0x8
	long allocation_size_checksum; // 0xC
	boolean locked; // 0x10
	boolean saved_game_valid; // 0x11
	long revert_time; // 0x14
	struct game_state_header *header; // 0x18
} game_state_globals = { 0 };

typedef void (*game_state_before_load_proc)(void);
typedef void (*game_state_after_load_proc)(void);
typedef void (*game_state_before_save_proc)(void);

static game_state_before_save_proc before_save_procs[] =
{
	dummy,
};

static game_state_before_load_proc before_load_procs[] =
{
	game_sound_clear,
};

static game_state_after_load_proc after_load_procs[] =
{
	scenario_reload_structure_bsp_if_necessary,
	sound_stop_all,
	game_sound_restore,
	observer_initialize_for_new_map,
	update_queues_reset_and_fill_with_lies,
	rasterizer_decals_update_function_pointers,
	recorded_animations_clear_debug_storage,
	ai_debug_initialize_for_new_map,
	structure_detail_objects_flush,
	game_state_set_revert_time,
	player_control_fix_for_loaded_game_state,
	director_initialize_for_saved_game,
	scripted_hud_messages_clear
};

/* ---------- public code */

void dummy(
	void)
{
	return;
}

static void game_state_call_before_save_procs(
	void)
{
	game_state_before_save_proc *proc = before_save_procs;
	long i;

	for (i =NUMBEROF(before_save_procs); i>0; i--, proc++)
	{
		(*proc)();
	}

	return;
}

static void game_state_call_before_load_procs(
	void)
{
	game_state_before_load_proc *proc = before_load_procs;
	long i;

	for (i =NUMBEROF(before_load_procs); i>0; i--, proc++)
	{
		(*proc)();
	}

	return;
}

static void game_state_call_after_load_procs(
	void)
{
	game_state_after_load_proc *proc = after_load_procs;
	long i;

	for (i =NUMBEROF(after_load_procs); i>0; i--, proc++)
	{
		(*proc)();
	}

	return;
}

void game_state_dispose(
	void)
{
	game_state_free_buffer();
	game_state_close_file();

	return;
}

void game_state_initialize_for_new_map(
	void)
{
	const char *name;

	game_state_globals.locked = TRUE;
	game_state_globals.saved_game_valid = FALSE;
	game_state_globals.revert_time = NONE;

	memset(game_state_globals.header, 0, sizeof(*game_state_globals.header));

	name = tag_get_name(global_scenario_index);
	strcpy(game_state_globals.header->map_name, name);
	strcpy(game_state_globals.header->build_number, "01.01.14.2342");

	game_state_globals.header->player_count = player_spawn_count;
	game_state_globals.header->difficulty = game_difficulty_level_get();
	game_state_globals.header->cache_file_checksum = cache_files_get_checksum();
	game_state_globals.header->allocation_size_checksum = game_state_globals.allocation_size_checksum;

	return;
}

void game_state_dispose_from_old_map(
	void)
{
	return;
}

void game_state_save(
	void)
{
	game_state_call_before_save_procs();

	main_stop_time();
	game_state_globals.saved_game_valid = (game_state_write_to_file()!=FALSE);
	main_start_time();

	return;
}

void game_state_revert(
	void)
{
	if (!game_state_globals.saved_game_valid && !recover_saved_games_hack)
	{
		main_reset_map();

		return;
	}

	game_state_call_before_load_procs();
	game_state_read_from_file();
	game_state_call_after_load_procs();

	return;
}

void game_state_save_to_persistent_storage(
	void)
{
	if (player_spawn_count==1)
	{
		game_state_revert();
		game_state_write_to_persistent_storage(
			game_state_globals.base_address,
			&game_state_globals.header->checksum,
			sizeof(*game_state_globals.header),
			/* the whole of the native builds' larger game state */
			GAME_STATE_SIZE);
	}

	return;
}

boolean game_state_test_persistent_storage(
	char *map_name,
	short *difficulty,
	boolean *corrupted)
{
	struct game_state_header header;
	boolean success;

	if (game_state_read_header_from_persistent_storage(
		&header,
		&header.checksum,
		sizeof(*game_state_globals.header),
		/* the whole of the native builds' larger game state */
		GAME_STATE_SIZE,
		corrupted))
	{
		*difficulty = header.difficulty;
		strcpy(map_name, header.map_name);

		success = TRUE;
	}
	else
	{
		*difficulty = _game_difficulty_level_normal;
		strcpy(map_name, "");

		success = FALSE;
	}

	return success;
}

void game_state_save_core(
	const char *name)
{
	/* the whole of the native builds' larger game state */
	if (game_state_write_core(name, game_state_globals.base_address, GAME_STATE_SIZE))
	{
		console_printf(FALSE, "saved '%s'", name);
	}
	else
	{
		console_printf(FALSE, "error writing '%s'", name);
	}

	return;
}

boolean game_state_reverted(
	void)
{
	return (game_state_globals.revert_time==game_time_get());
}

static boolean game_state_header_valid(
	struct game_state_header *header,
	boolean halt_on_error)
{
	boolean valid = FALSE;

	if (csstrcmp(header->map_name, tag_get_name(global_scenario_index)))
	{
		if (halt_on_error)
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state.c",
				409,
				FALSE,
				csprintf(temporary, "expected \"%s\" but got \"%s\"", tag_get_name(global_scenario_index), header->map_name));
		}
	}
	else if (header->allocation_size_checksum != game_state_globals.allocation_size_checksum)
	{
		if (halt_on_error)
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state.c",
				413,
				FALSE,
				csprintf(temporary, "allocation checksum mismatch"));
		}
	}
	else if (header->player_count != player_spawn_count)
	{
		if (halt_on_error)
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state.c",
				417,
				FALSE,
				csprintf(temporary, "expected #%d players but got #%d", player_spawn_count, header->player_count));
		}
	}
	else if (header->cache_file_checksum != cache_files_get_checksum())
	{
		if (halt_on_error)
		{
			match_vassert(
				"c:\\halo\\SOURCE\\saved games\\game_state.c",
				422,
				FALSE,
				csprintf(temporary, "checksum from map file doesn't match"));
		}
	}
	else
	{
		valid = TRUE;
	}

	return valid;
}

#ifdef HALO_WEB
/* the game state's parts, by name, for its fingerprints */
#define WEB_GAME_STATE_PARTS 256

static struct
{
	/* (copied: some parts are named by a string of the moment) */
	char name[48];
	byte *address;
	long size;
} web_game_state_parts[WEB_GAME_STATE_PARTS];
static long web_game_state_part_count;

static void game_state_web_record(
	const char *name,
	byte *address,
	long size)
{
	if (web_game_state_part_count < WEB_GAME_STATE_PARTS)
	{
		snprintf(web_game_state_parts[web_game_state_part_count].name,
			sizeof(web_game_state_parts[web_game_state_part_count].name), "%s", name ? name : "?");
		web_game_state_parts[web_game_state_part_count].address = address;
		web_game_state_parts[web_game_state_part_count].size = size;
		web_game_state_part_count++;
	}
}

void platform_log(const char *format, ...);

/* the game state's parts that are the machine's own, not the game's: what
it draws and plays (no tick reads them back), what each frame moves rather
than each tick (game_frame_update: particles, widgets, sounds), and the
clock's leftover time between ticks; each machine's may differ, and the
fingerprints leave them out */
static boolean game_state_web_part_own(
	const char *name)
{
	static const char *const own[] = {
		"cached object render states", "lights", "lights globals", "light",
		"decal vertex cache", "particle", "particle systems", "particle system particles",
		"contrail", "contrail point", "glow", "glow particles", "light volumes", "lightnings",
		"object looping sounds", "game sound globals", "motion sensor (radar)", "rumble",
		"screen effect filth", "rasterizer model ambient reflection tint",
		"decals", "decal globals", "decal vertices", "structure decals",
		/* (and what the game's frames move, game_frame_update: the widgets) */
		"widget", "flag", "antenna",
	};
	long index;

	for (index = 0; index < (long)NUMBEROF(own); index++)
	{
		if (!strcmp(name, own[index]))
			return TRUE;
	}
	return FALSE;
}

/* an object's checksum, without what is the machine's own in it: its mark
(the magic number an object search stamps, from a counter rendering uses
too), its location's unused word, its attachments in this machine's own
lights and sounds, its cached render state, and a unit's speech's sound (its index in this machine's sounds) and
illumination (from the lights, which each frame moves; nothing the game does
reads it) */
static void game_state_web_object_checksum(
	unsigned long *crc,
	struct object_header_datum *header)
{
	byte *datum = (byte *)header->datum;
	long own[16][2];
	long own_count = 0;
	long at = 0;
	long index;

	own[own_count][0] = offsetof(struct object_datum, object.magic_number);
	own[own_count++][1] = 4;
	/* (its location's bonus word, never read: a dropped item's mark, and
	whatever was on the stack for the locations computed there) */
	own[own_count][0] = offsetof(struct object_datum, object.location.bonus);
	own[own_count++][1] = sizeof(word);
	/* (its attachments in this machine's own lights, sounds, contrails and
	particle systems, whose slots each frame's updates take and free too) */
	{
		struct object_datum *object = (struct object_datum *)datum;
		long attachment;

		for (attachment = 0; attachment < (long)NUMBEROF(object->object.attachment_indices); attachment++)
		{
			if (object->object.attachment_types[attachment] != _object_attachment_type_effect &&
				object->object.attachment_types[attachment] != (char)NONE)
			{
				own[own_count][0] = offsetof(struct object_datum, object.attachment_indices) + attachment * 4;
				own[own_count++][1] = 4;
			}
		}
	}
	/* (and its cached render state, which drawing it sets) */
	own[own_count][0] = offsetof(struct object_datum, object.cached_render_state_index);
	own[own_count++][1] = 4;
	if (header->type == _object_type_biped || header->type == _object_type_vehicle)
	{
		own[own_count][0] = offsetof(struct unit_datum, unit.ambient_illumination);
		own[own_count++][1] = 2 * sizeof(real);
		own[own_count][0] = offsetof(struct unit_datum, unit.speech.impulse_sound_index);
		own[own_count++][1] = 4;
	}
	for (index = 0; index < own_count; index++)
	{
		crc_checksum_buffer(crc, datum + at, own[index][0] - at);
		at = own[index][0] + own[index][1];
	}
	crc_checksum_buffer(crc, datum + at, header->data_size - at);
}

/* a part's checksum, without what is the machine's own in it: the objects'
marks (the magic number an object search stamps, from a counter rendering
uses too), the fog's camera point, the clock's leftover time */
static void game_state_web_part_checksum(
	unsigned long *crc,
	long index)
{
	const char *name = web_game_state_parts[index].name;
	byte *address = web_game_state_parts[index].address;
	long size = web_game_state_parts[index].size;

	if (!strcmp(name, "objects"))
	{
		struct data_iterator iterator;
		struct object_header_datum *header;

		data_iterator_new(&iterator, object_header_data);
		while ((header = data_iterator_next(&iterator)) != NULL)
		{
			crc_checksum_buffer(crc, &header->type, sizeof(header->type));
			game_state_web_object_checksum(crc, header);
		}
	}
	else if (!strcmp(name, "scenario globals"))
	{
		/* (the map's structure; its fog and sound environment are where this
		machine's camera is) */
		crc_checksum_buffer(crc, address, offsetof(struct scenario_globals, atmospheric_fog));
	}
	else if (!strcmp(name, "effect"))
	{
		extern void effects_web_checksum(unsigned long *crc);

		effects_web_checksum(crc);
	}
	else if (!strcmp(name, "cinematic globals"))
	{
		/* (the letterbox's bars, which drawing them moves) */
		long after = offsetof(struct cinematic_global_data, show_letterbox);

		crc_checksum_buffer(crc, address + after, size - after);
	}
	else if (!strcmp(name, "game time globals"))
	{
		crc_checksum_buffer(crc, address, size - (long)sizeof(real));
	}
	else
	{
		crc_checksum_buffer(crc, address, size);
	}
}

/* the game's state's fingerprint (a checksum of what every machine's game
must have the same), and each part's, to the log (?HALO_LOCKSTEP_TEST: two
machines' compared) */
void game_state_web_fingerprint(
	long tick)
{
	static char line[16384];
	unsigned long whole;
	long index;
	int length = 0;

	crc_new(&whole);
	for (index = 0; index < web_game_state_part_count; index++)
	{
		if (!game_state_web_part_own(web_game_state_parts[index].name))
			game_state_web_part_checksum(&whole, index);
	}
	for (index = 0; index < web_game_state_part_count && length < (int)sizeof(line) - 64; index++)
	{
		unsigned long part;

		if (game_state_web_part_own(web_game_state_parts[index].name))
			continue;
		crc_new(&part);
		game_state_web_part_checksum(&part, index);
		length += snprintf(line + length, sizeof(line) - length, " %s=%08lx", web_game_state_parts[index].name, part);
	}
	/* ?HALO_LOCKSTEP_DUMP=<tick>: the whole game state at that tick, part by
	part, to z:\\lockstep_dump.bin, to compare machines' byte by byte */
	{
		char const *dump = getenv("HALO_LOCKSTEP_DUMP");

		/* (or several, comma separated: each overwrites the last but the
		last that differs is what's wanted) */
		char const *at = dump;
		boolean wanted = FALSE;

		while (at && *at)
		{
			if (atol(at) == tick)
				wanted = TRUE;
			at = strchr(at, ',');
			if (at)
				at++;
		}
		if (dump && wanted)
		{
			char dump_path[64];
			FILE *file;

			snprintf(dump_path, sizeof(dump_path), "z:\\lockstep_dump_%ld.bin", tick);
			file = fopen(dump_path, "wb");

			if (file)
			{
				for (index = 0; index < web_game_state_part_count; index++)
				{
					char name[64] = { 0 };

					strncpy(name, web_game_state_parts[index].name, sizeof(name) - 1);
					unsigned long address = (unsigned long)(size_t)web_game_state_parts[index].address;

					fwrite(name, 1, sizeof(name), file);
					fwrite(&address, 4, 1, file);
					fwrite(&web_game_state_parts[index].size, 4, 1, file);
					fwrite(web_game_state_parts[index].address, 1, web_game_state_parts[index].size, file);
				}
				fclose(file);
				platform_log("lockstep dump %ld written", tick);
			}
		}
	}
	/* each object's own checksum (its index, definition and checksum), to
	find which one first differs */
	{
		static char objects_line[65536];
		struct data_iterator iterator;
		struct object_header_datum *header;
		int objects_length = 0;

		data_iterator_new(&iterator, object_header_data);
		while ((header = data_iterator_next(&iterator)) != NULL && objects_length < (int)sizeof(objects_line) - 96)
		{
			byte *datum = (byte *)header->datum;
			unsigned long crc;

			crc_new(&crc);
			game_state_web_object_checksum(&crc, header);
			objects_length += snprintf(objects_line + objects_length, sizeof(objects_line) - objects_length,
				" %ld:%s=%08lx", (long)(iterator.datum_index & 0xFFFF), tag_get_name(*(long *)datum), crc);
		}
		platform_log("lockstep objects %ld:%s", tick, objects_line);
	}
	{
		extern long web_tick_local_random_draws;

		platform_log("lockstep draws %ld: %ld", tick, web_tick_local_random_draws);
	}
	platform_log("lockstep tick %ld: %08lx", tick, whole);
	platform_log("lockstep parts %ld:%s", tick, line);
}
#endif

static void game_state_allocation_record(
	const char *name,
	const char *type,
	long size,
	boolean gpu)
{
	// The January compiler inlines this logger into both arena allocators while
	// retaining one out-of-line copy under its private address-derived name.
	FILE *file = bss_004d27b0;

	if (!file)
	{
		file = fopen("d:\\gamestate.txt", "w");
		bss_004d27b0 = file;
	}

	if (file)
	{
		fprintf(file, "% 40s% 20s% 10d%s\n", name, type, size, gpu ? "*" : "");
		fflush(bss_004d27b0);
	}

	return;
}

static void game_state_set_revert_time(
	void)
{
	game_state_globals.revert_time = game_time_get();
	game_time_set_paused(FALSE);

	return;
}

void *game_state_malloc(
	const char *name,
	const char *type,
	long size)
{
	byte *pointer;

	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 153, !(size&3));
	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 156, !game_state_globals.locked);
	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 159, game_state_globals.cpu_allocation_size+size<=GAME_STATE_CPU_SIZE);

	game_state_allocation_record(name, type, size, FALSE);

	pointer = (byte *)game_state_globals.base_address+game_state_globals.cpu_allocation_size;
	game_state_globals.cpu_allocation_size+= size;
#ifdef HALO_WEB
	game_state_web_record(name, pointer, size);
#endif

	crc_checksum_buffer((unsigned long *)&game_state_globals.allocation_size_checksum, &size, sizeof(size));

	return pointer;
}

void *game_state_gpu_malloc(
	const char *name,
	const char *type,
	long size)
{
	byte *pointer;

	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 182, !(size&3));
	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 185, !game_state_globals.locked);
	match_assert("c:\\halo\\SOURCE\\saved games\\game_state.c", 188, game_state_globals.gpu_allocation_size+size<=GAME_STATE_GPU_SIZE);

	game_state_allocation_record(name, type, size, TRUE);

	game_state_globals.gpu_allocation_size+= size;
	pointer = (byte *)game_state_globals.base_address-game_state_globals.gpu_allocation_size+GAME_STATE_SIZE;
#ifdef HALO_WEB
	game_state_web_record(name, pointer, size);
#endif

	crc_checksum_buffer((unsigned long *)&game_state_globals.allocation_size_checksum, &size, sizeof(size));

	return pointer;
}

struct data_array *game_state_data_new(
	const char *name,
	short maximum_count,
	short size)
{
	struct data_array *data;

	data = game_state_malloc(name, "data array", data_allocation_size(maximum_count, size));
	data_initialize(data, name, maximum_count, size);

	return data;
}

struct memory_pool *game_state_memory_pool_new(
	const char *name,
	long size)
{
	struct memory_pool *pool;

	pool = game_state_malloc(name, "memory pool", memory_pool_allocation_size(size));
	memory_pool_initialize(pool, name, size);

	return pool;
}

struct lruv_cache *game_state_lruv_cache_new(
	const char *name,
	long page_count,
	long page_size_bits,
	long maximum_block_count,
	void (*delete_block_proc)(long),
	boolean (*locked_block_proc)(long))
{
	struct lruv_cache *cache;

	cache = game_state_malloc(name, "lruv cache", lruv_allocation_size(maximum_block_count));
	lruv_initialize(cache, name, page_count, page_size_bits, maximum_block_count, delete_block_proc, locked_block_proc);

	return cache;
}

void game_state_try_and_load_from_persistent_storage(
	void)
{
	struct game_state_header header;

	if (game_state_read_header_from_persistent_storage(
			&header,
			&header.checksum,
			sizeof(header),
			GAME_STATE_SIZE,
			NULL)
		&& game_state_header_valid(&header, FALSE)
		&& main_get_difficulty() == header.difficulty)
	{
		game_state_call_before_load_procs();
		game_state_read_from_persistent_storage(
			game_state_globals.base_address,
			GAME_STATE_SIZE);
		game_difficulty_level_set(main_get_difficulty());
		game_state_call_after_load_procs();
		game_state_save();
	}

	return;
}

void game_state_load_core(
	const char *name)
{
	struct game_state_header header;

	if (game_state_read_core_header(name, &header, sizeof(header))
		&& game_state_header_valid(&header, TRUE))
	{
		game_state_call_before_load_procs();
		game_state_read_core(
			name,
			game_state_globals.base_address,
			GAME_STATE_SIZE);
		console_printf(FALSE, "loaded '%s'", name);
		game_state_call_after_load_procs();
	}
	else
	{
		console_printf(FALSE, "couldn't open '%s'", name);
	}

	return;
}

void game_state_initialize(
	void)
{
	crc_new(&game_state_globals.allocation_size_checksum);
	/* the native builds place their larger game state above the tag cache
	(halo_port_capacity.h, cache/physical_memory_map.c) */
	game_state_globals.base_address = game_state_allocate_buffer(HALO_PORT_GAME_STATE_BASE_ADDRESS, GAME_STATE_CPU_SIZE, GAME_STATE_GPU_SIZE);
	game_state_create_or_open_file();
	game_state_globals.header = game_state_malloc("header", NULL, sizeof(*game_state_globals.header));

	return;
}

/* ---------- private code */
