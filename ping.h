/*
  Copyright Red Hat, Inc. 2003

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge,
  MA 02139, USA.
 */
/** @file
 * Header for ping.c.
 */
#ifndef __PING_H
#define __PING_H

#include <netinet/ip_icmp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define PING_ERRNO		-1
#define PING_SUCCESS		0
#define PING_ALIVE		PING_SUCCESS
#define PING_TIMEOUT		1
#define PING_HOST_UNREACH	2
#define PING_HOST_NOT_FOUND	3
#define PING_INVALID_CHECKSUM	4
#define PING_INVALID_RESPONSE	5
#define PING_INVALID_SIZE	6
#define PING_INVALID_ID		7

int32_t icmp_socket(void);
int32_t icmp_ping_hostfd(int32_t sock, char *hostname, uint32_t seq,
			uint32_t timeout);
int32_t icmp_ping_host(char *hostname, uint32_t seq,uint32_t timeout);

int32_t icmp_ping_addrfd(int32_t sock, struct sockaddr_in *sin_send, uint32_t seq,
		    uint32_t timeout);
int32_t icmp_ping_addr(struct sockaddr_in *sin_send, uint32_t seq,uint32_t timeout);

/* NOT reentrant - uses static buffers */
char *icmp_ping_strerror(int rv);

#define net_icmp_close(sock) close(sock)

#endif
