/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "turn_tcp.h"
#include "log.h"
#include "turn.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define TURN_TCP_INITIAL_BUFFER_SIZE 4096

int turn_tcp_decode(const char *buf, size_t len, turn_tcp_frame_t *frame) {
	if (len < 1)
		return 0;

	const uint8_t *p = (const uint8_t *)buf;

	// RFC 7983 section 5: the first byte tells the message types apart.
	//
	//   [0..3]    STUN
	//   [64..79]  TURN Channel
	//
	// A STUN message has its two most significant bits set to zero, so
	// the first byte is in [0..63].  A ChannelData message has a channel
	// number in [0x4000..0x4FFF], so its first byte is in [64..79].
	if ((p[0] & 0xC0) == 0x00) {
		// STUN message
		if (len < 20)
			return 0;

		uint16_t length = (uint16_t)((p[2] << 8) | p[3]);
		// RFC 8489 section 14: the message length does not include the
		// 20-byte header.
		size_t total = 20 + (size_t)length;
		if (len < total)
			return 0;

		frame->consumed = total;
		frame->size = total;
		return 1;
	}

	if (p[0] >= 64 && p[0] <= 79) {
		// ChannelData message
		if (len < 4)
			return 0;

		uint16_t length = (uint16_t)((p[2] << 8) | p[3]);
		// RFC 8656 section 12.5: over TCP the payload is padded to a
		// multiple of four bytes, and the padding is not reflected in
		// the Length field.
		size_t padded = (size_t)((length + 3) & ~((size_t)3));
		size_t total = 4 + padded;
		if (len < total)
			return 0;

		frame->consumed = total;
		frame->size = 4 + (size_t)length; // padding excluded
		return 1;
	}

	JLOG_WARN("Unexpected message type on the TURN stream, first byte=0x%02X", p[0]);
	return -1;
}

int turn_tcp_set_nonblocking(socket_t sock) {
	ctl_t nbio = 1;
	if (ioctlsocket(sock, FIONBIO, &nbio)) {
		JLOG_ERROR("Setting non-blocking mode on TCP socket failed, errno=%d", sockerrno);
		return -1;
	}
	return 0;
}

static int ensure_capacity(char **buffer, size_t *capacity, size_t needed) {
	if (*capacity >= needed)
		return 0;

	size_t new_capacity = *capacity ? *capacity : TURN_TCP_INITIAL_BUFFER_SIZE;
	while (new_capacity < needed)
		new_capacity *= 2;

	char *new_buffer = realloc(*buffer, new_capacity);
	if (!new_buffer) {
		JLOG_FATAL("Memory allocation for TURN stream buffer failed");
		return -1;
	}

	*buffer = new_buffer;
	*capacity = new_capacity;
	return 0;
}

turn_tcp_stream_t *turn_tcp_stream_create(const addr_record_t *dst) {
	socket_t sock = socket(dst->addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET) {
		JLOG_WARN("TCP socket creation failed, errno=%d", sockerrno);
		return NULL;
	}

	if (turn_tcp_set_nonblocking(sock) < 0) {
		closesocket(sock);
		return NULL;
	}

	int ret = connect(sock, (const struct sockaddr *)&dst->addr, dst->len);
	if (ret != 0 && sockerrno != SEINPROGRESS && sockerrno != SEWOULDBLOCK) {
		JLOG_WARN("TCP connection failed, errno=%d", sockerrno);
		closesocket(sock);
		return NULL;
	}

	turn_tcp_stream_t *stream = calloc(1, sizeof(turn_tcp_stream_t));
	if (!stream) {
		JLOG_FATAL("Memory allocation for TURN stream failed");
		closesocket(sock);
		return NULL;
	}

	stream->sock = sock;
	stream->connected = false;
	stream->failed = false;
	mutex_init(&stream->mutex, MUTEX_RECURSIVE);
	return stream;
}

turn_tcp_stream_t *turn_tcp_stream_create_accepted(socket_t sock) {
	if (sock == INVALID_SOCKET)
		return NULL;

	if (turn_tcp_set_nonblocking(sock) < 0) {
		closesocket(sock);
		return NULL;
	}

	turn_tcp_stream_t *stream = calloc(1, sizeof(turn_tcp_stream_t));
	if (!stream) {
		JLOG_FATAL("Memory allocation for TURN stream failed");
		closesocket(sock);
		return NULL;
	}

	stream->sock = sock;
	stream->connected = true;
	stream->failed = false;
	mutex_init(&stream->mutex, MUTEX_RECURSIVE);
	return stream;
}

void turn_tcp_stream_destroy(turn_tcp_stream_t *stream) {
	if (!stream)
		return;

	if (stream->sock != INVALID_SOCKET)
		closesocket(stream->sock);

	mutex_destroy(&stream->mutex);
	free(stream->wbuf);
	free(stream->rbuf);
	free(stream);
}

