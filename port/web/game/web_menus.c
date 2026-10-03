/*
WEB_MENUS.C

The site's changes to the game's menus (ui.map), made in its loaded tag data
rather than in the map file.

ONLINE PLAY: the multiplayer menu's SYSTEM LINK PLAY is ONLINE PLAY, which
goes straight to the list of games (ONLINE GAMES) as the site's player: the
profile last used (else the first), without the four-way profile screens.
Games are found and joined over the site's network (port/web/shell/net.js),
and netcode v2 joins games in progress.

The creator: on the site's first visit (no player profile yet) the game opens
on a short profile screen (name, colour, save) with the name being typed,
then the main menu; the profile is the player's from then on.

?HALO_WEB_DUMP_UI=1 logs every menu widget of the loaded map: its name, text,
event handlers (what each button does and the widget it opens) and children,
the map of the menus the changes below are made against.
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "game/players.h"
#include "interface/event_manager.h"
#include "interface/player_ui.h"
#include "interface/virtual_keyboard.h"
#include "saved games/player_profile.h"
#include "saved games/saved_game_files.h"
#include "tag_files/tag_files.h"
#include "text/text_group.h"

#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...);
boolean ui_widget_event_handler_function_invoke(struct widget_instance *widget, struct event_record *event,
	word function_index, boolean *widget_deleted);
boolean web_menus_event_function(struct widget_instance *widget, struct event_record *event,
	word function_index, boolean *widget_deleted);
boolean web_menus_load_first_screen(void);
void main_screen_shell_load(void);
boolean filesystem_check_thread_is_active(void);
struct player_profile *player_ui_get_edit_player_profile(void);
struct widget_instance *ui_widget_load_by_name_or_tag(char const *name, long tag_index, struct widget_instance *parent,
	short local_player_index, long invoking_widget_tag, long focused_child_parent_widget_tag, short focused_child_index);

/* ---------- the widget tag ('DeLa'): interface/ui_widget.c's view of it */

enum
{
	WEB_UI_WIDGET_DEFINITION_TAG = 'DeLa'
};

struct web_ui_event_handler
{
	long flags;
	short event_type;
	short function;
	struct tag_reference widget_tag;
	struct tag_reference sound_effect;
	char script[32];
};

struct web_ui_child
{
	struct tag_reference widget_tag;
	char name[32];
	long flags;
	short custom_controller_index;
	short vertical_offset;
	short horizontal_offset;
	byte unknown03A[0x50 - 0x3A];
};

struct web_ui_widget
{
	short type;
	short controller_index;
	char name[32];
	rectangle2d bounds;
	long flags;
	long milliseconds_to_auto_close;
	long auto_close_fade_time;
	struct tag_reference background_bitmap;
	struct tag_block game_data_inputs;
	struct tag_block event_handlers;
	struct tag_block search_and_replace_functions;
	byte unknown06C[0xEC - 0x6C];
	struct tag_reference text_label_string_list;
	struct tag_reference text_font;
	real_argb_color text_color;
	short justification;
	word text_box_flags;
	byte unknown120[0x12E - 0x120];
	short string_list_index;
	short horizontal_offset;
	short vertical_offset;
	byte unknown134[0x150 - 0x134];
	long list_flags;
	byte unknown154[0x2D4 - 0x154];
	struct tag_block conditional_widgets;
	byte unknown2E0[0x3E0 - 0x2E0];
	struct tag_block child_widgets;
};

typedef char web_ui_widget_size_check[sizeof(struct web_ui_widget) == 0x3EC ? 1 : -1];
typedef char web_ui_event_handler_size_check[sizeof(struct web_ui_event_handler) == 0x48 ? 1 : -1];
typedef char web_ui_child_size_check[sizeof(struct web_ui_child) == 0x50 ? 1 : -1];

static void string_text(long string_list_index, short string_index, char *text, int size);

/* ---------- the changes */

