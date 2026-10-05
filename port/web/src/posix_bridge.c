/*
POSIX_BRIDGE.C

The bridge between this page's network and the other players' pages.

The sockets' side (posix_net_web.c) and the page's side (port/web/shell/net.js)
share two rings of records in the game's memory: what the game sends
(datagrams, connections and stream data for other machines) and what the page
receives for it. The page carries the records to the other pages over WebRTC;
a thread here takes the incoming ones to the sockets.

Each record is a slot of RECORD_SIZE bytes:
	uint16 length (of the payload), uint8 type, uint8 reserved,
	uint32 address (the other machine's), uint32 connection,
	uint16 from_port, uint16 to_port, payload
*/

#include <emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdbool.h>

#include "web_net.h"

enum
{
	RECORD_SIZE = 1536,
	RECORD_HEADER_SIZE = 16,
	MAXIMUM_PAYLOAD = RECORD_SIZE - RECORD_HEADER_SIZE,
	RING_RECORDS = 512,
	MAXIMUM_CONNECTIONS = 64
};

enum record_type
{
	_record_datagram = 1,
	_record_connect = 2,
	_record_accept = 3,
	_record_refuse = 4,
	_record_stream = 5,
	_record_close = 6
};

struct ring
{
	uint32_t write;
	uint32_t read;
	uint8_t records[RING_RECORDS][RECORD_SIZE];
};

struct web_network
{
	/* this page's address, which the page announces */
	uint32_t address;
	struct ring outgoing;
	struct ring incoming;
};

struct connection
{
	uint32_t identifier;
	uint32_t address;
	int socket_index;
};

static struct web_network web_network;
static pthread_mutex_t bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static struct connection connections[MAXIMUM_CONNECTIONS];
static pthread_t bridge_thread;
static pthread_once_t address_once = PTHREAD_ONCE_INIT;

/* ---------- addresses: 10.x.y.z, which each page picks at random */

static void address_choose(void)
{
	uint32_t host = 1 + (uint32_t)(emscripten_random() * 0xFFFFFD);

	/* network order: 10 is the first byte */
	web_network.address = 10 | ((host & 0xFF) << 8) | (((host >> 8) & 0xFF) << 16) | ((host >> 16) << 24);
}

uint32_t web_net_address(void)
{
	pthread_once(&address_once, address_choose);
	return web_network.address;
}

uint32_t web_net_broadcast_address(void)
{
	return 10 | 0xFFFFFF00u;
}

/* ---------- the rings */

static bool ring_put(enum record_type type, uint32_t address, uint32_t connection, uint16_t from_port, uint16_t to_port,
	const void *payload, uint32_t size)
{
	struct ring *ring = &web_network.outgoing;
	uint32_t write;
	uint8_t *record;

	if (size > MAXIMUM_PAYLOAD)
		size = MAXIMUM_PAYLOAD;
	pthread_mutex_lock(&bridge_lock);
	write = __atomic_load_n(&ring->write, __ATOMIC_ACQUIRE);
	if (write - __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE) >= RING_RECORDS)
	{
		/* the page is not taking them: a datagram is dropped, as a network
		would; a stream's sender is told to try again (web_net_send_stream) */
		pthread_mutex_unlock(&bridge_lock);
		return false;
	}
	record = ring->records[write % RING_RECORDS];
	record[0] = (uint8_t)size;
	record[1] = (uint8_t)(size >> 8);
	record[2] = (uint8_t)type;
	record[3] = 0;
	memcpy(record + 4, &address, 4);
	memcpy(record + 8, &connection, 4);
	memcpy(record + 12, &from_port, 2);
	memcpy(record + 14, &to_port, 2);
	if (size)
		memcpy(record + RECORD_HEADER_SIZE, payload, size);
	__atomic_store_n(&ring->write, write + 1, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&bridge_lock);
	/* the page waits on the count */
	emscripten_futex_wake(&ring->write, 1);
	return true;
}

/* ---------- connections */

static struct connection *connection_find(uint32_t identifier)
{
	int index;

	for (index = 0; index < MAXIMUM_CONNECTIONS; index++)
	{
		if (identifier && connections[index].identifier == identifier)
			return &connections[index];
	}
	return NULL;
}

static struct connection *connection_add(uint32_t identifier, uint32_t address, int socket_index)
{
	int index;

	for (index = 0; index < MAXIMUM_CONNECTIONS; index++)
	{
		if (!connections[index].identifier)
		{
			connections[index].identifier = identifier;
			connections[index].address = address;
			connections[index].socket_index = socket_index;
			return &connections[index];
		}
	}
	return NULL;
}

/* ---------- the sockets to the bridge */

void web_net_send_datagram(uint32_t to_address, uint16_t to_port, uint16_t from_port, const void *data, uint32_t size)
{
	ring_put(_record_datagram, to_address, 0, from_port, to_port, data, size);
}

uint32_t web_net_connect(int socket_index, uint32_t to_address, uint16_t to_port, uint16_t from_port)
{
	uint32_t identifier;

	pthread_mutex_lock(&bridge_lock);
	do
		identifier = 1 + (uint32_t)(emscripten_random() * 4294967040.0);
	while (connection_find(identifier));
	if (!connection_add(identifier, to_address, socket_index))
	{
		pthread_mutex_unlock(&bridge_lock);
		web_net_connection_result(socket_index, false);
		return 0;
	}
	pthread_mutex_unlock(&bridge_lock);
	ring_put(_record_connect, to_address, identifier, from_port, to_port, NULL, 0);
	return identifier;
}

