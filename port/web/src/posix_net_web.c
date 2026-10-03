/*
POSIX_NET_WEB.C

The socket helpers of port/linux/src/posix.h (posix_net.c on Linux) on a
network inside the browser, which has no sockets of its own.

Each page is a machine with its own address (web_net_address). Its sockets
reach each other directly; what they send to another address (or broadcast)
goes to the bridge, which carries it to the other players' pages
(posix_bridge.c), and what the bridge receives arrives here. Datagram sockets
(UDP) keep queues of datagrams, stream sockets (TCP) a byte stream in each
direction.

Calls return -1 with a Winsock error code from posix_socket_last_error, as
posix_net.c's do. A blocking accept, connect or receive waits.
*/

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "posix.h"
#include "web_net.h"

/* Winsock error codes (winsockx.h) */
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAENOTSOCK 10038
#define WSAEMSGSIZE 10040
#define WSAENOPROTOOPT 10042
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEACCES 10013
#define WSAECONNRESET 10054
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAECONNREFUSED 10061

/* Winsock SOL_SOCKET option values (winsockx.h) */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SO_BROADCAST 0x0020
#define WINSOCK_SO_SNDBUF 0x1001
#define WINSOCK_SO_RCVBUF 0x1002
#define WINSOCK_SO_ERROR 0x1007
#define WINSOCK_SO_TYPE 0x1008

/* Winsock's and the C library's agree on these */
#define WINSOCK_SOCK_STREAM 1
#define WINSOCK_SOCK_DGRAM 2
#define WINSOCK_MSG_PEEK 2

enum
{
	MAXIMUM_SOCKETS = 64,
	MAXIMUM_QUEUED_DATAGRAMS = 128,
	MAXIMUM_DATAGRAM_SIZE = 1500,
	STREAM_BUFFER_SIZE = 256 * 1024,
	MAXIMUM_PENDING_CONNECTIONS = 8,
	FIRST_EPHEMERAL_PORT = 49152,
	/* the first socket's descriptor */
	SOCKET_BASE = 0x100
};

struct datagram
{
	uint32_t from_address;
	uint16_t from_port;
	uint16_t size;
	uint8_t data[MAXIMUM_DATAGRAM_SIZE];
};

struct stream_buffer
{
	uint8_t data[STREAM_BUFFER_SIZE];
	uint32_t read;
	uint32_t write;
};

enum socket_state
{
	_socket_free,
	_socket_open,
	_socket_listening,
	_socket_connecting,
	_socket_connected,
	_socket_closed_by_peer
};

struct web_socket
{
	enum socket_state state;
	int type;
	bool nonblocking;
	bool broadcast;
	uint16_t port;

	/* datagram sockets */
	struct datagram *datagrams;
	uint32_t datagram_read, datagram_write;

	/* stream sockets: the peer, and what arrived from it */
	uint32_t peer_address;
	uint16_t peer_port;
	int peer;                         /* a socket of this page's, or -1 */
	uint32_t connection;              /* the bridge's connection, for another machine */
	struct stream_buffer *incoming;
	int pending[MAXIMUM_PENDING_CONNECTIONS];
	int pending_count;
	/* a connect that failed (SO_ERROR, and the select's error set) */
	int error;
};

static pthread_mutex_t network_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t network_changed = PTHREAD_COND_INITIALIZER;
static struct web_socket sockets[MAXIMUM_SOCKETS];
static uint16_t next_ephemeral_port = FIRST_EPHEMERAL_PORT;
static __thread int last_error;

/* ---------- private code */

static int fail(int error)
{
	last_error = error;
	return -1;
}

static int succeed(int result)
{
	last_error = 0;
	return result;
}

/* the socket of a descriptor; under network_lock */
static struct web_socket *socket_get(int descriptor)
{
	if (descriptor < SOCKET_BASE || descriptor >= SOCKET_BASE + MAXIMUM_SOCKETS)
		return NULL;
	if (sockets[descriptor - SOCKET_BASE].state == _socket_free)
		return NULL;
	return &sockets[descriptor - SOCKET_BASE];
}