enum
{
	/* the site's own event handler functions, after the game's 102
	(interface/ui_widget_event_handler_functions.c hands them here) */
	WEB_FUNCTION_ONLINE_PLAY = 200,
	WEB_FUNCTION_CREATOR_SAVE,

	/* the game's */
	FUNCTION_PLAYER_PROFILE_SAVE_CHANGES = 67,
	FUNCTION_DISPLAY_ERROR_IF_NO_NETWORK_CONNECTION = 91,


	UI_WIDGET_TYPE_TEXT_BOX = 1
};

#define MULTIPLAYER_SCREEN "ui\\shell\\main_menu\\multiplayer_type_select\\"
#define CONN_ITEM MULTIPLAYER_SCREEN "multiplayer_type_conn_item"
#define OPTIONS_STRINGS MULTIPLAYER_SCREEN "multiplayer_options"
#define DESCRIPTION_STRINGS MULTIPLAYER_SCREEN "multiplayer_option_descriptions"
#define SERVER_LIST_SCREEN MULTIPLAYER_SCREEN "connected\\server_list\\server_list_screen"
#define FOUND_GAMES_HEADER MULTIPLAYER_SCREEN "connected\\server_list\\header_found_games"
#define LARGE_FONT "ui\\large_ui"
#define PROFILE_EDIT "ui\\shell\\main_menu\\settings_select\\player_setup\\"
#define CREATOR_SCREEN PROFILE_EDIT "create_and_edit_player_profile_screen"
#define PROFILE_EDIT_LIST PROFILE_EDIT "player_profile_edit\\profile_edit_select_list"
#define PROFILE_SAVE_ITEM PROFILE_EDIT "player_profile_edit\\save_changes_profile_item"

enum
{
	CONN_STRING_INDEX = 3
};

/* the strings' text: the game writes their terminators (text/text_group.c) */
static wchar_t online_play_text[] = L"ONLINE PLAY";
static wchar_t online_play_description[] =
	L"Play with friends online. Join any game in progress, or create your own.";
static wchar_t online_games_text[] = L"ONLINE GAMES";

/* the multiplayer strings with ONLINE GAMES added (for the header) */
#define WEB_MAXIMUM_STRINGS 32
static struct string_list_entry options_strings[WEB_MAXIMUM_STRINGS];

static void set_string(struct string_list_entry *entry, wchar_t *text)
{
	long length = 0;

	/* (wcslen is the C library's, of 32-bit characters: the game's are 16) */
	while (text[length])
		length++;
	entry->string.size = (long)((length + 1) * sizeof(wchar_t));
	entry->string.address = text;
}

