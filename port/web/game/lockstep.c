/*
LOCKSTEP.C

Online co-op: friends on their own machines in one campaign, each machine
running the whole game (lockstep). Every machine's game is the same from the
same players' actions (the game is deterministic in the browser: the game's
fixes are marked "lockstep play"), so the machines share only actions.

The machine that started the campaign is the session's host; a campaign in
progress is open to friends (advertised on the site's network: the ONLINE
GAMES list shows it, port/web/game/web_menus.c). The host decides each
tick's frame, every player's action for it, and sends it to the guests, who
send it their players' actions as they tick; every machine ticks the frames
in order, one a frame at most, so that what the main loop does between
ticks (a checkpoint's revert, a cinematic skipped, the next level) happens
after the same tick on every one.

A guest joins at any time: the host gives it the game state at a tick (the
whole of it, as a checkpoint is saved, compressed), what the game keeps
outside it (the random seed, the scripted camera, the scripted sounds'
times), and every frame from that tick on, the first of which adds its
player. The guest loads the level, the state, and catches up.

Every two seconds each guest sends the host its game state's hash
(game_state_web_hash, without what is each machine's own: what it draws,
plays and its local players'); one out of step is given the host's state
again. A new level (the last won) waits for every machine to load it.

The messages go over a TCP connection from each guest to the host (the
site's network in the browser: port/web/src/posix_net_web.c), and the
session's advertisement as UDP broadcasts.
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "camera/camera_scripting.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "memory/data.h"
#include "memory/zlib/zlib.h"
#include "networking/network_game_globals.h"
#include "saved games/player_profile.h"
#include "sound/sound_definitions.h"
#include "tag_files/tag_files.h"

#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...);

/* ---------- the browser's sockets (port/linux/src/posix.h) */

int posix_socket_last_error(void);
int posix_socket(int family, int type, int protocol);
int posix_socket_close(int socket);
int posix_socket_bind(int socket, const void *address, int address_length);
int posix_socket_listen(int socket, int backlog);
int posix_socket_accept(int socket, void *address, int *address_length);
int posix_socket_connect(int socket, const void *address, int address_length);
int posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address, int address_length);
int posix_socket_send(int socket, const void *buffer, int length, int flags);
int posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address, int *address_length);
int posix_socket_recv(int socket, void *buffer, int length, int flags);
int posix_socket_set_nonblocking(int socket, int nonblocking);
int posix_socket_set_nodelay(int socket);
int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length);

/* sockaddr_in, as the sockets take it */
struct lockstep_address
{
	unsigned short family;
	unsigned short port;
	unsigned long address;
	byte zero[8];
};

enum
{
	SOCKET_AF_INET = 2,
	SOCKET_STREAM = 1,
	SOCKET_DGRAM = 2,
	SOCKET_SOL_SOCKET = 0xffff,
	SOCKET_SO_BROADCAST = 0x0020,
	SOCKET_WOULDBLOCK = 10035,

	LOCKSTEP_TCP_PORT = 2310,
	LOCKSTEP_UDP_PORT = 2311,
	LOCKSTEP_VERSION = 1,
	LOCKSTEP_MAGIC = 0x504F4F43, /* 'COOP' */

	LOCKSTEP_MAXIMUM_MACHINES = 4,
	LOCKSTEP_MAXIMUM_PLAYERS = 4,
	LOCKSTEP_MAXIMUM_SESSIONS = 8,
	LOCKSTEP_HASH_PERIOD = 60,
	LOCKSTEP_HASH_HISTORY = 64,
	LOCKSTEP_INPUT_QUEUE = 8,
	LOCKSTEP_ADVERTISE_MILLISECONDS = 1000,
	LOCKSTEP_SESSION_TIMEOUT_MILLISECONDS = 4000,
	LOCKSTEP_READY_TIMEOUT_MILLISECONDS = 60000,

	/* the messages */
	_message_hello = 1,		/* guest: name, colour */
	_message_welcome,		/* host: the session, the state, then frames */
	_message_frame,			/* host: a tick's actions (and players added) */
	_message_input,			/* guest: its players' actions */
	_message_hash,			/* guest: its state's hash at a tick */
	_message_ready,			/* guest: it has loaded the level */
	_message_resync,		/* host: the state again, then frames */
	_message_bye,

	_command_add_player = 1,
};

/* a player of the session: which machine plays it, as which of its local
players, and (once made) its player */
struct lockstep_member
{
	short machine;
	short local_player_index;
	wchar_t name[12];
	short color;
	long player_index;
};

/* a byte buffer (a connection's outgoing and incoming bytes, a message) */
struct lockstep_buffer
{
	byte *data;
	long size;
	long capacity;
	long start;
};

struct lockstep_machine
{
	boolean connected;
	int socket;
	struct lockstep_buffer in;
	struct lockstep_buffer out;
	/* (the host's view of a guest) */
	boolean welcomed;
	boolean ready;
	long ready_epoch;
	struct player_action inputs[LOCKSTEP_INPUT_QUEUE][LOCKSTEP_MAXIMUM_PLAYERS];
	unsigned long input_flags[LOCKSTEP_INPUT_QUEUE];
	long input_count;
	struct player_action last_input[LOCKSTEP_MAXIMUM_PLAYERS];
	long resync_requested;
	long in_step_since;
};

/* a session seen on the network */
struct lockstep_session
{
	unsigned long address;
	unsigned long seen;
	wchar_t host_name[12];
	char map_name[64];
	short difficulty;
	short player_count;
};

/* a frame a guest has yet to tick */
struct lockstep_frame
{
	long epoch;
	long tick;
	unsigned long flags;
	short command_count;
	struct lockstep_member commands[LOCKSTEP_MAXIMUM_PLAYERS];
	short action_count;
	short player_indices[LOCKSTEP_MAXIMUM_PLAYERS];
	struct player_action actions[LOCKSTEP_MAXIMUM_PLAYERS];
};

enum
{
	_role_none,
	_role_host,
	_role_guest,
};

enum
{
	_guest_connecting,
	_guest_waiting_for_welcome,
	_guest_loading,
	_guest_playing,
};

static struct
{
	short role;
	short machine;
	long epoch;
	struct lockstep_member members[LOCKSTEP_MAXIMUM_PLAYERS];
	short member_count;

	/* host */
	int listen_socket;
	struct lockstep_machine machines[LOCKSTEP_MAXIMUM_MACHINES];
	struct lockstep_member pending_commands[LOCKSTEP_MAXIMUM_PLAYERS];
	short pending_command_count;
	unsigned long hashes[LOCKSTEP_HASH_HISTORY];
	long hash_ticks[LOCKSTEP_HASH_HISTORY];
	unsigned long barrier_started;
	unsigned long last_advertised;
	boolean in_session;