static int socket_index(const struct web_socket *socket)
{
	return (int)(socket - sockets);
}

static bool is_local_address(uint32_t address)
{
	return address == INADDR_ANY || address == htonl(INADDR_LOOPBACK) || address == web_net_address();
}

static bool is_broadcast_address(uint32_t address)
{
	return address == INADDR_BROADCAST || address == web_net_broadcast_address();
}

static struct web_socket *socket_bound_to(int type, uint16_t port)
{
	int index;

	for (index = 0; index < MAXIMUM_SOCKETS; index++)
	{
		struct web_socket *socket = &sockets[index];

		if (socket->state != _socket_free && socket->type == type && socket->port == port &&
			(type == WINSOCK_SOCK_DGRAM || socket->state == _socket_listening))
		{
			return socket;
		}
	}
	return NULL;
}

static uint16_t ephemeral_port(int type)
{
	int attempt;

	for (attempt = 0; attempt < 16384; attempt++)
	{
		uint16_t port = next_ephemeral_port++;

		if (next_ephemeral_port < FIRST_EPHEMERAL_PORT)
			next_ephemeral_port = FIRST_EPHEMERAL_PORT;
		if (!socket_bound_to(type, port))
			return port;
	}
	return 0;
}

/* queues a datagram on the socket bound to the port; under network_lock */
static void datagram_deliver(uint16_t port, uint32_t from_address, uint16_t from_port, const void *data, uint32_t size)
{
	struct web_socket *socket = socket_bound_to(WINSOCK_SOCK_DGRAM, port);
	struct datagram *datagram;

	if (!socket || size > MAXIMUM_DATAGRAM_SIZE ||
		socket->datagram_write - socket->datagram_read >= MAXIMUM_QUEUED_DATAGRAMS)
	{
		return;
	}
	datagram = &socket->datagrams[socket->datagram_write % MAXIMUM_QUEUED_DATAGRAMS];
	datagram->from_address = from_address;
	datagram->from_port = from_port;
	datagram->size = (uint16_t)size;
	memcpy(datagram->data, data, size);
	socket->datagram_write++;
	pthread_cond_broadcast(&network_changed);
}

static uint32_t stream_available(const struct stream_buffer *buffer)
{
	return buffer->write - buffer->read;
}

static uint32_t stream_put(struct stream_buffer *buffer, const void *data, uint32_t size)
{
	uint32_t space = STREAM_BUFFER_SIZE - stream_available(buffer);
	uint32_t count = size < space ? size : space;
	uint32_t index;

	for (index = 0; index < count; index++)
		buffer->data[(buffer->write + index) % STREAM_BUFFER_SIZE] = ((const uint8_t *)data)[index];
	buffer->write += count;
	return count;
}

static uint32_t stream_take(struct stream_buffer *buffer, void *data, uint32_t size, bool peek)
{
	uint32_t available = stream_available(buffer);
	uint32_t count = size < available ? size : available;
	uint32_t index;

	for (index = 0; index < count; index++)
		((uint8_t *)data)[index] = buffer->data[(buffer->read + index) % STREAM_BUFFER_SIZE];
	if (!peek)
		buffer->read += count;
	return count;
}

/* a free socket of the type, or -1; under network_lock */
static int socket_allocate(int type)
{
	int index;

	for (index = 0; index < MAXIMUM_SOCKETS; index++)
	{
		struct web_socket *socket = &sockets[index];

		if (socket->state == _socket_free)
		{
			memset(socket, 0, sizeof(*socket));
			socket->type = type;
			socket->peer = -1;
			if (type == WINSOCK_SOCK_DGRAM)
				socket->datagrams = calloc(MAXIMUM_QUEUED_DATAGRAMS, sizeof(struct datagram));
			else
				socket->incoming = calloc(1, sizeof(struct stream_buffer));
			if (!socket->datagrams && !socket->incoming)
				return -1;
			socket->state = _socket_open;
			return index;
		}
	}
	return -1;
}

static void socket_release(struct web_socket *socket)
{
	free(socket->datagrams);
	free(socket->incoming);
	memset(socket, 0, sizeof(*socket));
	socket->peer = -1;
}