/* the changes, made to the loaded ui.map (again whenever it is loaded) */
static void apply_changes(void)
{
	long conn_index = tag_loaded(WEB_UI_WIDGET_DEFINITION_TAG, CONN_ITEM);
	long screen_index = tag_loaded(WEB_UI_WIDGET_DEFINITION_TAG, SERVER_LIST_SCREEN);
	long header_index = tag_loaded(WEB_UI_WIDGET_DEFINITION_TAG, FOUND_GAMES_HEADER);
	long options_index = tag_loaded(UNICODE_STRING_LIST_TAG, OPTIONS_STRINGS);
	long descriptions_index = tag_loaded(UNICODE_STRING_LIST_TAG, DESCRIPTION_STRINGS);
	long font_index = tag_loaded('font', LARGE_FONT);
	struct web_ui_widget *conn;
	struct web_ui_widget *header;
	struct string_list *options;
	struct string_list *descriptions;
	long index;

	if (conn_index == NONE || screen_index == NONE || header_index == NONE || options_index == NONE ||
		descriptions_index == NONE || font_index == NONE)
	{
		return;
	}
	conn = tag_get(WEB_UI_WIDGET_DEFINITION_TAG, conn_index);
	header = tag_get(WEB_UI_WIDGET_DEFINITION_TAG, header_index);
	options = tag_get(UNICODE_STRING_LIST_TAG, options_index);
	descriptions = tag_get(UNICODE_STRING_LIST_TAG, descriptions_index);
	if (conn->event_handlers.count && ((struct web_ui_event_handler *)conn->event_handlers.address)->function == WEB_FUNCTION_ONLINE_PLAY)
		return;
	if (options->strings.count <= CONN_STRING_INDEX || options->strings.count >= WEB_MAXIMUM_STRINGS)
	{
		platform_log("web menus: unexpected multiplayer strings");
		return;
	}

	/* SYSTEM LINK PLAY: ONLINE PLAY, straight to the games */
	for (index = 0; index < conn->event_handlers.count; index++)
	{
		struct web_ui_event_handler *handler = (struct web_ui_event_handler *)conn->event_handlers.address + index;

		handler->function = WEB_FUNCTION_ONLINE_PLAY;
		handler->widget_tag.index = screen_index;
	}
	memcpy(options_strings, options->strings.address, options->strings.count * sizeof(struct string_list_entry));
	set_string(&options_strings[CONN_STRING_INDEX], online_play_text);
	set_string(&options_strings[options->strings.count], online_games_text);
	options->strings.address = options_strings;
	/* (its description: the one of linked Xboxes) */
	for (index = 0; index < descriptions->strings.count; index++)
	{
		char text[64];

		string_text(descriptions_index, (short)index, text, sizeof(text));
		if (!strncmp(text, "Connect up to four Xbox", 23))
			set_string((struct string_list_entry *)descriptions->strings.address + index, online_play_description);
	}

	/* FOUND GAMES (a picture): ONLINE GAMES, in the menus' font */
	header->type = UI_WIDGET_TYPE_TEXT_BOX;
	header->background_bitmap.index = NONE;
	header->text_label_string_list.group_tag = UNICODE_STRING_LIST_TAG;
	header->text_label_string_list.index = options_index;
	header->string_list_index = (short)options->strings.count;
	header->text_font.group_tag = 'font';
	header->text_font.index = font_index;
	header->text_color.alpha = 1.0f;
	header->text_color.red = 0.16f;
	header->text_color.green = 0.59f;
	header->text_color.blue = 1.0f;
	header->horizontal_offset = 13;
	header->vertical_offset = 30;
	options->strings.count++;
	platform_log("web menus: online play");
}

/* the site's player's profile: the one last used, else the first there is */
static long site_profile(struct player_profile *profile)
{
	static long profiles[512];
	long profile_index = player_ui_get_player1_last_used_profile_index();
	word count = NUMBEROF(profiles);
	word index;

	if (profile_index != NONE && player_profile_get(profile_index, profile))
		return profile_index;
	/* (the count is the list's size, and then the profiles') */
	player_profiles_enumerate_available_to_local_player_index(0, &count, profiles, FALSE);
	if (!count)
	{
		count = NUMBEROF(profiles);
		player_profiles_enumerate_available_to_local_player_index(0, &count, profiles, TRUE);
	}
	for (index = 0; index < count; index++)
	{
		if (player_profile_get(profiles[index], profile))
			return profiles[index];
	}
	return NONE;
}

/* ONLINE PLAY: the network checked as SYSTEM LINK PLAY does; the player
joins as its profile (the four-way screens' qtr_screen_start2join and
qtr_screen_profile_select do this) */
static boolean online_play(struct widget_instance *widget, struct event_record *event, boolean *widget_deleted)
{
	struct player_profile profile;
	short controller_index = event && event->controller_index != NONE ? event->controller_index : 0;
	long profile_index;

	if (!ui_widget_event_handler_function_invoke(widget, event, FUNCTION_DISPLAY_ERROR_IF_NO_NETWORK_CONNECTION, widget_deleted))
		return FALSE;
	profile_index = site_profile(&profile);
	if (profile_index == NONE)
	{
		platform_log("web menus: no player profile");
		return FALSE;
	}
	if (!player_name_valid(profile.player_name, NUMBEROF(profile.player_name)))
	{
		/* (the creator names it) */
		platform_log("web menus: the profile's name can't be used in multiplayer");
	}
	player_ui_local_player_joined_multiplayer_game(controller_index);
	player_ui_set_active_player_profile(controller_index, profile_index, &profile);
	if (controller_index == 0)
		player_ui_remember_player1_profile(TRUE);
	return TRUE;
}