	/* guest */
	short guest_state;
	struct lockstep_machine host;
	struct lockstep_frame *frames;
	long frame_count;
	long frame_capacity;
	byte *snapshot;
	long snapshot_size;
	byte *side;
	long side_size;
	long snapshot_tick;
	char map_name[64];
	short difficulty;
	boolean join_loading;

	/* everyone */
	int udp_socket;
	struct lockstep_session sessions[LOCKSTEP_MAXIMUM_SESSIONS];
	boolean udp_open;
} lockstep = { _role_none, 0, 0 };

/* the game's (port and source) */
long game_state_web_snapshot_size(void);
void game_state_web_snapshot(byte *buffer);
boolean game_state_web_restore(byte const *buffer, long size);
unsigned long game_state_web_hash(void);
long camera_script_web_state_size(void);
void camera_script_web_get_state(void *buffer);
void camera_script_web_set_state(void const *buffer);
void update_client_web_local_action(short local_player_index, struct player_action *action);
unsigned long player_control_web_take_action_flags(void);
void player_control_web_apply_action_flags(unsigned long flags);
unsigned long get_random_seed(void);
unsigned long *get_global_random_seed_address(void);
void dispose_global_network_game_client(void);
void dispose_global_network_game_server(void);
boolean web_lockstep_join(short index);
void local_player_set_player_index(short local_player_index, long player_index);

/* ---------- buffers */

static void buffer_reserve(struct lockstep_buffer *buffer, long size)
{
	if (buffer->start && buffer->start == buffer->size)
	{
		buffer->start = 0;
		buffer->size = 0;
	}
	if (buffer->size + size > buffer->capacity)
	{
		long capacity = buffer->capacity ? buffer->capacity : 4096;

		if (buffer->start)
		{
			memmove(buffer->data, buffer->data + buffer->start, buffer->size - buffer->start);
			buffer->size -= buffer->start;
			buffer->start = 0;
		}
		while (buffer->size + size > capacity)
			capacity *= 2;
		if (capacity != buffer->capacity)
		{
			byte *data = malloc(capacity);

			if (buffer->data)
			{
				memcpy(data, buffer->data, buffer->size);
				free(buffer->data);
			}
			buffer->data = data;
			buffer->capacity = capacity;
		}
	}
}

static void buffer_put(struct lockstep_buffer *buffer, void const *data, long size)
{
	buffer_reserve(buffer, size);
	memcpy(buffer->data + buffer->size, data, size);
	buffer->size += size;
}

static void buffer_free(struct lockstep_buffer *buffer)
{
	if (buffer->data)
		free(buffer->data);
	memset(buffer, 0, sizeof(*buffer));
}

/* a message: its type and length, then what it says */
static void message_begin(struct lockstep_buffer *message, long type)
{
	unsigned long header[2];

	message->size = 0;
	message->start = 0;
	header[0] = (unsigned long)type;
	header[1] = 0;
	buffer_put(message, header, sizeof(header));
}

static void message_send(struct lockstep_machine *machine, struct lockstep_buffer *message)
{
	((unsigned long *)message->data)[1] = (unsigned long)(message->size - 8);
	if (machine->connected)
		buffer_put(&machine->out, message->data, message->size);
}

/* what is left to send, sent as the socket takes it */
static void machine_flush(struct lockstep_machine *machine)
{
	while (machine->connected && machine->out.size > machine->out.start)
	{
		int sent = posix_socket_send(machine->socket, machine->out.data + machine->out.start,
			(int)MIN(machine->out.size - machine->out.start, 65536), 0);

		if (sent < 0)
		{
			if (posix_socket_last_error() != SOCKET_WOULDBLOCK)
			{
				platform_log("lockstep: a connection failed (%d)", posix_socket_last_error());
				machine->connected = FALSE;
			}
			break;
		}
		if (sent == 0)
			break;
		machine->out.start += sent;
	}
}

/* what has come in */
static void machine_receive(struct lockstep_machine *machine)
{
	byte chunk[16384];

	while (machine->connected)
	{
		int received = posix_socket_recv(machine->socket, chunk, sizeof(chunk), 0);

		if (received < 0)
		{
			if (posix_socket_last_error() != SOCKET_WOULDBLOCK)
			{
				platform_log("lockstep: a connection closed (%d)", posix_socket_last_error());
				machine->connected = FALSE;
			}
			break;
		}
		if (received == 0)
		{
			platform_log("lockstep: a connection closed");
			machine->connected = FALSE;
			break;
		}
		buffer_put(&machine->in, chunk, received);
	}
}

/* the next whole message come in, or FALSE */
static boolean machine_next_message(struct lockstep_machine *machine, long *type, byte **data, long *size)
{
	long available = machine->in.size - machine->in.start;
	unsigned long header[2];

	if (available < 8)
		return FALSE;
	memcpy(header, machine->in.data + machine->in.start, 8);
	if (available < 8 + (long)header[1])
		return FALSE;
	*type = (long)header[0];
	*data = machine->in.data + machine->in.start + 8;
	*size = (long)header[1];
	machine->in.start += 8 + (long)header[1];
	return TRUE;
}

static void machine_close(struct lockstep_machine *machine)
{
	if (machine->socket > 0)
		posix_socket_close(machine->socket);
	buffer_free(&machine->in);
	buffer_free(&machine->out);
	memset(machine, 0, sizeof(*machine));
}

/* a reader of a message */
struct lockstep_reader
{
	byte const *data;
	long size;
	long at;
	boolean failed;
};

static void read_bytes(struct lockstep_reader *reader, void *data, long size)
{
	if (reader->at + size > reader->size)
	{
		reader->failed = TRUE;
		memset(data, 0, size);
		return;
	}
	memcpy(data, reader->data + reader->at, size);
	reader->at += size;
}

static long read_long(struct lockstep_reader *reader)
{
	long value;

	read_bytes(reader, &value, sizeof(value));
	return value;
}

static short read_short(struct lockstep_reader *reader)
{
	short value;

	read_bytes(reader, &value, sizeof(value));
	return value;
}

static void put_long(struct lockstep_buffer *message, long value)
{
	buffer_put(message, &value, sizeof(value));
}

static void put_short(struct lockstep_buffer *message, short value)
{
	buffer_put(message, &value, sizeof(value));
}

/* ---------- the session's players */

static void put_member(struct lockstep_buffer *message, struct lockstep_member const *member)
{
	put_short(message, member->machine);
	put_short(message, member->local_player_index);
	buffer_put(message, member->name, sizeof(member->name));
	put_short(message, member->color);
	put_long(message, member->player_index);
}

static void read_member(struct lockstep_reader *reader, struct lockstep_member *member)
{
	member->machine = read_short(reader);
	member->local_player_index = read_short(reader);
	read_bytes(reader, member->name, sizeof(member->name));
	member->color = read_short(reader);
	member->player_index = read_long(reader);
}