socket_t turn_tcp_stream_socket(const turn_tcp_stream_t *stream) {
	return stream ? stream->sock : INVALID_SOCKET;
}

bool turn_tcp_stream_is_connected(turn_tcp_stream_t *stream) {
	if (!stream)
		return false;

	mutex_lock(&stream->mutex);
	bool connected = stream->connected;
	mutex_unlock(&stream->mutex);
	return connected;
}

bool turn_tcp_stream_is_failed(turn_tcp_stream_t *stream) {
	if (!stream)
		return true;

	mutex_lock(&stream->mutex);
	bool failed = stream->failed;
	mutex_unlock(&stream->mutex);
	return failed;
}

int turn_tcp_stream_check_connected(turn_tcp_stream_t *stream) {
	if (!stream)
		return -1;

	mutex_lock(&stream->mutex);

	if (stream->failed) {
		mutex_unlock(&stream->mutex);
		return -1;
	}

	if (stream->connected) {
		mutex_unlock(&stream->mutex);
		return 1;
	}

	// The socket is non-blocking, so the connection is either still in
	// progress or has completed.  A socket that is not writable yet means
	// the connection attempt is still running; a socket that became
	// writable either succeeded or failed, and SO_ERROR tells which.
	fd_set wset;
	FD_ZERO(&wset);
	FD_SET(stream->sock, &wset);
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	int ret = select(stream->sock + 1, NULL, &wset, NULL, &tv);
	if (ret < 0) {
		if (sockerrno == SEINTR) {
			mutex_unlock(&stream->mutex);
			return 0;
		}
		JLOG_WARN("select failed, errno=%d", sockerrno);
		stream->failed = true;
		mutex_unlock(&stream->mutex);
		return -1;
	}

	if (ret == 0) {
		// Still connecting.
		mutex_unlock(&stream->mutex);
		return 0;
	}

	int err = 0;
	socklen_t err_len = sizeof(err);
	if (getsockopt(stream->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &err_len) != 0) {
		JLOG_WARN("getsockopt failed, errno=%d", sockerrno);
		stream->failed = true;
		mutex_unlock(&stream->mutex);
		return -1;
	}

	if (err != 0) {
		JLOG_WARN("TCP connection failed, errno=%d", err);
		stream->failed = true;
		mutex_unlock(&stream->mutex);
		return -1;
	}

	stream->connected = true;
	mutex_unlock(&stream->mutex);
	return 1;
}

int turn_tcp_stream_wait(turn_tcp_stream_t *stream, int timeout_ms) {
	if (!stream)
		return -1;

	mutex_lock(&stream->mutex);

	if (stream->failed) {
		mutex_unlock(&stream->mutex);
		return -1;
	}

	fd_set rset;
	FD_ZERO(&rset);
	FD_SET(stream->sock, &rset);
	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	int ret = select(stream->sock + 1, &rset, NULL, NULL, &tv);
	if (ret < 0) {
		if (sockerrno == SEINTR) {
			mutex_unlock(&stream->mutex);
			return 0;
		}
		JLOG_WARN("select failed, errno=%d", sockerrno);
		stream->failed = true;
		mutex_unlock(&stream->mutex);
		return -1;
	}

	mutex_unlock(&stream->mutex);
	return ret > 0 ? 1 : 0;
}

static int flush_locked(turn_tcp_stream_t *stream) {
	// Nothing can be written before the connection is established.  The
	// data stays in the write buffer and is flushed once the connection
	// completes.
	if (!stream->connected)
		return 0;

	while (stream->wpos < stream->wlen) {
#if defined(__APPLE__) || defined(_WIN32)
		int flags = 0;
#else
		int flags = MSG_NOSIGNAL;
#endif
		int len = send(stream->sock, stream->wbuf + stream->wpos,
		               (socklen_t)(stream->wlen - stream->wpos), flags);
		if (len < 0) {
			if (sockerrno == SEAGAIN || sockerrno == SEWOULDBLOCK) {
				JLOG_VERBOSE("TURN stream write would block");
				return 0;
			}
			JLOG_WARN("TURN stream write failed, errno=%d", sockerrno);
			stream->failed = true;
			return -1;
		}

		if (len == 0) {
			stream->failed = true;
			return -1;
		}

		stream->wpos += (size_t)len;
	}

	// Everything has been written, reset the buffer.
	stream->wpos = 0;
	stream->wlen = 0;
	return 0;
}

int turn_tcp_stream_flush(turn_tcp_stream_t *stream) {
	if (!stream)
		return -1;

	mutex_lock(&stream->mutex);
	int ret = flush_locked(stream);
	mutex_unlock(&stream->mutex);
	return ret;
}

bool turn_tcp_stream_has_pending(turn_tcp_stream_t *stream) {
	if (!stream)
		return false;

	mutex_lock(&stream->mutex);
	bool pending = stream->wpos < stream->wlen;
	mutex_unlock(&stream->mutex);
	return pending;
}