/* ---------- the creator */

/* the profile screen as the creator, while it is up: its list only the
name, colour and save, and save going on to the main menu (on the next
frame, as the game loads it: the save item's handler opens the saving screen
first) */
#define CREATOR_MAXIMUM_ITEMS 8
#define CREATOR_MAXIMUM_HANDLERS 4

static struct
{
	boolean launched;
	boolean decide_pending;
	boolean keyboard_pending;
	boolean restore_pending;
	long profile_index;
	/* the changed tags, and what they were */
	struct web_ui_widget *list;
	struct tag_block list_children;
	struct web_ui_widget *save_item;
	struct web_ui_event_handler save_handlers[CREATOR_MAXIMUM_HANDLERS];
	struct web_ui_child items[CREATOR_MAXIMUM_ITEMS];
} creator = { FALSE, FALSE, FALSE, FALSE, NONE };

static void creator_restore(void)
{
	if (creator.list)
	{
		creator.list->child_widgets = creator.list_children;
		memcpy(creator.save_item->event_handlers.address, creator.save_handlers,
			creator.save_item->event_handlers.count * sizeof(struct web_ui_event_handler));
		creator.list = NULL;
		creator.save_item = NULL;
	}
	creator.restore_pending = FALSE;
}

/* the profile screen as the creator */
static boolean creator_change_screen(void)
{
	long list_index = tag_loaded(WEB_UI_WIDGET_DEFINITION_TAG, PROFILE_EDIT_LIST);
	long save_index = tag_loaded(WEB_UI_WIDGET_DEFINITION_TAG, PROFILE_SAVE_ITEM);
	struct web_ui_widget *list;
	struct web_ui_widget *save_item;
	long index;
	long count = 0;

	if (list_index == NONE || save_index == NONE)
		return FALSE;
	list = tag_get(WEB_UI_WIDGET_DEFINITION_TAG, list_index);
	save_item = tag_get(WEB_UI_WIDGET_DEFINITION_TAG, save_index);
	if (list->child_widgets.count > CREATOR_MAXIMUM_ITEMS || save_item->event_handlers.count > CREATOR_MAXIMUM_HANDLERS)
		return FALSE;
	for (index = 0; index < list->child_widgets.count; index++)
	{
		struct web_ui_child *child = (struct web_ui_child *)list->child_widgets.address + index;

		if (!strcmp(child->name, "name_profile_item") || !strcmp(child->name, "color_profile_item") ||
			!strcmp(child->name, "save_changes_profile_item"))
		{
			creator.items[count++] = *child;
		}
	}
	if (count != 3)
		return FALSE;
	creator.list = list;
	creator.list_children = list->child_widgets;
	creator.save_item = save_item;
	memcpy(creator.save_handlers, save_item->event_handlers.address,
		save_item->event_handlers.count * sizeof(struct web_ui_event_handler));
	list->child_widgets.count = count;
	list->child_widgets.address = creator.items;
	for (index = 0; index < save_item->event_handlers.count; index++)
	{
		struct web_ui_event_handler *handler = (struct web_ui_event_handler *)save_item->event_handlers.address + index;

		handler->function = WEB_FUNCTION_CREATOR_SAVE;
	}
	return TRUE;
}

static boolean has_player_profile(void)
{
	static long profiles[512];
	word count = NUMBEROF(profiles);

	/* (the count is the list's size, and then the profiles') */
	player_profiles_enumerate_available_to_local_player_index(0, &count, profiles, FALSE);
	return count > 0;
}