/* a member's player made, as every machine makes it (in the same order) */
static void member_make_player(struct lockstep_member *member)
{
	struct network_player network_player;
	boolean mine = member->machine == lockstep.machine;
	long player_index;

	memset(&network_player, 0, sizeof(network_player));
	memcpy(network_player.name, member->name, sizeof(network_player.name));
	network_player.primary_color_index = member->color;
	network_player.machine_index = (char)member->machine;
	network_player.controller_index = (char)member->local_player_index;
	player_index = player_new(member->machine, NONE, mine ? member->local_player_index : NONE, &network_player);
	member->player_index = player_index;
	if (mine && player_index != NONE)
		local_player_set_player_index(member->local_player_index, player_index);
}

/* this machine's players its local players, the others' none (as after
another machine's state is loaded) */
static void members_set_local_players(void)
{
	short local_player_index;
	short index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		players_globals->local_players[local_player_index] = NONE;
	for (index = 0; index < lockstep.member_count; index++)
	{
		struct lockstep_member *member = &lockstep.members[index];
		struct player_datum *player;

		if (member->player_index == NONE || !(player = datum_try_and_get(player_data, member->player_index)))
			continue;
		player->local_player_index = NONE;
		if (member->machine == lockstep.machine)
			local_player_set_player_index(member->local_player_index, member->player_index);
	}
}

/* ---------- this machine's name and colour */

static void my_name(wchar_t *name, short *color)
{
	struct player_profile profile;

	memset(name, 0, 12 * sizeof(wchar_t));
	*color = 0;
	if (player_ui_get_active_player_profile_index(0) != NONE)
	{
		player_ui_get_active_player_profile(0, &profile);
		memcpy(name, profile.player_name, 11 * sizeof(wchar_t));
		*color = profile.primary_color_index;
	}
	if (!name[0])
		memcpy(name, L"Player", 7 * sizeof(wchar_t));
}

/* ---------- what the game keeps outside its state */

static long side_size(void)
{
	return 8 + camera_script_web_state_size() + 4 + 4096 * 8;
}

/* the random seed, the player count, the scripted camera, and the sounds
scripts have started (their times, in their tags) */
static long side_write(byte *side)
{
	struct tag_iterator iterator;
	long tag_index;
	long size = 0;
	long sounds = 0;
	long count_at;

	*(unsigned long *)(side + size) = get_random_seed();
	size += 4;
	*(long *)(side + size) = player_spawn_count;
	size += 4;
	camera_script_web_get_state(side + size);
	size += camera_script_web_state_size();
	count_at = size;
	size += 4;
	tag_iterator_new(&iterator, SOUND_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE && sounds < 4096)
	{
		struct sound_definition *definition = sound_definition_get(tag_index);

		if (definition->scripting_time != NONE)
		{
			*(long *)(side + size) = tag_index;
			*(unsigned long *)(side + size + 4) = definition->scripting_time;
			size += 8;
			sounds++;
		}
	}
	*(long *)(side + count_at) = sounds;
	return size;
}

static void side_read(byte const *side, long size)
{
	struct tag_iterator iterator;
	long tag_index;
	long at = 0;
	long sounds;
	long index;

	if (size < 12 + camera_script_web_state_size())
		return;
	*get_global_random_seed_address() = *(unsigned long const *)side;
	player_spawn_count = (short)*(long const *)(side + 4);
	at = 8;
	camera_script_web_set_state(side + at);
	at += camera_script_web_state_size();
	sounds = *(long const *)(side + at);
	at += 4;
	tag_iterator_new(&iterator, SOUND_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
		sound_definition_get(tag_index)->scripting_time = NONE;
	for (index = 0; index < sounds && at + 8 <= size; index++, at += 8)
	{
		long sound_index = *(long const *)(side + at);

		if (sound_index != NONE)
			sound_definition_get(sound_index)->scripting_time = *(unsigned long const *)(side + at + 4);
	}
}

/* ---------- the host */

static boolean host_level_in_progress(void)
{
	return game_in_progress() && !main_menu_is_active() && game_connection() == _game_connection_local &&
		!game_engine_running() && main_get_current_solo_level() != NONE;
}

static void host_open(void)
{
	struct lockstep_address address;

	lockstep.listen_socket = posix_socket(SOCKET_AF_INET, SOCKET_STREAM, 0);
	if (lockstep.listen_socket < 0)
		return;
	memset(&address, 0, sizeof(address));
	address.family = SOCKET_AF_INET;
	address.port = (unsigned short)((LOCKSTEP_TCP_PORT >> 8) | ((LOCKSTEP_TCP_PORT & 0xff) << 8));
	if (posix_socket_bind(lockstep.listen_socket, &address, sizeof(address)) < 0 ||
		posix_socket_listen(lockstep.listen_socket, 4) < 0)
	{
		platform_log("lockstep: can't host (%d)", posix_socket_last_error());
		posix_socket_close(lockstep.listen_socket);
		lockstep.listen_socket = 0;
		return;
	}
	posix_socket_set_nonblocking(lockstep.listen_socket, 1);
	lockstep.role = _role_host;
	lockstep.machine = 0;
	lockstep.in_session = FALSE;
	platform_log("lockstep: hosting");
}

static void host_close(void)
{
	short index;

	for (index = 1; index < LOCKSTEP_MAXIMUM_MACHINES; index++)
	{
		if (lockstep.machines[index].connected)
		{
			struct lockstep_buffer message = { 0 };

			message_begin(&message, _message_bye);
			message_send(&lockstep.machines[index], &message);
			machine_flush(&lockstep.machines[index]);
			buffer_free(&message);
		}
		machine_close(&lockstep.machines[index]);
	}
	if (lockstep.listen_socket > 0)
		posix_socket_close(lockstep.listen_socket);
	lockstep.listen_socket = 0;
	lockstep.role = _role_none;
	lockstep.in_session = FALSE;
	lockstep.member_count = 0;
	lockstep.pending_command_count = 0;
	platform_log("lockstep: no longer hosting");
}

/* the session begins with the first guest: this machine's players are its
first members */
static void host_begin_session(void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	lockstep.member_count = 0;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL && lockstep.member_count < LOCKSTEP_MAXIMUM_PLAYERS)
	{
		struct lockstep_member *member = &lockstep.members[lockstep.member_count++];

		member->machine = 0;
		member->local_player_index = player->local_player_index != NONE ? player->local_player_index : 0;
		memcpy(member->name, player->name, sizeof(member->name));
		member->color = 0;
		member->player_index = iterator.datum_index;
	}
	lockstep.in_session = TRUE;
	lockstep.epoch = 1;
	memset(lockstep.hash_ticks, 0xff, sizeof(lockstep.hash_ticks));
	platform_log("lockstep: session begins with %d players here", lockstep.member_count);
}

