/*
  Copyright Red Hat, Inc. 2003, 2008

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
 * Ping functions for Red Hat Cluster Manager (RFC777)
 *
 * These functions provide internal ping functionality.  These functions are
 * used to help determine quorum when a two or four node cluster has an
 * even split.
 */
 
#include <ping.h>
#include <ctype.h>

/**
 * From RFC 777:
 *
 * [ICMP] Header Checksum
 * The 16 bit one's complement of the one's complement sum of all 16
 * bit words in the header.  For computing the checksum, the checksum
 * field should be zero.  This checksum may be replaced in the
 * future.
 *
 * @param buf		16-but word array
 * @param buflen	Length (in 8-bit bytes!) of buf
 * @return		ICMP header checksum of buf.
 */
uint16_t
icmp_checksum(uint16_t *buf, uint32_t buflen)
{
	uint32_t remain = buflen, sum = 0;
	char *data = (char *)buf;

	while (remain > 1) {
		sum += *((uint16_t *)data);
		data += 2;
		remain -= 2;
	}

	/*
	 * Clean up the last byte (if there is one)
	 */
	if (remain)
		sum += *data;

	sum = (sum>>16)+(sum&0xffff);
	sum += (sum>>16);
	
	return (uint16_t)((~sum)&0xffff);
}


/**
 * Set up an ICMP socket (basically, a raw socket).  This requires root 
 * privileges.
 *
 * @return See socket(2).
 */
int32_t
icmp_socket(void)
{
	return socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
}


/**
 * Call gethostbyname and return an appropriate PING return value based on
 * the response we get.  Fill in sin_send.
 *
 * @param hostname	Hostname to look up.
 * @param sin_send	IP address structure (pre-allocated).
 * @return		PING_HOST_NOT_FOUND if the host is not found, 0
 *			on success, -1 on other error.
 * @see gethostbyname
 */
int32_t
icmp_ping_getaddr(char *hostname, struct sockaddr_in *sin_send)
{
	struct hostent *hp;

	memset(sin_send, 0, sizeof(*sin_send));
	sin_send->sin_family = AF_INET;
	sin_send->sin_port = 0;

	/*
	 * Don't do a DNS lookup if someone gave us an address in dotted
 	 * quad format; it's a waste of time and slows things down
	 *
	 * Per the man page, host names "must begin with alphabetic and end
	 * with an alphanumeric character".  Ergo, if it begins with a digit,
	 * it is probably an address in dotted-quad form.
 	 */
	if (isdigit(hostname[0]) &&
	    inet_pton(AF_INET, hostname, &sin_send->sin_addr) > 0) {
		return 0;
	}

	/*
	 * Grab the hostname
	 */
	while ((hp = gethostbyname(hostname)) == NULL) {
		switch(h_errno) {
		case TRY_AGAIN:
			continue;
		case HOST_NOT_FOUND:
		case NO_ADDRESS:
		case NO_RECOVERY:
			return PING_HOST_NOT_FOUND;
		}
		return -1;
	}

	/*
	 * Copy in target address from DNS/hosts/etc.
	 */
	memcpy(&sin_send->sin_addr, hp->h_addr, sizeof(sin_send->sin_addr));

	return 0;
}


/**
 * Send a ping (ICMP_ECHO) to a given IP address and file descriptor.
 * This is set up so that a daemon can drop privileges after binding to a raw
 * socket (perhaps preserving a static ping socket), but still be able to use
 * ping.  This is a pretty long function.
 *
 * @param sock		Socket to send on.
 * @param sin_send	Address to send to.
 * @param seq		Sequence number.
 * @param timeout	Timeout (in seconds)
 * @return		-1 on syscall error, 0 on success.
 *			See ping.h for list of return values >0.
 * @see	icmp_ping_host icmp_ping_hostfd icmp_ping_addr
 */