/* save: the profile saved as the profile editor does, and the player's */
static boolean creator_save(struct widget_instance *widget, struct event_record *event, boolean *widget_deleted)
{
	struct player_profile profile;

	if (!ui_widget_event_handler_function_invoke(widget, event, FUNCTION_PLAYER_PROFILE_SAVE_CHANGES, widget_deleted))
		return FALSE;
	if (creator.profile_index != NONE && player_profile_get(creator.profile_index, &profile))
	{
		player_ui_set_active_player_profile(0, creator.profile_index, &profile);
		player_ui_remember_player1_profile(TRUE);
	}
	player_ui_end_editing_profile();
	/* (the handler running this is the save item's, which is put back
	afterwards: web_menus_frame, which then loads the main menu) */
	creator.restore_pending = TRUE;
	platform_log("web menus: the player's profile is made");
	return TRUE;
}

/* ---------- private code */

static char const *tag_name_or_none(long tag_index)
{
	return tag_index == NONE ? "-" : tag_get_name(tag_index);
}

/* a string list's string as ASCII, for the log */
static void string_text(long string_list_index, short string_index, char *text, int size)
{
	wchar_t const *string = NULL;
	int length = 0;

	text[0] = 0;
	if (string_list_index == NONE || string_index < 0)
		return;
	string = unicode_string_list_get_string(string_list_index, string_index);
	if (!string)
		return;
	while (string[length] && length < size - 1)
	{
		text[length] = string[length] < 128 ? (char)string[length] : '?';
		length++;
	}
	text[length] = 0;
}

static void dump_widgets(void)
{
	struct tag_iterator iterator;
	long tag_index;
	long count = 0;

	tag_iterator_new(&iterator, WEB_UI_WIDGET_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct web_ui_widget *widget = tag_get(WEB_UI_WIDGET_DEFINITION_TAG, tag_index);
		char text[96];
		long index;

		string_text(widget->text_label_string_list.index, widget->string_list_index, text, sizeof(text));
		platform_log("ui %ld %s | type %d name '%s' text [%s#%d] '%s' handlers %ld children %ld",
			tag_index, tag_get_name(tag_index), widget->type, widget->name,
			tag_name_or_none(widget->text_label_string_list.index), widget->string_list_index, text,
			widget->event_handlers.count, widget->child_widgets.count);
		for (index = 0; index < widget->event_handlers.count; index++)
		{
			struct web_ui_event_handler *handler = (struct web_ui_event_handler *)widget->event_handlers.address + index;

			platform_log("ui   handler %ld: flags %lx event %d function %d widget %s script '%s'",
				index, (unsigned long)handler->flags, handler->event_type, handler->function,
				tag_name_or_none(handler->widget_tag.index), handler->script);
		}
		for (index = 0; index < widget->child_widgets.count; index++)
		{
			struct web_ui_child *child = (struct web_ui_child *)widget->child_widgets.address + index;

			platform_log("ui   child %ld: %s '%s'", index, tag_name_or_none(child->widget_tag.index), child->name);
		}
		count++;
	}
	platform_log("ui: %ld widgets", count);
	/* the strings that name system link */
	tag_iterator_new(&iterator, UNICODE_STRING_LIST_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct string_list *list = tag_get(UNICODE_STRING_LIST_TAG, tag_index);
		short index;

		for (index = 0; index < list->strings.count; index++)
		{
			char text[160];

			string_text(tag_index, index, text, sizeof(text));
			if (strstr(text, "YSTEM LINK") || strstr(text, "ystem link") || strstr(text, "ystem Link"))
				platform_log("ui string %s#%d: '%s'", tag_get_name(tag_index), index, text);
		}
	}
	/* the headers' pictures */
	tag_iterator_new(&iterator, WEB_UI_WIDGET_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct web_ui_widget *widget = tag_get(WEB_UI_WIDGET_DEFINITION_TAG, tag_index);

		if (strstr(tag_get_name(tag_index), "header") || widget->text_font.index != NONE)
			platform_log("ui look %s: bounds %d %d %d %d flags %lx bitmap %s font %s colour %.2f %.2f %.2f %.2f just %d tbf %x off %d %d",
				tag_get_name(tag_index), widget->bounds.v[0], widget->bounds.v[1], widget->bounds.v[2], widget->bounds.v[3],
				(unsigned long)widget->flags, tag_name_or_none(widget->background_bitmap.index), tag_name_or_none(widget->text_font.index),
				widget->text_color.alpha, widget->text_color.red, widget->text_color.green, widget->text_color.blue,
				widget->justification, widget->text_box_flags, widget->horizontal_offset, widget->vertical_offset);
	}
}