/* the state, the session and the frames to come, to a guest (joining, or
out of step) at this tick */
static void host_send_state(short machine_index, long type)
{
	struct lockstep_machine *machine = &lockstep.machines[machine_index];
	struct lockstep_buffer message = { 0 };
	long raw_size = game_state_web_snapshot_size();
	byte *raw = malloc(raw_size);
	/* (zlib 1.1's bound: a thousandth more, and 12 bytes) */
	uLongf packed_size = raw_size + raw_size / 1000 + 64;
	byte *packed = malloc(packed_size);
	byte *side = malloc(side_size());
	long side_length;
	char const *map_name = main_get_solo_level_name(main_get_current_solo_level());
	char map[64] = { 0 };
	short index;
	unsigned long started = system_milliseconds();

	game_state_web_snapshot(raw);
	if (compress2(packed, &packed_size, raw, raw_size, 1) != Z_OK)
	{
		platform_log("lockstep: couldn't pack the game state");
		free(raw);
		free(packed);
		free(side);
		return;
	}
	side_length = side_write(side);
	if (map_name)
		strncpy(map, map_name, sizeof(map) - 1);
	message_begin(&message, type);
	put_short(&message, machine_index);
	put_long(&message, lockstep.epoch);
	put_long(&message, game_time_get());
	put_short(&message, game_difficulty_level_get());
	buffer_put(&message, map, sizeof(map));
	put_short(&message, lockstep.member_count);
	for (index = 0; index < lockstep.member_count; index++)
		put_member(&message, &lockstep.members[index]);
	put_long(&message, side_length);
	buffer_put(&message, side, side_length);
	put_long(&message, raw_size);
	put_long(&message, (long)packed_size);
	buffer_put(&message, packed, (long)packed_size);
	message_send(machine, &message);
	buffer_free(&message);
	free(raw);
	free(packed);
	free(side);
	machine->welcomed = TRUE;
	machine->ready = TRUE;
	/* (it has this level: the host waits for it at the next) */
	machine->ready_epoch = lockstep.epoch;
	machine->input_count = 0;
	machine->in_step_since = game_time_get();
	platform_log("lockstep: gave machine %d the game at tick %ld (%ld KB, %lu ms)",
		machine_index, game_time_get(), (long)(packed_size / 1024), system_milliseconds() - started);
}

/* a guest's hello: a member to add at the next tick */
static void host_hello(short machine_index, struct lockstep_reader *reader)
{
	struct lockstep_member member;
	short version = read_short(reader);

	if (reader->failed || version != LOCKSTEP_VERSION ||
		lockstep.member_count + lockstep.pending_command_count >= LOCKSTEP_MAXIMUM_PLAYERS)
	{
		platform_log("lockstep: machine %d can't join (version %d, %d players)", machine_index, version, lockstep.member_count);
		lockstep.machines[machine_index].connected = FALSE;
		return;
	}
	memset(&member, 0, sizeof(member));
	member.machine = machine_index;
	member.local_player_index = 0;
	read_bytes(reader, member.name, sizeof(member.name));
	member.name[11] = 0;
	member.color = read_short(reader);
	member.player_index = NONE;
	if (!lockstep.in_session)
		host_begin_session();
	host_send_state(machine_index, _message_welcome);
	lockstep.pending_commands[lockstep.pending_command_count++] = member;
	platform_log("lockstep: machine %d joins", machine_index);
}

static void host_receive(short machine_index)
{
	struct lockstep_machine *machine = &lockstep.machines[machine_index];
	long type;
	byte *data;
	long size;

	machine_receive(machine);
	while (machine_next_message(machine, &type, &data, &size))
	{
		struct lockstep_reader reader = { data, size, 0, FALSE };

		switch (type)
		{
		case _message_hello:
			host_hello(machine_index, &reader);
			break;
		case _message_input:
		{
			long epoch = read_long(&reader);
			unsigned long flags = (unsigned long)read_long(&reader);
			short count = read_short(&reader);
			short index;
			struct player_action actions[LOCKSTEP_MAXIMUM_PLAYERS];

			memset(actions, 0, sizeof(actions));
			for (index = 0; index < count && index < LOCKSTEP_MAXIMUM_PLAYERS; index++)
				read_bytes(&reader, &actions[index], sizeof(actions[index]));
			if (reader.failed || epoch != lockstep.epoch)
				break;
			if (machine->input_count == LOCKSTEP_INPUT_QUEUE)
			{
				/* (too far behind: the oldest go, their buttons kept) */
				flags |= machine->input_flags[0];
				memmove(&machine->inputs[0], &machine->inputs[1], sizeof(machine->inputs[0]) * (LOCKSTEP_INPUT_QUEUE - 1));
				memmove(&machine->input_flags[0], &machine->input_flags[1], sizeof(machine->input_flags[0]) * (LOCKSTEP_INPUT_QUEUE - 1));
				machine->input_count--;
			}
			memcpy(machine->inputs[machine->input_count], actions, sizeof(actions));
			machine->input_flags[machine->input_count] = flags;
			machine->input_count++;
			break;
		}
		case _message_hash:
		{
			long epoch = read_long(&reader);
			long tick = read_long(&reader);
			unsigned long hash = (unsigned long)read_long(&reader);
			long slot = (tick / LOCKSTEP_HASH_PERIOD) % LOCKSTEP_HASH_HISTORY;

			if (reader.failed || epoch != lockstep.epoch || tick < machine->in_step_since)
				break;
			if (lockstep.hash_ticks[slot] == tick)
			{
				if (lockstep.hashes[slot] != hash)
				{
					platform_log("lockstep: machine %d out of step at tick %ld (%08lx, here %08lx): giving it the game again",
						machine_index, tick, hash, lockstep.hashes[slot]);
					machine->resync_requested = TRUE;
				}
				else if (tick % (LOCKSTEP_HASH_PERIOD * 5) == 0)
				{
					platform_log("lockstep: machine %d in step at tick %ld", machine_index, tick);
				}
			}
			break;
		}
		case _message_ready:
		{
			long epoch = read_long(&reader);

			machine->ready_epoch = epoch;
			platform_log("lockstep: machine %d has loaded the level (%ld)", machine_index, epoch);
			break;
		}
		case _message_bye:
			machine->connected = FALSE;
			break;
		}
	}
}

/* every guest has loaded the level (or been waited for long enough) */
static boolean host_all_ready(void)
{
	short index;

	for (index = 1; index < LOCKSTEP_MAXIMUM_MACHINES; index++)
	{
		struct lockstep_machine *machine = &lockstep.machines[index];

		if (machine->connected && machine->welcomed && machine->ready_epoch < lockstep.epoch)
		{
			if (system_milliseconds() - lockstep.barrier_started < LOCKSTEP_READY_TIMEOUT_MILLISECONDS)
				return FALSE;
		}
	}
	return TRUE;
}