static void deadline_after(struct timespec *deadline, long long microseconds)
{
	clock_gettime(CLOCK_REALTIME, deadline);
	deadline->tv_sec += (time_t)(microseconds / 1000000);
	deadline->tv_nsec += (long)(microseconds % 1000000) * 1000L;
	if (deadline->tv_nsec >= 1000000000L)
	{
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
}

/* waits for the network to change, under network_lock; false at the deadline */
static bool wait_for_change(const struct timespec *deadline)
{
	if (!deadline)
	{
		pthread_cond_wait(&network_changed, &network_lock);
		return true;
	}
	return pthread_cond_timedwait(&network_changed, &network_lock, deadline) != ETIMEDOUT;
}

static void address_out(void *address, int *address_length, uint32_t host, uint16_t port)
{
	struct sockaddr_in in;

	if (!address || !address_length)
		return;
	memset(&in, 0, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = host;
	in.sin_port = htons(port);
	memcpy(address, &in, *address_length < (int)sizeof(in) ? (size_t)*address_length : sizeof(in));
	*address_length = (int)sizeof(in);
}

/* ---------- the bridge's side */

void web_net_receive_datagram(uint32_t from_address, uint16_t from_port, uint16_t to_port, const void *data, uint32_t size)
{
	pthread_mutex_lock(&network_lock);
	datagram_deliver(to_port, from_address, from_port, data, size);
	pthread_mutex_unlock(&network_lock);
}

/* another machine connects to a listening port: returns the connection's
socket, or -1 to refuse */
int web_net_receive_connection(uint32_t connection, uint32_t from_address, uint16_t from_port, uint16_t to_port)
{
	struct web_socket *listener;
	int result = -1;

	pthread_mutex_lock(&network_lock);
	listener = socket_bound_to(WINSOCK_SOCK_STREAM, to_port);
	if (listener && listener->pending_count < MAXIMUM_PENDING_CONNECTIONS)
	{
		int index = socket_allocate(WINSOCK_SOCK_STREAM);

		if (index >= 0)
		{
			struct web_socket *socket = &sockets[index];

			socket->state = _socket_connected;
			socket->port = to_port;
			socket->peer_address = from_address;
			socket->peer_port = from_port;
			socket->connection = connection;
			listener->pending[listener->pending_count++] = index;
			pthread_cond_broadcast(&network_changed);
			result = index;
		}
	}
	pthread_mutex_unlock(&network_lock);
	return result;
}

void web_net_receive_stream(int index, const void *data, uint32_t size)
{
	pthread_mutex_lock(&network_lock);
	if (index >= 0 && index < MAXIMUM_SOCKETS && sockets[index].state == _socket_connected)
	{
		stream_put(sockets[index].incoming, data, size);
		pthread_cond_broadcast(&network_changed);
	}
	pthread_mutex_unlock(&network_lock);
}

void web_net_connection_result(int index, bool connected)
{
	pthread_mutex_lock(&network_lock);
	if (index >= 0 && index < MAXIMUM_SOCKETS)
	{
		struct web_socket *socket = &sockets[index];

		if (socket->state == _socket_connecting)
		{
			socket->state = connected ? _socket_connected : _socket_open;
			socket->error = connected ? 0 : WSAECONNREFUSED;
		}
		else if (!connected && socket->state == _socket_connected)
		{
			socket->state = _socket_closed_by_peer;
		}
		pthread_cond_broadcast(&network_changed);
	}
	pthread_mutex_unlock(&network_lock);
}

/* ---------- sockets (posix.h) */

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	int index;

	(void)protocol;
	if (family != AF_INET || (type != WINSOCK_SOCK_DGRAM && type != WINSOCK_SOCK_STREAM))
		return fail(WSAEAFNOSUPPORT);
	web_net_initialize();
	pthread_mutex_lock(&network_lock);
	index = socket_allocate(type);
	pthread_mutex_unlock(&network_lock);
	if (index < 0)
		return fail(WSAEMFILE);
	return succeed(SOCKET_BASE + index);
}

int posix_socket_close(int descriptor)
{
	struct web_socket *socket;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	if (socket->type == WINSOCK_SOCK_STREAM && socket->state == _socket_connected)
	{
		if (socket->peer >= 0 && sockets[socket->peer].peer == socket_index(socket))
		{
			sockets[socket->peer].state = _socket_closed_by_peer;
			sockets[socket->peer].peer = -1;
		}
		else if (socket->peer < 0 && socket->connection)
		{
			web_net_close_connection(socket->connection);
		}
	}
	/* connections waiting on a listener that closes are refused */
	if (socket->state == _socket_listening)
	{
		int pending;

		for (pending = 0; pending < socket->pending_count; pending++)
		{
			struct web_socket *waiting = &sockets[socket->pending[pending]];

			if (waiting->peer >= 0)
			{
				sockets[waiting->peer].state = _socket_closed_by_peer;
				sockets[waiting->peer].peer = -1;
			}
			else if (waiting->connection)
			{
				web_net_close_connection(waiting->connection);
			}
			socket_release(waiting);
		}
	}
	socket_release(socket);
	pthread_cond_broadcast(&network_changed);
	pthread_mutex_unlock(&network_lock);
	return succeed(0);
}

int posix_socket_bind(int descriptor, const void *address, int address_length)
{
	struct sockaddr_in in;
	struct web_socket *socket;
	struct web_socket *bound;
	uint16_t port;

	if (!address || address_length < (int)sizeof(in))
		return fail(WSAEFAULT);
	memcpy(&in, address, sizeof(in));
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	port = ntohs(in.sin_port);
	bound = port ? socket_bound_to(socket->type, port) : NULL;
	if (bound && bound != socket)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAEADDRINUSE);
	}
	socket->port = port ? port : ephemeral_port(socket->type);
	pthread_mutex_unlock(&network_lock);
	return succeed(0);
}

