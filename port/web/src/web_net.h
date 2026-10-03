/*
WEB_NET.H

The network between the players' pages: this page's address, and the
bridge that carries datagrams and connections to the other pages
(posix_bridge.c, with port/web/shell/net.js). posix_net_web.c, the sockets,
is its other side.
*/

#ifndef __HALO_WEB_NET_H
#define __HALO_WEB_NET_H

#include <stdbool.h>
#include <stdint.h>

/* this page's address and its network's broadcast address (network order) */
uint32_t web_net_address(void);
uint32_t web_net_broadcast_address(void);
void web_net_initialize(void);

/* the sockets to the bridge */
void web_net_send_datagram(uint32_t to_address, uint16_t to_port, uint16_t from_port, const void *data, uint32_t size);
uint32_t web_net_connect(int socket_index, uint32_t to_address, uint16_t to_port, uint16_t from_port);
void web_net_send_stream(uint32_t connection, const void *data, uint32_t size);
void web_net_close_connection(uint32_t connection);

/* the bridge to the sockets */
void web_net_receive_datagram(uint32_t from_address, uint16_t from_port, uint16_t to_port, const void *data, uint32_t size);
int web_net_receive_connection(uint32_t connection, uint32_t from_address, uint16_t from_port, uint16_t to_port);
void web_net_receive_stream(int socket_index, const void *data, uint32_t size);
void web_net_connection_result(int socket_index, bool connected);

#endif