static void host_update(void)
{
	short index;

	if (!host_level_in_progress())
	{
		/* (the session ends at the main menu; between its levels it waits) */
		if (lockstep.role == _role_host &&
			(main_menu_is_active() || game_engine_running() || game_connection() != _game_connection_local))
		{
			host_close();
		}
		return;
	}
	if (lockstep.role == _role_none)
		host_open();
	if (lockstep.role != _role_host)
		return;

	/* new guests */
	for (;;)
	{
		struct lockstep_address address;
		int length = sizeof(address);
		int socket = posix_socket_accept(lockstep.listen_socket, &address, &length);

		if (socket < 0)
		{
			static unsigned long last_logged;

			if (posix_socket_last_error() != SOCKET_WOULDBLOCK && system_milliseconds() - last_logged > 5000)
			{
				last_logged = system_milliseconds();
				platform_log("lockstep: accept failed (%d)", posix_socket_last_error());
			}
			break;
		}
		for (index = 1; index < LOCKSTEP_MAXIMUM_MACHINES && lockstep.machines[index].connected; index++)
			;
		if (index == LOCKSTEP_MAXIMUM_MACHINES)
		{
			posix_socket_close(socket);
			continue;
		}
		machine_close(&lockstep.machines[index]);
		lockstep.machines[index].connected = TRUE;
		lockstep.machines[index].socket = socket;
		posix_socket_set_nonblocking(socket, 1);
		posix_socket_set_nodelay(socket);
		platform_log("lockstep: machine %d connects", index);
	}
	for (index = 1; index < LOCKSTEP_MAXIMUM_MACHINES; index++)
	{
		struct lockstep_machine *machine = &lockstep.machines[index];

		if (!machine->connected)
		{
			if (machine->socket > 0)
			{
				platform_log("lockstep: machine %d left", index);
				machine_close(machine);
			}
			continue;
		}
		host_receive(index);
		if (machine->connected && machine->resync_requested)
		{
			machine->resync_requested = FALSE;
			host_send_state(index, _message_resync);
		}
	}
}

/* the host's frame for this tick: its own players' actions, each guest's
next (else its last), the players to add */
static void host_build_frame(struct lockstep_frame *frame)
{
	struct player_action local[LOCKSTEP_MAXIMUM_PLAYERS];
	struct lockstep_buffer message = { 0 };
	short index;
	short local_player_index;

	memset(frame, 0, sizeof(*frame));
	frame->epoch = lockstep.epoch;
	frame->tick = game_time_get();
	frame->flags = player_control_web_take_action_flags();
	for (local_player_index = 0; local_player_index < LOCKSTEP_MAXIMUM_PLAYERS; local_player_index++)
		update_client_web_local_action(local_player_index, &local[local_player_index]);
	for (index = 1; index < LOCKSTEP_MAXIMUM_MACHINES; index++)
	{
		struct lockstep_machine *machine = &lockstep.machines[index];

		if (machine->connected && machine->input_count > 0)
		{
			memcpy(machine->last_input, machine->inputs[0], sizeof(machine->last_input));
			frame->flags |= machine->input_flags[0];
			memmove(&machine->inputs[0], &machine->inputs[1], sizeof(machine->inputs[0]) * (LOCKSTEP_INPUT_QUEUE - 1));
			memmove(&machine->input_flags[0], &machine->input_flags[1], sizeof(machine->input_flags[0]) * (LOCKSTEP_INPUT_QUEUE - 1));
			machine->input_count--;
		}
		else if (!machine->connected)
		{
			/* (a guest gone: its players stand still) */
			memset(machine->last_input, 0, sizeof(machine->last_input));
		}
	}
	for (index = 0; index < lockstep.member_count; index++)
	{
		struct lockstep_member *member = &lockstep.members[index];
		struct player_action *action = &frame->actions[frame->action_count];

		if (member->player_index == NONE)
			continue;
		if (member->machine == 0)
			*action = local[member->local_player_index];
		else
			*action = lockstep.machines[member->machine].last_input[member->local_player_index];
		frame->player_indices[frame->action_count++] = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(member->player_index);
	}
	frame->command_count = lockstep.pending_command_count;
	memcpy(frame->commands, lockstep.pending_commands, sizeof(lockstep.pending_commands[0]) * lockstep.pending_command_count);
	lockstep.pending_command_count = 0;

	/* to every guest welcomed */
	message_begin(&message, _message_frame);
	put_long(&message, frame->epoch);
	put_long(&message, frame->tick);
	put_long(&message, (long)frame->flags);
	put_short(&message, frame->command_count);
	for (index = 0; index < frame->command_count; index++)
		put_member(&message, &frame->commands[index]);
	put_short(&message, frame->action_count);
	for (index = 0; index < frame->action_count; index++)
	{
		put_short(&message, frame->player_indices[index]);
		buffer_put(&message, &frame->actions[index], sizeof(frame->actions[index]));
	}
	for (index = 1; index < LOCKSTEP_MAXIMUM_MACHINES; index++)
	{
		if (lockstep.machines[index].connected && lockstep.machines[index].welcomed)
			message_send(&lockstep.machines[index], &message);
	}
	buffer_free(&message);
}

/* ---------- the guest */

static void guest_leave(char const *why)
{
	if (lockstep.role != _role_guest)
		return;
	platform_log("lockstep: left the session: %s", why);
	if (lockstep.host.connected)
	{
		struct lockstep_buffer message = { 0 };

		message_begin(&message, _message_bye);
		message_send(&lockstep.host, &message);
		machine_flush(&lockstep.host);
		buffer_free(&message);
	}
	machine_close(&lockstep.host);
	if (lockstep.frames)
		free(lockstep.frames);
	lockstep.frames = NULL;
	lockstep.frame_count = 0;
	lockstep.frame_capacity = 0;
	if (lockstep.snapshot)
		free(lockstep.snapshot);
	lockstep.snapshot = NULL;
	if (lockstep.side)
		free(lockstep.side);
	lockstep.side = NULL;
	lockstep.role = _role_none;
	lockstep.member_count = 0;
	if (game_in_progress() && !main_menu_is_active())
		main_goto_main_menu();
}