/* ---------- public code */

/* the site's event handler functions (WEB_FUNCTION_...) */
boolean web_menus_event_function(struct widget_instance *widget, struct event_record *event,
	word function_index, boolean *widget_deleted)
{
	switch (function_index)
	{
	case WEB_FUNCTION_ONLINE_PLAY:
		return online_play(widget, event, widget_deleted);
	case WEB_FUNCTION_CREATOR_SAVE:
		return creator_save(widget, event, widget_deleted);
	}
	platform_log("web menus: no function %d", function_index);
	return FALSE;
}

/* the creator, where there is no player profile yet */
static boolean creator_open(void)
{
	struct player_profile *profile;
	wchar_t name[128];

	creator.launched = TRUE;
	if (has_player_profile())
		return FALSE;
	saved_game_file_get_useable_untitled_profile_name(name);
	if (!name[0])
		return FALSE;
	creator.profile_index = player_profile_new(0, name);
	if (creator.profile_index == NONE)
		return FALSE;
	player_ui_begin_editing_profile(creator.profile_index);
	profile = player_ui_get_edit_player_profile();
	if (!profile || !creator_change_screen())
	{
		player_ui_end_editing_profile();
		return FALSE;
	}
	ustrncpy(profile->player_name, name, MAXIMUM_PLAYER_PROFILE_NAME_LENGTH - 1);
	profile->player_name[MAXIMUM_PLAYER_PROFILE_NAME_LENGTH - 1] = 0;
	if (!ui_widget_load_by_name_or_tag(CREATOR_SCREEN, NONE, NULL, NONE, NONE, NONE, NONE))
	{
		creator_restore();
		player_ui_end_editing_profile();
		return FALSE;
	}
	/* the name first; the keyboard is made after the main menu would have
	been, so it opens on the next frame */
	creator.keyboard_pending = TRUE;
	platform_log("web menus: the creator");
	return TRUE;
}

/* the main menu's loading (interface/ui_widget.c main_screen_shell_load):
TRUE when the creator opens instead, on the site's first visit, or nothing
until the saved profiles are known (the game finds them on a thread of its
own as it starts: web_menus_frame loads the menu again then) */
boolean web_menus_load_first_screen(void)
{
	creator_restore();
	if (creator.launched)
		return FALSE;
	if (filesystem_check_thread_is_active())
	{
		creator.decide_pending = TRUE;
		return TRUE;
	}
	return creator_open();
}

/* each frame (port/web/src/web_platform.c): the dump once the menus are up */
void web_menus_frame(void)
{
	static int frames;
	static int dumped;
	char const *dump;

	if (creator.decide_pending && !filesystem_check_thread_is_active())
	{
		creator.decide_pending = FALSE;
		main_screen_shell_load();
	}
	if (creator.keyboard_pending && !virtual_keyboard_active())
	{
		struct player_profile *profile = player_ui_get_edit_player_profile();

		creator.keyboard_pending = FALSE;
		if (profile && !virtual_keyboard_launch(profile->player_name, sizeof(profile->player_name), 8))
			platform_log("web menus: no keyboard for the name");
	}
	if (creator.restore_pending)
	{
		creator_restore();
		main_screen_shell_load();
	}
	/* (the ui.map is loaded again on returning from a game) */
	if (frames++ % 15 == 0)
		apply_changes();
	if (dumped || frames < 120)
		return;
	dumped = 1;
	dump = getenv("HALO_WEB_DUMP_UI");
	if (dump && *dump == '1')
		dump_widgets();
}