int posix_socket_listen(int descriptor, int backlog)
{
	struct web_socket *socket;

	(void)backlog;
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket || socket->type != WINSOCK_SOCK_STREAM)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	if (!socket->port)
		socket->port = ephemeral_port(WINSOCK_SOCK_STREAM);
	socket->state = _socket_listening;
	pthread_mutex_unlock(&network_lock);
	return succeed(0);
}

int posix_socket_accept(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket;
	int index;

	pthread_mutex_lock(&network_lock);
	for (;;)
	{
		socket = socket_get(descriptor);
		if (!socket || socket->state != _socket_listening)
		{
			pthread_mutex_unlock(&network_lock);
			return fail(WSAEINVAL);
		}
		if (socket->pending_count > 0)
			break;
		if (socket->nonblocking)
		{
			pthread_mutex_unlock(&network_lock);
			return fail(WSAEWOULDBLOCK);
		}
		wait_for_change(NULL);
	}
	index = socket->pending[0];
	memmove(socket->pending, socket->pending + 1, (size_t)(socket->pending_count - 1) * sizeof(int));
	socket->pending_count--;
	address_out(address, address_length, sockets[index].peer_address, sockets[index].peer_port);
	pthread_mutex_unlock(&network_lock);
	return succeed(SOCKET_BASE + index);
}