/* the host's state (on joining, or again), to load once the level is */
static void guest_state(struct lockstep_reader *reader, boolean joining)
{
	char map[64];
	short member_count;
	short index;
	long raw_size;
	long packed_size;
	uLongf unpacked;

	lockstep.machine = read_short(reader);
	lockstep.epoch = read_long(reader);
	lockstep.snapshot_tick = read_long(reader);
	lockstep.difficulty = read_short(reader);
	read_bytes(reader, map, sizeof(map));
	map[sizeof(map) - 1] = 0;
	member_count = read_short(reader);
	for (index = 0; index < member_count && index < LOCKSTEP_MAXIMUM_PLAYERS; index++)
		read_member(reader, &lockstep.members[index]);
	lockstep.member_count = MIN(member_count, LOCKSTEP_MAXIMUM_PLAYERS);
	lockstep.side_size = read_long(reader);
	if (lockstep.side)
		free(lockstep.side);
	lockstep.side = malloc(MAX(lockstep.side_size, 1));
	read_bytes(reader, lockstep.side, lockstep.side_size);
	raw_size = read_long(reader);
	packed_size = read_long(reader);
	if (reader->failed || raw_size <= 0 || packed_size <= 0 || reader->at + packed_size > reader->size)
	{
		guest_leave("a broken game state");
		return;
	}
	if (lockstep.snapshot)
		free(lockstep.snapshot);
	lockstep.snapshot = malloc(raw_size);
	unpacked = raw_size;
	if (uncompress(lockstep.snapshot, &unpacked, reader->data + reader->at, packed_size) != Z_OK || (long)unpacked != raw_size)
	{
		guest_leave("a game state that would not unpack");
		return;
	}
	lockstep.snapshot_size = raw_size;
	/* (the frames from before it are no use) */
	lockstep.frame_count = 0;
	platform_log("lockstep: the host's game at tick %ld: %s, %d players", lockstep.snapshot_tick, map, lockstep.member_count);
	if (joining)
	{
		strncpy(lockstep.map_name, map, sizeof(lockstep.map_name) - 1);
		lockstep.guest_state = _guest_loading;
		lockstep.join_loading = TRUE;
		/* the level, as starting it from the menus does */
		dispose_global_network_game_client();
		dispose_global_network_game_server();
		player_spawn_count = lockstep.member_count;
		player_ui_set_single_player_local_player_controller(0, 0);
		main_set_difficulty(lockstep.difficulty);
		main_set_map_name(lockstep.map_name);
		game_connection_set(_game_connection_local);
		main_menu_switch_to_single_player();
	}
}

/* the state given, in place of this machine's */
static void guest_load_state(void)
{
	if (!lockstep.snapshot)
		return;
	if (!game_state_web_restore(lockstep.snapshot, lockstep.snapshot_size))
	{
		guest_leave("a game state of another size (another build?)");
		return;
	}
	side_read(lockstep.side, lockstep.side_size);
	members_set_local_players();
	if (lockstep.snapshot)
		free(lockstep.snapshot);
	lockstep.snapshot = NULL;
	lockstep.guest_state = _guest_playing;
	platform_log("lockstep: playing from tick %ld", game_time_get());
}

static void guest_frame_message(struct lockstep_reader *reader)
{
	struct lockstep_frame frame;
	short index;

	memset(&frame, 0, sizeof(frame));
	frame.epoch = read_long(reader);
	frame.tick = read_long(reader);
	frame.flags = (unsigned long)read_long(reader);
	frame.command_count = read_short(reader);
	for (index = 0; index < frame.command_count && index < LOCKSTEP_MAXIMUM_PLAYERS; index++)
		read_member(reader, &frame.commands[index]);
	frame.action_count = read_short(reader);
	for (index = 0; index < frame.action_count && index < LOCKSTEP_MAXIMUM_PLAYERS; index++)
	{
		frame.player_indices[index] = read_short(reader);
		read_bytes(reader, &frame.actions[index], sizeof(frame.actions[index]));
	}
	if (reader->failed)
		return;
	if (lockstep.frame_count == lockstep.frame_capacity)
	{
		lockstep.frame_capacity = lockstep.frame_capacity ? 2 * lockstep.frame_capacity : 1024;
		struct lockstep_frame *frames = malloc(lockstep.frame_capacity * sizeof(*lockstep.frames));

		if (lockstep.frames)
		{
			memcpy(frames, lockstep.frames, lockstep.frame_count * sizeof(*lockstep.frames));
			free(lockstep.frames);
		}
		lockstep.frames = frames;
	}
	lockstep.frames[lockstep.frame_count++] = frame;
}

static void guest_update(void)
{
	long type;
	byte *data;
	long size;

	machine_receive(&lockstep.host);
	while (machine_next_message(&lockstep.host, &type, &data, &size))
	{
		struct lockstep_reader reader = { data, size, 0, FALSE };

		switch (type)
		{
		case _message_welcome:
			guest_state(&reader, TRUE);
			break;
		case _message_resync:
			guest_state(&reader, FALSE);
			if (lockstep.guest_state == _guest_playing)
				lockstep.guest_state = _guest_loading;
			break;
		case _message_frame:
			guest_frame_message(&reader);
			break;
		case _message_bye:
			guest_leave("the host left");
			return;
		}
		if (lockstep.role != _role_guest)
			return;
	}
	if (!lockstep.host.connected)
	{
		guest_leave("the connection to the host closed");
		return;
	}
	/* the state, once the level is loaded (a resync's at once) */
	if (lockstep.guest_state == _guest_loading && lockstep.snapshot && !lockstep.join_loading &&
		game_in_progress() && !main_menu_is_active())
	{
		guest_load_state();
	}
	machine_flush(&lockstep.host);
}

/* the frame for this tick, which the guest has (the ones before it gone) */
static struct lockstep_frame *guest_frame(void)
{
	long tick = game_time_get();
	long index;
	long first = 0;

	while (first < lockstep.frame_count &&
		(lockstep.frames[first].epoch < lockstep.epoch ||
			(lockstep.frames[first].epoch == lockstep.epoch && lockstep.frames[first].tick < tick)))
	{
		first++;
	}
	if (first)
	{
		memmove(lockstep.frames, lockstep.frames + first, (lockstep.frame_count - first) * sizeof(*lockstep.frames));
		lockstep.frame_count -= first;
	}
	for (index = 0; index < lockstep.frame_count; index++)
	{
		if (lockstep.frames[index].epoch == lockstep.epoch && lockstep.frames[index].tick == tick)
			return &lockstep.frames[index];
	}
	return NULL;
}

/* ---------- discovery */

static void udp_open(void)
{
	struct lockstep_address address;
	int yes = 1;

	if (lockstep.udp_open)
		return;
	lockstep.udp_socket = posix_socket(SOCKET_AF_INET, SOCKET_DGRAM, 0);
	if (lockstep.udp_socket < 0)
		return;
	memset(&address, 0, sizeof(address));
	address.family = SOCKET_AF_INET;
	address.port = (unsigned short)((LOCKSTEP_UDP_PORT >> 8) | ((LOCKSTEP_UDP_PORT & 0xff) << 8));
	posix_socket_setsockopt(lockstep.udp_socket, SOCKET_SOL_SOCKET, SOCKET_SO_BROADCAST, &yes, sizeof(yes));
	if (posix_socket_bind(lockstep.udp_socket, &address, sizeof(address)) < 0)
	{
		posix_socket_close(lockstep.udp_socket);
		return;
	}
	posix_socket_set_nonblocking(lockstep.udp_socket, 1);
	lockstep.udp_open = TRUE;
}