int32_t
icmp_ping_addrfd(int32_t sock, struct sockaddr_in *sin_send, uint32_t seq,
	    uint32_t timeout)
{
	char buffer[256]; /* XXX */
	struct icmp *packetp;
	struct ip *ipp;
	struct sockaddr_in sin_recv;
	struct timeval tv;
	fd_set rfds;
	uint16_t checksum;
	uint16_t packetlen = sizeof(uint16_t) * ICMP_MINLEN;
	uint32_t x, sin_recv_len = sizeof(sin_recv);

	/*
	 * Set up ICMP echo packet
	 */
	packetp = (struct icmp *)buffer;

	memset(buffer, 0, sizeof(buffer));
	packetp->icmp_type = ICMP_ECHO;
	packetp->icmp_seq = seq;
	packetp->icmp_id = getpid();
	packetp->icmp_cksum = icmp_checksum((uint16_t *)packetp,
						ICMP_MINLEN);

	/*
	 * Send the packet
	 */
	while ((x = sendto(sock, packetp, packetlen, 0,
	    		   (struct sockaddr *)sin_send,
			   sizeof(*sin_send))) < packetlen)
		if (x < 0)
			return -1;

	if (timeout) {
		tv.tv_sec = timeout;
		tv.tv_usec = 0;
	}

	/*
	 * Wait for response
	 */
	while (1) {
		
		/*
		 * Set up select call...
		 */
		FD_ZERO(&rfds);
		FD_SET(sock,&rfds);
		while ((x = select(sock+1, &rfds, NULL, NULL,
		   		   timeout ? &tv : NULL)) <= 0) {
			if (!x)
				return PING_TIMEOUT;
			return -1;
		}
		
		/*
		 * Receive response
		 */
		if ((x = recvfrom(sock, buffer, sizeof(buffer), 0,
				  (struct sockaddr *)&sin_recv,
				  &sin_recv_len)) < 0) {
			return -1;
		}

		/*
		 * Ensure it's the proper size...
		 * - (ipp->ip_hl << 2) is because IP header length is in
		 * 32-bit words instead of bytes.
		 * - ICMP_MINLEN is defined in netinet/ip_icmp.h.
		 */
		ipp = (struct ip *)buffer;
		if (x < ((ipp->ip_hl << 2) + ICMP_MINLEN)) {
			if (timeout)
				continue;
			return PING_INVALID_SIZE;
		}
		
		/*
		 * Validate the checksum.  The checksum needs to be set to
		 * 0 for validation purposes.
		 */
		packetp = (struct icmp *)((buffer + (ipp->ip_hl << 2)));
		checksum = packetp->icmp_cksum;
		packetp->icmp_cksum = 0;
		if (checksum != icmp_checksum((uint16_t*)packetp,
					      ICMP_MINLEN)) {
			if (timeout)
				continue;
			return PING_INVALID_CHECKSUM;
		}

		/*
		 * Ensure it's the proper id...
		 */
		switch (packetp->icmp_type) {
		case ICMP_ECHO:
		case ICMP_ECHOREPLY:
			if (packetp->icmp_id != getpid()) {
				if (timeout)
					continue;
				return PING_INVALID_ID;
			}
			return PING_SUCCESS;
		case ICMP_DEST_UNREACH:
			return PING_HOST_UNREACH;
		}

		/* XXX */
		return PING_INVALID_RESPONSE;
	}
}


/**
 * Send a ping (ICMP_ECHO) to a given IP address.  This is set up so that a
 * daemon can drop privileges after binding to a raw socket (perhaps
 * preserving a static ping socket), but still be able to use ping calls.
 *
 * @param sock		Socket to send on
 * @param hostname	Host name to ping
 * @param seq		Sequence number.
 * @param timeout	Timeout (in seconds)
 * @return		-1 on syscall error, 0 on success.
 *			See ping.h for list of return values >0.
 * @see	icmp_ping_getaddr icmp_ping_addrfd icmp_ping 
 */
int32_t
icmp_ping_hostfd(int32_t sock, char *hostname, uint32_t seq, uint32_t timeout)
{
	struct sockaddr_in sin_send;
	int rv;

	rv = icmp_ping_getaddr(hostname, &sin_send);
	if (rv)
		return rv;

	return icmp_ping_addrfd(sock, &sin_send, seq, timeout);
}


/**
 * Send a ping (ICMP_ECHO) to a given IP address. 
 *
 * @param sin_send	Address we want to send to.
 * @param seq		Sequence number (user defined)
 * @param timeout	Timeout (in seconds)
 * @return		-1 on syscall error, 0 on success.
 *			See ping.h for list of return values >0.
 * @see	icmp_ping_addrfd icmp_socket icmp_ping_host icmp_ping_hostfd
 */
int32_t
icmp_ping_addr(struct sockaddr_in *sin_send, uint32_t seq, uint32_t timeout)
{
	int sock, rv, esv;

	sock = icmp_socket();
	if (sock < 0)
		return -1;

	rv = icmp_ping_addrfd(sock, sin_send, seq, timeout);

	esv = errno;
	close(sock);
	errno = esv;

	return rv;
}