int posix_socket_connect(int descriptor, const void *address, int address_length)
{
	struct sockaddr_in in;
	struct web_socket *socket;
	uint32_t to;
	uint16_t port;

	if (!address || address_length < (int)sizeof(in))
		return fail(WSAEFAULT);
	memcpy(&in, address, sizeof(in));
	to = in.sin_addr.s_addr;
	port = ntohs(in.sin_port);
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	if (socket->type == WINSOCK_SOCK_DGRAM)
	{
		socket->peer_address = to;
		socket->peer_port = port;
		pthread_mutex_unlock(&network_lock);
		return succeed(0);
	}
	if (socket->state == _socket_connected)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAEISCONN);
	}
	if (!socket->port)
		socket->port = ephemeral_port(WINSOCK_SOCK_STREAM);
	socket->peer_address = to;
	socket->peer_port = port;
	socket->error = 0;
	if (is_local_address(to))
	{
		/* a socket of this page's: pair them */
		struct web_socket *listener = socket_bound_to(WINSOCK_SOCK_STREAM, port);
		int index;

		if (!listener || listener->pending_count >= MAXIMUM_PENDING_CONNECTIONS ||
			(index = socket_allocate(WINSOCK_SOCK_STREAM)) < 0)
		{
			socket->error = WSAECONNREFUSED;
			pthread_mutex_unlock(&network_lock);
			return fail(WSAECONNREFUSED);
		}
		sockets[index].state = _socket_connected;
		sockets[index].port = port;
		/* a connection to 127.0.0.1 comes from 127.0.0.1 */
		sockets[index].peer_address = to == htonl(INADDR_LOOPBACK) ? to : web_net_address();
		sockets[index].peer_port = socket->port;
		sockets[index].peer = socket_index(socket);
		socket->state = _socket_connected;
		socket->peer = index;
		listener->pending[listener->pending_count++] = index;
		pthread_cond_broadcast(&network_changed);
		pthread_mutex_unlock(&network_lock);
		return succeed(0);
	}
	/* another machine: the bridge connects */
	socket->state = _socket_connecting;
	pthread_mutex_unlock(&network_lock);
	socket->connection = web_net_connect(socket_index(socket), to, port, socket->port);
	pthread_mutex_lock(&network_lock);
	if (socket->nonblocking)
	{
		pthread_mutex_unlock(&network_lock);
		/* (as Winsock: the game then waits for it to be writeable) */
		return fail(WSAEWOULDBLOCK);
	}
	while (socket->state == _socket_connecting)
		wait_for_change(NULL);
	if (socket->state != _socket_connected)
	{
		int error = socket->error ? socket->error : WSAECONNREFUSED;

		pthread_mutex_unlock(&network_lock);
		return fail(error);
	}
	pthread_mutex_unlock(&network_lock);
	return succeed(0);
}

int posix_socket_sendto(int descriptor, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct sockaddr_in in;
	struct web_socket *socket;
	uint32_t to;
	uint16_t port, from_port;

	(void)flags;
	if (!address)
		return posix_socket_send(descriptor, buffer, length, flags);
	if (address_length < (int)sizeof(in))
		return fail(WSAEFAULT);
	if (length < 0 || length > MAXIMUM_DATAGRAM_SIZE)
		return fail(WSAEMSGSIZE);
	memcpy(&in, address, sizeof(in));
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket || socket->type != WINSOCK_SOCK_DGRAM)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	if (!socket->port)
		socket->port = ephemeral_port(WINSOCK_SOCK_DGRAM);
	from_port = socket->port;
	to = in.sin_addr.s_addr;
	port = ntohs(in.sin_port);
	if (is_broadcast_address(to) && !socket->broadcast)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAEACCES);
	}
	/* this machine hears its own broadcasts, as on a network; a datagram to
	127.0.0.1 comes from 127.0.0.1 (the game's server finds its own client
	machine by the address of its connection, which is the loopback one) */
	if (is_local_address(to) || is_broadcast_address(to))
	{
		uint32_t from_address = to == htonl(INADDR_LOOPBACK) ? to : web_net_address();

		datagram_deliver(port, from_address, from_port, buffer, (uint32_t)length);
	}
	pthread_mutex_unlock(&network_lock);
	if (!is_local_address(to))
		web_net_send_datagram(to, port, from_port, buffer, (uint32_t)length);
	return succeed(length);
}