struct lockstep_advertisement
{
	unsigned long magic;
	short version;
	short player_count;
	wchar_t host_name[12];
	char map_name[64];
	short difficulty;
	short open;
};

static void advertise(void)
{
	struct lockstep_advertisement advertisement;
	struct lockstep_address address;
	char const *map_name = main_get_solo_level_name(main_get_current_solo_level());
	short color;

	if (system_milliseconds() - lockstep.last_advertised < LOCKSTEP_ADVERTISE_MILLISECONDS)
		return;
	lockstep.last_advertised = system_milliseconds();
	memset(&advertisement, 0, sizeof(advertisement));
	advertisement.magic = LOCKSTEP_MAGIC;
	advertisement.version = LOCKSTEP_VERSION;
	advertisement.player_count = lockstep.in_session ? lockstep.member_count : local_player_count();
	my_name(advertisement.host_name, &color);
	if (map_name)
		strncpy(advertisement.map_name, map_name, sizeof(advertisement.map_name) - 1);
	advertisement.difficulty = game_difficulty_level_get();
	advertisement.open = advertisement.player_count < LOCKSTEP_MAXIMUM_PLAYERS;
	memset(&address, 0, sizeof(address));
	address.family = SOCKET_AF_INET;
	address.port = (unsigned short)((LOCKSTEP_UDP_PORT >> 8) | ((LOCKSTEP_UDP_PORT & 0xff) << 8));
	address.address = 0xffffffffUL;
	posix_socket_sendto(lockstep.udp_socket, &advertisement, sizeof(advertisement), 0, &address, sizeof(address));
}

static void discover(void)
{
	for (;;)
	{
		struct lockstep_advertisement advertisement;
		struct lockstep_address from;
		int length = sizeof(from);
		int received = posix_socket_recvfrom(lockstep.udp_socket, &advertisement, sizeof(advertisement), 0, &from, &length);
		short index;
		short slot = NONE;

		if (received < 0)
			break;
		if (received != sizeof(advertisement) || advertisement.magic != LOCKSTEP_MAGIC ||
			advertisement.version != LOCKSTEP_VERSION || (lockstep.role == _role_host && from.address == 0))
		{
			continue;
		}
		for (index = 0; index < LOCKSTEP_MAXIMUM_SESSIONS; index++)
		{
			if (lockstep.sessions[index].seen && lockstep.sessions[index].address == from.address)
				slot = index;
		}
		for (index = 0; slot == NONE && index < LOCKSTEP_MAXIMUM_SESSIONS; index++)
		{
			if (!lockstep.sessions[index].seen ||
				system_milliseconds() - lockstep.sessions[index].seen > LOCKSTEP_SESSION_TIMEOUT_MILLISECONDS)
			{
				slot = index;
			}
		}
		if (slot == NONE)
			continue;
		lockstep.sessions[slot].address = from.address;
		lockstep.sessions[slot].seen = system_milliseconds() | 1;
		memcpy(lockstep.sessions[slot].host_name, advertisement.host_name, sizeof(advertisement.host_name));
		lockstep.sessions[slot].host_name[11] = 0;
		memcpy(lockstep.sessions[slot].map_name, advertisement.map_name, sizeof(advertisement.map_name));
		lockstep.sessions[slot].map_name[63] = 0;
		lockstep.sessions[slot].difficulty = advertisement.difficulty;
		lockstep.sessions[slot].player_count = advertisement.open ? advertisement.player_count : LOCKSTEP_MAXIMUM_PLAYERS;
	}
}

/* ---------- public code */

boolean web_lockstep_active(void)
{
	return (lockstep.role == _role_host && lockstep.in_session) ||
		(lockstep.role == _role_guest && lockstep.guest_state == _guest_playing);
}

/* each main loop's frame (source/main/main.c) */
void web_lockstep_update(void)
{
	char const *join = getenv("HALO_LOCKSTEP_JOIN");

	udp_open();
	if (!lockstep.udp_open)
		return;
	discover();
	if (lockstep.role == _role_guest)
	{
		guest_update();
	}
	else
	{
		host_update();
		if (lockstep.role == _role_host)
		{
			short index;

			advertise();
			for (index = 1; index < LOCKSTEP_MAXIMUM_MACHINES; index++)
				machine_flush(&lockstep.machines[index]);
		}
	}
	/* ?HALO_LOCKSTEP_JOIN=1: join the first session seen (for tests) */
	if (join && *join == '1' && lockstep.role == _role_none && main_menu_is_active())
	{
		short index;

		for (index = 0; index < LOCKSTEP_MAXIMUM_SESSIONS; index++)
		{
			if (lockstep.sessions[index].seen && system_milliseconds() - lockstep.sessions[index].seen < LOCKSTEP_SESSION_TIMEOUT_MILLISECONDS)
			{
				web_lockstep_join(index);
				break;
			}
		}
	}
}

/* the sessions seen: the host's name, level and players (web_menus.c) */
boolean web_lockstep_session(short index, unsigned long *address, wchar_t *host_name, char *map_name, short *player_count)
{
	struct lockstep_session *session;

	if (index < 0 || index >= LOCKSTEP_MAXIMUM_SESSIONS)
		return FALSE;
	session = &lockstep.sessions[index];
	if (!session->seen || system_milliseconds() - session->seen > LOCKSTEP_SESSION_TIMEOUT_MILLISECONDS)
		return FALSE;
	if (address)
		*address = session->address;
	if (host_name)
		memcpy(host_name, session->host_name, sizeof(session->host_name));
	if (map_name)
		strcpy(map_name, session->map_name);
	if (player_count)
		*player_count = session->player_count;
	return TRUE;
}

/* join a session seen: connect, and say who */
boolean web_lockstep_join(short index)
{
	struct lockstep_address address;
	struct lockstep_buffer message = { 0 };
	wchar_t name[12];
	short color;

	if (lockstep.role != _role_none || !web_lockstep_session(index, NULL, NULL, NULL, NULL))
		return FALSE;
	memset(&lockstep.host, 0, sizeof(lockstep.host));
	lockstep.host.socket = posix_socket(SOCKET_AF_INET, SOCKET_STREAM, 0);
	if (lockstep.host.socket < 0)
		return FALSE;
	memset(&address, 0, sizeof(address));
	address.family = SOCKET_AF_INET;
	address.port = (unsigned short)((LOCKSTEP_TCP_PORT >> 8) | ((LOCKSTEP_TCP_PORT & 0xff) << 8));
	address.address = lockstep.sessions[index].address;
	if (posix_socket_connect(lockstep.host.socket, &address, sizeof(address)) < 0)
	{
		platform_log("lockstep: couldn't reach the host (%d)", posix_socket_last_error());
		posix_socket_close(lockstep.host.socket);
		lockstep.host.socket = 0;
		return FALSE;
	}
	posix_socket_set_nonblocking(lockstep.host.socket, 1);
	posix_socket_set_nodelay(lockstep.host.socket);
	lockstep.host.connected = TRUE;
	lockstep.role = _role_guest;
	lockstep.guest_state = _guest_waiting_for_welcome;
	lockstep.member_count = 0;
	my_name(name, &color);
	message_begin(&message, _message_hello);
	put_short(&message, LOCKSTEP_VERSION);
	buffer_put(&message, name, sizeof(name));
	put_short(&message, color);
	message_send(&lockstep.host, &message);
	buffer_free(&message);
	machine_flush(&lockstep.host);
	platform_log("lockstep: joining the session at %lu.%lu.%lu.%lu",
		address.address & 0xff, (address.address >> 8) & 0xff, (address.address >> 16) & 0xff, address.address >> 24);
	return TRUE;
}