/**
 * Send a ping (ICMP_ECHO) to a given hostname.
 *
 * @param hostname	Target host
 * @param seq		Sequence number (user defined)
 * @param timeout	Timeout (in seconds)
 * @return		-1 on syscall error, 0 on success.
 *			See ping.h for list of return values >0.
 * @see	icmp_socket icmp_ping_hostfd icmp_ping_addr icmp_ping_addrfd
 */
int32_t
icmp_ping_host(char *hostname, uint32_t seq, uint32_t timeout)
{
	uint32_t rv, sock, esv;

	sock = icmp_socket();
	if (sock == -1)
		return -1;

	rv = icmp_ping_hostfd(sock, hostname, seq, timeout);

	esv = errno;
	close(sock);
	errno = esv;

	return rv;
}


/* WARNING: Not reentrant */
char *
icmp_ping_strerror(int rv)
{
	static char buf[80];

	switch(rv) {
	case PING_ERRNO:
		return strerror(errno);
	case PING_SUCCESS:
		return strerror(0);
	case PING_TIMEOUT:
		return strerror(ETIMEDOUT);
	case PING_HOST_UNREACH:
		return strerror(EHOSTUNREACH);
	case PING_HOST_NOT_FOUND:
		snprintf(buf, sizeof(buf), "Host not found");
		return buf;
	case PING_INVALID_CHECKSUM:
		snprintf(buf, sizeof(buf), "Invalid checksum");
		break;
	case PING_INVALID_SIZE:
		snprintf(buf, sizeof(buf), "Invalid size of reply packet");
		return buf;
	case PING_INVALID_RESPONSE:
		snprintf(buf, sizeof(buf), "Invalid response");
		return buf;
	case PING_INVALID_ID:
		snprintf(buf, sizeof(buf), "Invalid ID in response");
		return buf;
	default:
		break;
	}

	snprintf(buf, sizeof(buf), "Unkown (%d)", rv);
	return buf;
}


#ifdef STANDALONE
int signaled = 0;

void
signal_handler(int sig)
{
	signaled = 1;
}

#include <signal.h>
#include <sys/time.h>

static inline void
__diff_tv(struct timeval *dest, struct timeval *start, struct timeval *end)
{
	dest->tv_sec = end->tv_sec - start->tv_sec;
	dest->tv_usec = end->tv_usec - start->tv_usec;

	if (dest->tv_usec < 0) {
		dest->tv_usec += 1000000;
		dest->tv_sec--;
	}
}


int
main(int argc, char **argv)
{
	struct timeval begin, end, delta;
	int timeout = 2, rv, sent = 0, received = 0;

	if (argc < 2) {
		printf("usage: %s <host> [timeout]\n", argv[0]);
		return 2;
	}

	if (argc == 3)
		timeout = atoi(argv[2]);

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	printf("Pinging %s\n", argv[1]);

	while (!signaled) {

		gettimeofday(&begin, NULL);
		rv = icmp_ping_host(argv[1], 1, timeout);
		++sent;
		gettimeofday(&end, NULL);
		__diff_tv(&delta, &begin, &end);

		switch(rv) {
		case PING_ERRNO:
			perror("icmp_ping_host");
			return 1;
			break;
		case PING_SUCCESS:
			++received;
			printf("Reply #%d RTT = %d.%06d seconds\n", received,
			       (int)delta.tv_sec, (int)delta.tv_usec );
			break;
		case PING_TIMEOUT:
			printf("%s timed out\n", argv[1]);
			break;
		case PING_HOST_UNREACH:
			printf("%s is unreachable\n", argv[1]);
			break;
		case PING_HOST_NOT_FOUND:
			printf("Host %s not found!\n", argv[1]);
			return 1;
			break;
		case PING_INVALID_CHECKSUM:
			printf("Invalid checksum in reply.\n");
			break;
		case PING_INVALID_SIZE:
			printf("Invalid size of reply packet.\n");
			break;
		case PING_INVALID_RESPONSE:
			printf("Invalid response.\n");
			break;
		case PING_INVALID_ID:
			printf("Invalid ID in response.\n");
			break;
		}

		sleep(1);
	}

	printf("%d sent; %d received\n", sent, received);

	if (sent > received) {
		return 1;
	}
	return 0;
}
#endif