int posix_socket_send(int descriptor, const void *buffer, int length, int flags)
{
	struct web_socket *socket;
	int result;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	if (socket->type == WINSOCK_SOCK_DGRAM)
	{
		struct sockaddr_in to;

		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = socket->peer_address;
		to.sin_port = htons(socket->peer_port);
		pthread_mutex_unlock(&network_lock);
		if (!to.sin_port)
			return fail(WSAENOTCONN);
		return posix_socket_sendto(descriptor, buffer, length, flags, &to, sizeof(to));
	}
	if (socket->state == _socket_closed_by_peer)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAECONNRESET);
	}
	if (socket->state != _socket_connected)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(socket->state == _socket_connecting ? WSAEWOULDBLOCK : WSAENOTCONN);
	}
	if (socket->peer >= 0)
	{
		result = (int)stream_put(sockets[socket->peer].incoming, buffer, (uint32_t)length);
		pthread_cond_broadcast(&network_changed);
	}
	else
	{
		uint32_t connection = socket->connection;

		pthread_mutex_unlock(&network_lock);
		web_net_send_stream(connection, buffer, (uint32_t)length);
		return succeed(length);
	}
	pthread_mutex_unlock(&network_lock);
	if (result == 0 && length > 0)
		return fail(WSAEWOULDBLOCK);
	return succeed(result);
}

int posix_socket_recvfrom(int descriptor, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct web_socket *socket;
	struct datagram *datagram;
	int count;
	int size;

	pthread_mutex_lock(&network_lock);
	for (;;)
	{
		socket = socket_get(descriptor);
		if (!socket)
		{
			pthread_mutex_unlock(&network_lock);
			return fail(WSAENOTSOCK);
		}
		if (socket->type == WINSOCK_SOCK_STREAM)
		{
			pthread_mutex_unlock(&network_lock);
			return posix_socket_recv(descriptor, buffer, length, flags);
		}
		if (socket->datagram_read != socket->datagram_write)
			break;
		if (socket->nonblocking || !socket->port)
		{
			pthread_mutex_unlock(&network_lock);
			return fail(WSAEWOULDBLOCK);
		}
		wait_for_change(NULL);
	}
	datagram = &socket->datagrams[socket->datagram_read % MAXIMUM_QUEUED_DATAGRAMS];
	size = datagram->size;
	count = size < length ? size : length;
	memcpy(buffer, datagram->data, (size_t)count);
	address_out(address, address_length, datagram->from_address, datagram->from_port);
	if (!(flags & WINSOCK_MSG_PEEK))
		socket->datagram_read++;
	pthread_mutex_unlock(&network_lock);
	/* a datagram larger than the buffer: Winsock gives its start with
	WSAEMSGSIZE */
	if (size > length)
		return fail(WSAEMSGSIZE);
	return succeed(count);
}

int posix_socket_recv(int descriptor, void *buffer, int length, int flags)
{
	struct web_socket *socket;
	uint32_t count;

	pthread_mutex_lock(&network_lock);
	for (;;)
	{
		socket = socket_get(descriptor);
		if (!socket)
		{
			pthread_mutex_unlock(&network_lock);
			return fail(WSAENOTSOCK);
		}
		if (socket->type == WINSOCK_SOCK_DGRAM)
		{
			pthread_mutex_unlock(&network_lock);
			return posix_socket_recvfrom(descriptor, buffer, length, flags, NULL, NULL);
		}
		if (stream_available(socket->incoming) > 0)
			break;
		if (socket->state == _socket_closed_by_peer)
		{
			pthread_mutex_unlock(&network_lock);
			return succeed(0);
		}
		if (socket->state != _socket_connected)
		{
			pthread_mutex_unlock(&network_lock);
			return fail(socket->state == _socket_connecting ? WSAEWOULDBLOCK : WSAENOTCONN);
		}
		if (socket->nonblocking)
		{
			pthread_mutex_unlock(&network_lock);
			return fail(WSAEWOULDBLOCK);
		}
		wait_for_change(NULL);
	}
	count = stream_take(socket->incoming, buffer, (uint32_t)length, (flags & WINSOCK_MSG_PEEK) != 0);
	pthread_mutex_unlock(&network_lock);
	return succeed((int)count);
}

int posix_socket_shutdown(int descriptor, int how)
{
	int exists;

	(void)how;
	pthread_mutex_lock(&network_lock);
	exists = socket_get(descriptor) != NULL;
	pthread_mutex_unlock(&network_lock);
	return exists ? succeed(0) : fail(WSAENOTSOCK);
}