/* the ticks this frame (source/game/game_time.c): one at most, as the
session's frames allow */
long web_lockstep_ticks(long ticks_elapsed)
{
	if (lockstep.role == _role_host && lockstep.in_session)
	{
		if (!host_all_ready())
			return 0;
		return MIN(ticks_elapsed, 1);
	}
	if (lockstep.role == _role_guest)
	{
		if (lockstep.guest_state != _guest_playing)
			return 0;
		return guest_frame() ? 1 : 0;
	}
	return ticks_elapsed;
}

/* a guest behind the host draws nothing until it has caught up (main.c) */
boolean web_lockstep_catching_up(void)
{
	return lockstep.role == _role_guest && lockstep.guest_state == _guest_playing && lockstep.frame_count > 3;
}

/* the tick's every player's action, by player (player_queues_new.c) */
boolean web_lockstep_tick_actions(struct player_action *actions, long count)
{
	struct lockstep_frame host_frame;
	struct lockstep_frame *frame;
	long index;

	for (index = 0; index < count; index++)
	{
		memset(&actions[index], 0, sizeof(actions[index]));
		actions[index].desired_weapon_index = NONE;
		actions[index].desired_grenade_index = NONE;
		actions[index].desired_zoom_level = NONE;
	}
	if (lockstep.role == _role_host)
	{
		host_build_frame(&host_frame);
		frame = &host_frame;
	}
	else
	{
		short local_player_index;
		struct lockstep_buffer message = { 0 };

		frame = guest_frame();
		if (!frame)
			return FALSE;
		/* this machine's players' actions, to the host for a tick to come */
		message_begin(&message, _message_input);
		put_long(&message, lockstep.epoch);
		put_long(&message, (long)player_control_web_take_action_flags());
		put_short(&message, LOCKSTEP_MAXIMUM_PLAYERS);
		for (local_player_index = 0; local_player_index < LOCKSTEP_MAXIMUM_PLAYERS; local_player_index++)
		{
			struct player_action action;

			update_client_web_local_action(local_player_index, &action);
			buffer_put(&message, &action, sizeof(action));
		}
		message_send(&lockstep.host, &message);
		buffer_free(&message);
	}

	/* its players added (every machine's in the same order) */
	for (index = 0; index < frame->command_count; index++)
	{
		if (lockstep.member_count < LOCKSTEP_MAXIMUM_PLAYERS)
		{
			struct lockstep_member *member = &lockstep.members[lockstep.member_count++];
			*member = frame->commands[index];
			member_make_player(member);
			player_spawn_count = lockstep.member_count;
			/* (one joining a level in progress comes to a teammate, as a co-op
			player does when dead; before any has a body, it starts where they
			do) */
			if (member->player_index != NONE)
			{
				short other;

				for (other = 0; other < lockstep.member_count - 1; other++)
				{
					struct player_datum *teammate = lockstep.members[other].player_index != NONE ?
						datum_try_and_get(player_data, lockstep.members[other].player_index) : NULL;

					if (teammate && teammate->unit_index != NONE)
					{
						((struct player_datum *)datum_get(player_data, member->player_index))->statistics.deaths = 1;
						break;
					}
				}
			}
			platform_log("lockstep: player %d added (machine %d) at tick %ld", lockstep.member_count, member->machine, frame->tick);
		}
	}
	for (index = 0; index < frame->action_count; index++)
	{
		if (frame->player_indices[index] >= 0 && frame->player_indices[index] < count)
			actions[frame->player_indices[index]] = frame->actions[index];
	}
	player_control_web_apply_action_flags(frame->flags);
	return TRUE;
}

/* after each tick (game_time.c): the state's hash, every two seconds */
void web_lockstep_after_tick(void)
{
	long tick = game_time_get();

	if (!web_lockstep_active() || tick % LOCKSTEP_HASH_PERIOD)
		return;
	if (lockstep.role == _role_host)
	{
		long slot = (tick / LOCKSTEP_HASH_PERIOD) % LOCKSTEP_HASH_HISTORY;

		lockstep.hashes[slot] = game_state_web_hash();
		lockstep.hash_ticks[slot] = tick;
	}
	else
	{
		struct lockstep_buffer message = { 0 };

		message_begin(&message, _message_hash);
		put_long(&message, lockstep.epoch);
		put_long(&message, tick);
		put_long(&message, (long)game_state_web_hash());
		message_send(&lockstep.host, &message);
		buffer_free(&message);
	}
}

/* a level's players (main.c create_local_players): the session's, in its
order, on every machine */
boolean web_lockstep_create_players(void)
{
	short index;

	if (!((lockstep.role == _role_host && lockstep.in_session) || lockstep.role == _role_guest))
		return FALSE;
	for (index = 0; index < lockstep.member_count; index++)
		member_make_player(&lockstep.members[index]);
	player_spawn_count = lockstep.member_count;
	return TRUE;
}

/* a level loaded (main.c main_new_map): a guest's join loads the host's
state next; a new level of the session waits for every machine */
void web_lockstep_map_loaded(void)
{
	if (lockstep.role == _role_guest)
	{
		if (lockstep.join_loading)
		{
			/* (the state is loaded at the next frame, guest_update) */
			lockstep.join_loading = FALSE;
		}
		else
		{
			struct lockstep_buffer message = { 0 };

			lockstep.epoch++;
			lockstep.frame_count = 0;
			message_begin(&message, _message_ready);
			put_long(&message, lockstep.epoch);
			message_send(&lockstep.host, &message);
			buffer_free(&message);
			machine_flush(&lockstep.host);
		}
	}
	else if (lockstep.role == _role_host && lockstep.in_session)
	{
		lockstep.epoch++;
		lockstep.barrier_started = system_milliseconds();
		memset(lockstep.hash_ticks, 0xff, sizeof(lockstep.hash_ticks));
	}
}
