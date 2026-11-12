/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef JUICE_SERVER_H
#define JUICE_SERVER_H

#ifndef NO_SERVER

#include "addr.h"
#include "juice.h"
#include "socket.h"
#include "stun.h"
#include "thread.h"
#include "timestamp.h"
#include "turn.h"
#include "turn_tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define SERVER_DEFAULT_REALM "libjuice"
#define SERVER_DEFAULT_MAX_ALLOCATIONS 1000 // should be 1024-1 or less to be safe for poll()
#define SERVER_DEFAULT_MAX_PEERS 16

#define SERVER_NONCE_KEY_SIZE 32

// RFC 8656: The server [...] SHOULD expire the nonce at least once every hour during the lifetime
// of the allocation
#define SERVER_NONCE_KEY_LIFETIME 600 * 1000 // 10 min

typedef enum server_turn_alloc_state {
	SERVER_TURN_ALLOC_EMPTY,
	SERVER_TURN_ALLOC_DELETED,
	SERVER_TURN_ALLOC_FULL
} server_turn_alloc_state_t;

typedef struct server_turn_alloc {
	server_turn_alloc_state_t state;
	addr_record_t record;
	juice_server_credentials_t *credentials;
	uint8_t transaction_id[STUN_TRANSACTION_ID_SIZE];
	timestamp_t timestamp;
	socket_t sock;
	turn_map_t map;

	// Stream transport (TURN over TCP, RFC 8656 section 12).
	//
	// When this is set, the allocation was created over a TCP control
	// connection and the responses are sent over that stream instead of
	// over the server's UDP socket.
	turn_tcp_stream_t *stream;
} server_turn_alloc_t;

typedef struct juice_credentials_list {
	struct juice_credentials_list *next;
	juice_server_credentials_t credentials;
	uint8_t userhash[USERHASH_SIZE];
	timestamp_t timestamp;
} juice_credentials_list_t;

// A TCP control connection accepted by the server.
typedef struct server_turn_session {
	struct server_turn_session *next;
	turn_tcp_stream_t *stream;
	addr_record_t peer;
} server_turn_session_t;

// The connection a TURN message was received on.
//
// For UDP this is the peer address only.  For TURN over TCP it also
// carries the control connection the message arrived on, so that the
// response goes back over the very same stream.
typedef struct server_conn {
	addr_record_t src;
	turn_tcp_stream_t *stream; // NULL for UDP
} server_conn_t;

typedef struct juice_server {
	juice_server_config_t config;          // Note config.credentials will be empty
	juice_credentials_list_t *credentials; // Credentials are stored in this list
	uint8_t nonce_key[SERVER_NONCE_KEY_SIZE];
	timestamp_t nonce_key_timestamp;
	socket_t sock;
	socket_t tcp_sock;
	server_turn_session_t *sessions;
	server_conn_t *current_conn; // set while dispatching a message
	thread_t thread;
	mutex_t mutex;
	bool thread_stopped;
	server_turn_alloc_t *allocs;
	int allocs_count;
} juice_server_t;

juice_server_t *server_create(const juice_server_config_t *config);
void server_do_destroy(juice_server_t *server);
void server_destroy(juice_server_t *server);

uint16_t server_get_port(juice_server_t *server);
int server_add_credentials(juice_server_t *server, const juice_server_credentials_t *credentials,
                           timediff_t lifetime);

juice_credentials_list_t *server_do_add_credentials(juice_server_t *server,
                                                    const juice_server_credentials_t *credentials,
                                                    timediff_t lifetime); // internal

void server_run(juice_server_t *server);
int server_send(juice_server_t *agent, const addr_record_t *dst, const char *data, size_t size);
int server_stun_send(juice_server_t *server, const addr_record_t *dst, const stun_message_t *msg,
                     const char *password // password may be NULL
);
int server_recv(juice_server_t *server);
int server_forward(juice_server_t *server, server_turn_alloc_t *alloc);
int server_input(juice_server_t *agent, char *buf, size_t len, const addr_record_t *src);
int server_input_conn(juice_server_t *server, char *buf, size_t len, const server_conn_t *conn);
int server_interrupt(juice_server_t *server);
int server_bookkeeping(juice_server_t *agent, timestamp_t *next_timestamp);

void server_get_nonce(juice_server_t *server, const addr_record_t *src, char *nonce);
void server_prepare_credentials(juice_server_t *server, const addr_record_t *src,
                                const juice_server_credentials_t *credentials, stun_message_t *msg);

int server_dispatch_stun(juice_server_t *server, void *buf, size_t size, stun_message_t *msg,
                         const addr_record_t *src);
int server_answer_stun_binding(juice_server_t *server, const uint8_t *transaction_id,
                               const addr_record_t *src);
int server_answer_stun_error(juice_server_t *server, const uint8_t *transaction_id,
                             const addr_record_t *src, stun_method_t method, unsigned int code,
                             const juice_server_credentials_t *credentials);

int server_process_stun_binding(juice_server_t *server, const stun_message_t *msg,
                                const addr_record_t *src);
int server_process_turn_allocate(juice_server_t *server, const stun_message_t *msg,
                                 const addr_record_t *src, juice_server_credentials_t *credentials);
int server_process_turn_create_permission(juice_server_t *server, const stun_message_t *msg,
                                          const addr_record_t *src,
                                          const juice_server_credentials_t *credentials);
int server_process_turn_channel_bind(juice_server_t *server, const stun_message_t *msg,
                                     const addr_record_t *src,
                                     const juice_server_credentials_t *credentials);
int server_process_turn_send(juice_server_t *server, const stun_message_t *msg,
                             const addr_record_t *src);
int server_process_channel_data(juice_server_t *server, char *buf, size_t len,
                                const addr_record_t *src);

// TURN over TCP (RFC 8656 section 12)
int server_tcp_listen(juice_server_t *server);
void server_tcp_close(juice_server_t *server);
int server_tcp_accept(juice_server_t *server);
int server_tcp_session_recv(juice_server_t *server, server_turn_session_t *session);
void server_tcp_session_close(juice_server_t *server, server_turn_session_t *session);

#endif // ifndef NO_SERVER

#endif
