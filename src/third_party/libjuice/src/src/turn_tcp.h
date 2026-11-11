/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef JUICE_TURN_TCP_H
#define JUICE_TURN_TCP_H

#include "addr.h"
#include "socket.h"
#include "thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum size of a single TURN message carried over a stream transport.
//
// A STUN message is at most 20 (header) + 65535 (payload) bytes, a
// ChannelData message at most 4 (header) + 65535 (payload) bytes, padded
// to a 4-byte boundary.
#define TURN_TCP_MAX_MESSAGE_SIZE (20 + 65535 + 4)

// Framing over a stream transport.
//
// RFC 8656 section 12.4 and section 12.5 define the ChannelData message,
// RFC 8489 section 14 defines the framing of STUN messages over TCP.
//
// Contrary to ICE-TCP (RFC 6544), which prefixes every message with a
// 16-bit length (RFC 4571), TURN over TCP relies on the messages being
// self-delimiting:
//
//   - A STUN message starts with a 20-byte header whose Length field
//     gives the size of the payload.  Its total size on the wire is
//     20 + Length.  The two most significant bits of the first byte are
//     zeroes, which distinguishes it from a ChannelData message.
//
//   - A ChannelData message starts with a 4-byte header whose Length
//     field gives the size of the application data, *excluding* the
//     padding.  Over TCP the payload MUST be padded to a 4-byte
//     boundary (RFC 8656 section 12.5), so the message occupies
//     4 + round_up(Length, 4) bytes on the wire.
//
// The two message types are told apart by examining the first byte, as
// described in RFC 7983 section 5.
typedef struct turn_tcp_frame {
	size_t consumed; // bytes consumed from the stream
	size_t size;     // size of the message to deliver (padding excluded)
} turn_tcp_frame_t;

// Decode a single message from a stream buffer.
//
// Returns 1 if a complete message is available (frame is filled), 0 if more
// data is needed, -1 on a protocol error (the stream must be closed).
int turn_tcp_decode(const char *buf, size_t len, turn_tcp_frame_t *frame);

// Put a socket in non-blocking mode.
int turn_tcp_set_nonblocking(socket_t sock);

// A TURN connection carried over a stream transport (TCP).
typedef struct turn_tcp_stream {
	socket_t sock;
	mutex_t mutex;

	char *wbuf; // write buffer
	size_t wcap;
	size_t wlen;
	size_t wpos;

	char *rbuf; // read buffer
	size_t rcap;
	size_t rlen;
	size_t rpos;

	bool connected;
	bool failed;
} turn_tcp_stream_t;

// Create a stream and start connecting to the given address.
// Returns NULL on failure.
turn_tcp_stream_t *turn_tcp_stream_create(const addr_record_t *dst);

// Wrap an already-connected socket (e.g. one returned by accept()).
turn_tcp_stream_t *turn_tcp_stream_create_accepted(socket_t sock);

void turn_tcp_stream_destroy(turn_tcp_stream_t *stream);

socket_t turn_tcp_stream_socket(const turn_tcp_stream_t *stream);

bool turn_tcp_stream_is_connected(turn_tcp_stream_t *stream);

bool turn_tcp_stream_is_failed(turn_tcp_stream_t *stream);

// Check whether the pending connection attempt has completed.
// Returns 1 if connected, 0 if still pending, -1 if the connection failed.
int turn_tcp_stream_check_connected(turn_tcp_stream_t *stream);

// Wait for the stream to become readable.
// Returns 1 if data is available, 0 on timeout, -1 on error.
int turn_tcp_stream_wait(turn_tcp_stream_t *stream, int timeout_ms);

// Send a raw (already self-delimiting) message.
// The data is buffered and flushed as the socket allows.
int turn_tcp_stream_send(turn_tcp_stream_t *stream, const char *data, size_t size);

// Send application data wrapped in a ChannelData message, padded to a
// 4-byte boundary as required over TCP by RFC 8656 section 12.5.
int turn_tcp_stream_send_channel_data(turn_tcp_stream_t *stream, const char *data, size_t size,
                                       uint16_t channel);

// Flush pending writes. Returns 0 on success, -1 on failure.
int turn_tcp_stream_flush(turn_tcp_stream_t *stream);

// True if bytes are still waiting to be written.
bool turn_tcp_stream_has_pending(turn_tcp_stream_t *stream);

// Read and decode one message.
// Returns 1 if a message was decoded (buffer and out_size are filled), 0 if
// no complete message is available yet, -1 on error or if the peer closed
// the connection.
int turn_tcp_stream_recv(turn_tcp_stream_t *stream, char *buffer, size_t size, size_t *out_size);

#endif