int turn_tcp_stream_send(turn_tcp_stream_t *stream, const char *data, size_t size) {
	if (!stream || !data)
		return -1;

	mutex_lock(&stream->mutex);

	if (stream->failed) {
		mutex_unlock(&stream->mutex);
		return -1;
	}

	// Compact the buffer if it has been fully written.
	if (stream->wpos == stream->wlen) {
		stream->wpos = 0;
		stream->wlen = 0;
	} else if (stream->wpos > 0) {
		memmove(stream->wbuf, stream->wbuf + stream->wpos, stream->wlen - stream->wpos);
		stream->wlen -= stream->wpos;
		stream->wpos = 0;
	}

	if (ensure_capacity(&stream->wbuf, &stream->wcap, stream->wlen + size) < 0) {
		mutex_unlock(&stream->mutex);
		return -1;
	}

	memcpy(stream->wbuf + stream->wlen, data, size);
	stream->wlen += size;

	int ret = flush_locked(stream);
	mutex_unlock(&stream->mutex);
	return ret;
}

int turn_tcp_stream_send_channel_data(turn_tcp_stream_t *stream, const char *data, size_t size,
                                       uint16_t channel) {
	if (!stream || !data)
		return -1;

	if (!is_valid_channel(channel)) {
		JLOG_WARN("Invalid channel number: 0x%hX", channel);
		return -1;
	}

	if (size > 0xFFFF) {
		JLOG_WARN("ChannelData is too long, size=%zu", size);
		return -1;
	}

	// RFC 8656 section 12.5: over TCP the payload is padded to a multiple
	// of four bytes, and the padding is not reflected in the Length field.
	size_t padded = (size + 3) & ~((size_t)3);
	size_t total = sizeof(struct channel_data_header) + padded;

	char *buffer = malloc(total);
	if (!buffer) {
		JLOG_FATAL("Memory allocation for ChannelData failed");
		return -1;
	}

	struct channel_data_header *header = (struct channel_data_header *)buffer;
	header->channel_number = htons(channel);
	header->length = htons((uint16_t)size);
	memcpy(buffer + sizeof(struct channel_data_header), data, size);
	if (padded > size)
		memset(buffer + sizeof(struct channel_data_header) + size, 0, padded - size);

	int ret = turn_tcp_stream_send(stream, buffer, total);
	free(buffer);
	return ret;
}

int turn_tcp_stream_recv(turn_tcp_stream_t *stream, char *buffer, size_t size, size_t *out_size) {
	if (!stream || !buffer || !out_size)
		return -1;

	mutex_lock(&stream->mutex);

	if (stream->failed) {
		mutex_unlock(&stream->mutex);
		return -1;
	}

	// Try to decode a message out of what has already been read.
	for (;;) {
		size_t available = stream->rlen - stream->rpos;
		if (available > 0) {
			turn_tcp_frame_t frame;
			int ret = turn_tcp_decode(stream->rbuf + stream->rpos, available, &frame);
			if (ret < 0) {
				stream->failed = true;
				mutex_unlock(&stream->mutex);
				return -1;
			}

			if (ret == 1) {
				if (frame.size > size) {
					JLOG_WARN("TURN stream message is too large, size=%zu", frame.size);
					stream->failed = true;
					mutex_unlock(&stream->mutex);
					return -1;
				}

				memcpy(buffer, stream->rbuf + stream->rpos, frame.size);
				*out_size = frame.size;
				stream->rpos += frame.consumed;

				if (stream->rpos == stream->rlen) {
					stream->rpos = 0;
					stream->rlen = 0;
				}

				mutex_unlock(&stream->mutex);
				return 1;
			}
		}

		// Move the pending bytes to the beginning of the buffer.
		if (stream->rpos > 0) {
			memmove(stream->rbuf, stream->rbuf + stream->rpos, stream->rlen - stream->rpos);
			stream->rlen -= stream->rpos;
			stream->rpos = 0;
		}

		// Read more data.
		if (ensure_capacity(&stream->rbuf, &stream->rcap,
		                    stream->rlen + TURN_TCP_INITIAL_BUFFER_SIZE) < 0) {
			stream->failed = true;
			mutex_unlock(&stream->mutex);
			return -1;
		}

		int len = recv(stream->sock, stream->rbuf + stream->rlen,
		               (socklen_t)(stream->rcap - stream->rlen), 0);
		if (len < 0) {
			if (sockerrno == SEAGAIN || sockerrno == SEWOULDBLOCK) {
				JLOG_VERBOSE("No more data on the TURN stream");
				mutex_unlock(&stream->mutex);
				return 0;
			}
			JLOG_WARN("TURN stream read failed, errno=%d", sockerrno);
			stream->failed = true;
			mutex_unlock(&stream->mutex);
			return -1;
		}

		if (len == 0) {
			JLOG_INFO("TURN stream closed by the peer");
			stream->failed = true;
			mutex_unlock(&stream->mutex);
			return -1;
		}

		stream->rlen += (size_t)len;
	}
}