int posix_socket_set_nonblocking(int descriptor, int nonblocking)
{
	struct web_socket *socket;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
		socket->nonblocking = nonblocking != 0;
	pthread_mutex_unlock(&network_lock);
	return socket ? succeed(0) : fail(WSAENOTSOCK);
}

int posix_socket_set_nodelay(int descriptor)
{
	int exists;

	pthread_mutex_lock(&network_lock);
	exists = socket_get(descriptor) != NULL;
	pthread_mutex_unlock(&network_lock);
	return exists ? succeed(0) : fail(WSAENOTSOCK);
}

int posix_socket_bytes_available(int descriptor, posix_ulong *count)
{
	struct web_socket *socket;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	if (socket->type == WINSOCK_SOCK_DGRAM)
	{
		*count = socket->datagram_read != socket->datagram_write ?
			socket->datagrams[socket->datagram_read % MAXIMUM_QUEUED_DATAGRAMS].size : 0;
	}
	else
	{
		*count = stream_available(socket->incoming);
	}
	pthread_mutex_unlock(&network_lock);
	return succeed(0);
}

int posix_socket_setsockopt(int descriptor, int level, int name, const void *value, int length)
{
	struct web_socket *socket;

	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	if (level == WINSOCK_SOL_SOCKET && name == WINSOCK_SO_BROADCAST && value && length >= 1)
	{
		int enabled = 0;

		memcpy(&enabled, value, length < (int)sizeof(enabled) ? (size_t)length : sizeof(enabled));
		socket->broadcast = enabled != 0;
	}
	/* the rest (buffer sizes, Xbox-only options) have nothing to do here */
	pthread_mutex_unlock(&network_lock);
	return succeed(0);
}

int posix_socket_getsockopt(int descriptor, int level, int name, void *value, int *length)
{
	struct web_socket *socket;
	int result = 0;

	if (!value || !length || *length < 4)
		return fail(WSAEFAULT);
	if (level != WINSOCK_SOL_SOCKET)
		return fail(WSAENOPROTOOPT);
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (!socket)
	{
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOTSOCK);
	}
	switch (name)
	{
	case WINSOCK_SO_ERROR: result = socket->error; socket->error = 0; break;
	case WINSOCK_SO_TYPE: result = socket->type; break;
	case WINSOCK_SO_BROADCAST: result = socket->broadcast; break;
	case WINSOCK_SO_RCVBUF:
	case WINSOCK_SO_SNDBUF: result = STREAM_BUFFER_SIZE; break;
	default:
		pthread_mutex_unlock(&network_lock);
		return fail(WSAENOPROTOOPT);
	}
	pthread_mutex_unlock(&network_lock);
	memcpy(value, &result, 4);
	*length = 4;
	return succeed(0);
}

int posix_socket_getsockname(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket;
	uint16_t port;

	if (!address || !address_length || *address_length < (int)sizeof(struct sockaddr_in))
		return fail(WSAEFAULT);
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	port = socket ? socket->port : 0;
	pthread_mutex_unlock(&network_lock);
	if (!socket)
		return fail(WSAENOTSOCK);
	address_out(address, address_length, web_net_address(), port);
	return succeed(0);
}

int posix_socket_getpeername(int descriptor, void *address, int *address_length)
{
	struct web_socket *socket;
	uint32_t host = 0;
	uint16_t port = 0;
	bool connected = false;

	if (!address || !address_length || *address_length < (int)sizeof(struct sockaddr_in))
		return fail(WSAEFAULT);
	pthread_mutex_lock(&network_lock);
	socket = socket_get(descriptor);
	if (socket)
	{
		host = socket->peer_address;
		port = socket->peer_port;
		connected = socket->type == WINSOCK_SOCK_DGRAM ? port != 0 :
			socket->state == _socket_connected || socket->state == _socket_closed_by_peer;
	}
	pthread_mutex_unlock(&network_lock);
	if (!socket)
		return fail(WSAENOTSOCK);
	if (!connected)
		return fail(WSAENOTCONN);
	address_out(address, address_length, host, port);
	return succeed(0);
}