/* the bytes of a connection's stream the bridge took (all but what would
not fit: the rest is the sender's to send again, as a full TCP window's) */
uint32_t web_net_send_stream(uint32_t identifier, const void *data, uint32_t size)
{
	struct connection *connection;
	uint32_t address;
	uint32_t offset;

	pthread_mutex_lock(&bridge_lock);
	connection = connection_find(identifier);
	address = connection ? connection->address : 0;
	pthread_mutex_unlock(&bridge_lock);
	if (!address)
		return size;
	for (offset = 0; offset < size; offset += MAXIMUM_PAYLOAD)
	{
		uint32_t count = size - offset < MAXIMUM_PAYLOAD ? size - offset : MAXIMUM_PAYLOAD;

		if (!ring_put(_record_stream, address, identifier, 0, 0, (const uint8_t *)data + offset, count))
			return offset;
	}
	return size;
}

void web_net_close_connection(uint32_t identifier)
{
	struct connection *connection;
	uint32_t address = 0;

	pthread_mutex_lock(&bridge_lock);
	connection = connection_find(identifier);
	if (connection)
	{
		address = connection->address;
		connection->identifier = 0;
	}
	pthread_mutex_unlock(&bridge_lock);
	if (address)
		ring_put(_record_close, address, identifier, 0, 0, NULL, 0);
}

/* ---------- the page to the sockets */

static void dispatch(const uint8_t *record)
{
	uint32_t size = record[0] | (record[1] << 8);
	enum record_type type = record[2];
	uint32_t address, identifier;
	uint16_t from_port, to_port;
	const uint8_t *payload = record + RECORD_HEADER_SIZE;
	struct connection *connection;
	int socket_index;

	memcpy(&address, record + 4, 4);
	memcpy(&identifier, record + 8, 4);
	memcpy(&from_port, record + 12, 2);
	memcpy(&to_port, record + 14, 2);
	if (size > MAXIMUM_PAYLOAD)
		return;
	switch (type)
	{
	case _record_datagram:
		web_net_receive_datagram(address, from_port, to_port, payload, size);
		break;
	case _record_connect:
		socket_index = web_net_receive_connection(identifier, address, from_port, to_port);
		if (socket_index >= 0)
		{
			pthread_mutex_lock(&bridge_lock);
			connection_add(identifier, address, socket_index);
			pthread_mutex_unlock(&bridge_lock);
			ring_put(_record_accept, address, identifier, to_port, from_port, NULL, 0);
		}
		else
		{
			ring_put(_record_refuse, address, identifier, to_port, from_port, NULL, 0);
		}
		break;
	case _record_accept:
	case _record_refuse:
	case _record_stream:
	case _record_close:
		pthread_mutex_lock(&bridge_lock);
		connection = connection_find(identifier);
		socket_index = connection ? connection->socket_index : -1;
		if (connection && (type == _record_refuse || type == _record_close))
			connection->identifier = 0;
		pthread_mutex_unlock(&bridge_lock);
		if (socket_index < 0)
			break;
		if (type == _record_stream)
			web_net_receive_stream(socket_index, payload, size);
		else
			web_net_connection_result(socket_index, type == _record_accept);
		break;
	}
}

static void *bridge_main(void *context)
{
	struct ring *ring = &web_network.incoming;

	(void)context;
	for (;;)
	{
		uint32_t write = __atomic_load_n(&ring->write, __ATOMIC_ACQUIRE);
		uint32_t read = __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE);

		if (read == write)
		{
			/* the page wakes this when it writes */
			emscripten_futex_wait(&ring->write, write, 100.0);
			continue;
		}
		dispatch(ring->records[read % RING_RECORDS]);
		__atomic_store_n(&ring->read, read + 1, __ATOMIC_RELEASE);
	}
	return NULL;
}

/* the page's view of the block: its address and layout */
EMSCRIPTEN_KEEPALIVE uint32_t web_net_layout(uint32_t *layout)
{
	layout[0] = offsetof(struct web_network, address);
	layout[1] = offsetof(struct web_network, outgoing);
	layout[2] = offsetof(struct web_network, incoming);
	layout[3] = offsetof(struct ring, write);
	layout[4] = offsetof(struct ring, read);
	layout[5] = offsetof(struct ring, records);
	layout[6] = RING_RECORDS;
	layout[7] = RECORD_SIZE;
	layout[8] = RECORD_HEADER_SIZE;
	return (uint32_t)(uintptr_t)&web_network;
}

static void bridge_start(void)
{
	web_net_address();
	if (pthread_create(&bridge_thread, NULL, bridge_main, NULL) != 0)
	{
		fprintf(stderr, "the network bridge's thread did not start\n");
		return;
	}
	/* the page attaches to the rings once they exist */
	MAIN_THREAD_ASYNC_EM_ASM({
		if (Module['haloNetAttach']) Module['haloNetAttach']();
	});
}

void web_net_initialize(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, bridge_start);
}