static bool socket_readable(const struct web_socket *socket)
{
	if (socket->type == WINSOCK_SOCK_DGRAM)
		return socket->datagram_read != socket->datagram_write;
	if (socket->state == _socket_listening)
		return socket->pending_count > 0;
	return stream_available(socket->incoming) > 0 || socket->state == _socket_closed_by_peer;
}

static bool socket_writable(const struct web_socket *socket)
{
	return socket->type == WINSOCK_SOCK_DGRAM || socket->state == _socket_connected;
}

/* a connect that failed: in the error set, as Winsock's */
static bool socket_failed(const struct web_socket *socket)
{
	return socket->type == WINSOCK_SOCK_STREAM && socket->state == _socket_open && socket->error;
}

/* the ready count, and with update the lists rewritten to hold only the
ready ones; under network_lock */
static int select_ready(int *lists[3], int *counts[3], bool update)
{
	int total = 0;
	int list;

	for (list = 0; list < 3; list++)
	{
		int kept = 0;
		int index;

		if (!lists[list])
			continue;
		for (index = 0; index < *counts[list]; index++)
		{
			int descriptor = lists[list][index];
			struct web_socket *socket = socket_get(descriptor);
			bool ready = socket && (list == 0 ? socket_readable(socket) : list == 1 ? socket_writable(socket) :
				socket_failed(socket));

			if (!ready)
				continue;
			if (update)
				lists[list][kept] = descriptor;
			kept++;
		}
		if (update)
			*counts[list] = kept;
		total += kept;
	}
	return total;
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	int *lists[3] = { read, write, error };
	int *counts[3] = { read_count, write_count, error_count };
	long long microseconds = (long long)timeout_seconds * 1000000 + timeout_microseconds;
	struct timespec deadline;
	int list;
	int result;

	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
		{
			lists[list] = NULL;
			counts[list] = NULL;
		}
	}
	if (!infinite)
		deadline_after(&deadline, microseconds < 0 ? 0 : microseconds);
	pthread_mutex_lock(&network_lock);
	while (select_ready(lists, counts, false) == 0)
	{
		if (!infinite && microseconds <= 0)
			break;
		if (!wait_for_change(infinite ? NULL : &deadline))
			break;
	}
	result = select_ready(lists, counts, true);
	pthread_mutex_unlock(&network_lock);
	/* like Winsock, a select with nothing ready leaves the last error as it
	was */
	if (result > 0)
		last_error = 0;
	return result;
}

/* ---------- addresses, random numbers and the process */

posix_ulong posix_local_ipv4_address(void)
{
	return web_net_address();
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	unsigned char *cursor = buffer;

	/* (the browser's crypto.getRandomValues, 256 bytes at a time) */
	while (size)
	{
		size_t count = size < 256 ? size : 256;

		if (getentropy(cursor, count) != 0)
		{
			fputs("no random numbers from the browser: cannot continue\n", stderr);
			abort();
		}
		cursor += count;
		size -= (posix_ulong)count;
	}
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	struct in_addr address;

	/* only dotted quads: the page's network has no names */
	if (host && inet_pton(AF_INET, host, &address) == 1)
		return address.s_addr;
	return 0;
}

int posix_command_line_argument(int index, char *buffer, posix_ulong size)
{
	(void)index; (void)buffer; (void)size;
	return 0;
}

posix_ulong posix_process_id(void)
{
	return (posix_ulong)getpid();
}

int posix_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme; (void)description;
	return 0;
}

int posix_user_secret(unsigned char *secret, int size)
{
	(void)secret; (void)size;
	return 0;
}

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port; (void)preferred_port; (void)external_address; (void)external_port;
	snprintf(error, (size_t)error_size, "a page has no router to ask");
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}

int posix_discord_connect(void)
{
	return -1;
}

int posix_discord_write(int handle, const void *buffer, int length)
{
	(void)handle; (void)buffer; (void)length;
	return -1;
}

int posix_discord_read(int handle, void *buffer, int length)
{
	(void)handle; (void)buffer; (void)length;
	return -1;
}

void posix_discord_close(int handle)
{
	(void)handle;
}
