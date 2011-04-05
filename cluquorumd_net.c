/*
  Copyright Red Hat, Inc. 2002-2004

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
 * Quorum Daemon Net-Tiebreaker Thread + Functions.  Easy to remove
 * from the quorum daemon. This was originally part of cluquorumd.c,
 * but timing problems therein forced me to make pinging the tiebreaker
 * asynchronous from the quorum daemon (as of 1.2.17)
 */
 
#include <string.h>
#include <pthread.h>
#include <syslog.h>
#include <net_tie.h>
#include <ping.h>


#define LOG(lvl, fmt, args...) do{ syslog(lvl, fmt, ##args); printf(fmt, ##args); } while(0)


static int ping_interval = 2000000; /* In microseconds */
static int declare_online = 1;
static int declare_offline = 1;
static pthread_t net_thread = (pthread_t)0;
static int net_vote_alive = 0;
static char *tb_ip = NULL;
static pthread_rwlock_t net_lock = PTHREAD_RWLOCK_INITIALIZER;
static int totem_timeout = 0;


/**
  Clean up local variables
 */
static void
net_cleanup(void)
{
	pthread_rwlock_wrlock(&net_lock);
	net_vote_alive = 0;
	if (tb_ip) {
		free(tb_ip);
		tb_ip = NULL;
	}
	net_thread = (pthread_t)0;
	pthread_rwlock_unlock(&net_lock);
}


/**
  Net tiebreaker thread.

  @param arg		Unused.
  @return		NULL
 */
void *
net_quorum_thread(void *arg)
{
	int restart;
	char alive, was_alive, hits = 0, misses = 0, _online, _offline;
	int interval, ping_ret, errno_save;
	char target[64];

	while (1) {
		alive = 0;
		restart = 0;

		pthread_rwlock_rdlock(&net_lock);
		was_alive = net_vote_alive;
		if (!tb_ip) {
			pthread_rwlock_unlock(&net_lock);
			break;
		}
		strncpy(target, tb_ip, sizeof(target));

		interval = ping_interval;
		_online = declare_online;
		_offline = declare_offline;

		pthread_rwlock_unlock(&net_lock);

		ping_ret = icmp_ping_host(target, 0, 1);
	        if (ping_ret == 0) {
			/*
			 * If we ping successfully, misses must
			 * be reset.  We must miss _offline 
			 * *consecutive* pings to declare the
			 * IP tiebreaker offline
			 */
			misses = 0;
			alive = 1;
		} else {
			/*
			 * Save errno for later because pthread_rwlock_*
			 * may alter it.
			 */
			errno_save = errno;
			/*
			 * Sorry, must have _online consecutive
			 * hits to be declared online if currently down.
			 * Otherwise, this line is meaningless.
			 */
			hits = 0;
		}

		pthread_rwlock_rdlock(&net_lock);
		if (strcmp(tb_ip, target)) {
			/* Tie breaker changed during ping; restart */
			restart = 1;
		}
		pthread_rwlock_unlock(&net_lock);

		/*
		   If tiebreaker IP changed, start from the top and 
		   try pinging the new one before updating status
		 */
		if (restart)
			continue;

		if (was_alive && !alive) {
			if (++misses < _offline) {
				alive = was_alive;

				/*
				 * pthread_rwlock_* are not guaranteed to
				 * leave errno unmodified, so set back to
				 * our saved value before reporting the
				 * error.
				 */
				errno = errno_save;

				/* Whine if we miss a ping */
				LOG(LOG_DEBUG, "IPv4 TB: Missed ping "
				       "(%d/%d); %s\n", misses, _offline,
				       icmp_ping_strerror(ping_ret));
			} else {
				LOG(LOG_NOTICE, "IPv4 TB @ %s Offline\n",
				       target);
			}
		} else if (!was_alive && alive) {
			if (++hits < _online) {
				alive = was_alive;
				misses = 0;
			} else {
				LOG(LOG_NOTICE, "IPv4 TB @ %s Online\n",
				       target);
			}
		}
		
		pthread_rwlock_wrlock(&net_lock);
		net_vote_alive = alive;
		pthread_rwlock_unlock(&net_lock);

		usleep(interval);
	}
	net_cleanup();

	printf("Exiting\n");

	return NULL;
}


static int
get_interval_tko(int fo_time, int _interval)
{
	int _tko;
	int up_time, down_time;
	char *val = NULL;
	int ccsfd;

	if (fo_time < 2000000) {
		LOG(LOG_ERR, "IPv4-TB: Failover time too fast for "
		       "IP-based tiebreaker.\n");
		return -1;
	}

	_tko = fo_time / _interval;

	/* IP declare-up time must *EXCEED* failover time */
	up_time = fo_time + (3 * _interval);

	/* Death/dead time for IP tiebreakers must be *less* than failover
	 * time, leaving enough space ping lag */
	down_time = _interval * (((_tko&~1)-1) / 2);

	/* Slow down the ping rate slightly */
	_interval = (_interval<<2)/3;

	/* Get our base TKOs for up / down */
	up_time /= _interval;
	down_time /= _interval;

	pthread_rwlock_wrlock(&net_lock);
	ping_interval = _interval;

	/* Ensure we exceed membership f/o speed for declaring online */
	declare_online = up_time;

	/* Way less for declaring offline. */
	declare_offline = down_time;
	pthread_rwlock_unlock(&net_lock);

	LOG(LOG_INFO, "IPv4-TB: Interval %d microseconds, On:%d Off:%d\n",
	       _interval, up_time, down_time);

	return 0;
}


/**
  Store the tiebreaker IP in our local copy.

  @param target		New tiebreaker IP address
 */
int
net_tiebreaker_init(char *target, int token, int interval)
{
	errno = EINVAL;

	if (!target)
		return -1;

	if (get_interval_tko(token, interval) < 0)
		return -1;

	pthread_rwlock_wrlock(&net_lock);
	if (tb_ip && strcmp(tb_ip, target))
		free(tb_ip);
	tb_ip = strdup(target);
	pthread_rwlock_unlock(&net_lock);

	errno = 0;
	return;
}
	

/**
  Provide the status of the net tiebreaker IP to the quorum daemon.

  @return		0 if the IP responded, 1 if not
 */
int
net_tiebreaker(void)
{
	int ret = 0;
	
	pthread_rwlock_rdlock(&net_lock);
	ret = net_vote_alive;
	pthread_rwlock_unlock(&net_lock);
	return ret;
}


/**
  Cancel the net tiebreaker thread
 */
int
net_cancel_quorum_thread(void)
{
	pthread_rwlock_rdlock(&net_lock);
	if (net_thread == (pthread_t)0) {
		pthread_rwlock_unlock(&net_lock);
		return 0;
	}
	pthread_rwlock_unlock(&net_lock);
		
	pthread_cancel(net_thread);
	net_cleanup();
	return 0;
}


/**
  Spawn the net tiebreaker thread.  Must have already called 
  net_tiebreaker_init at least once, or the thread will exit quickly.

  @return		Values returned by pthread_create.
 */
int
net_create_quorum_thread(pthread_t * thread)
{
	int ret;
	pthread_attr_t attrs;

	pthread_attr_init(&attrs);
	pthread_attr_setinheritsched(&attrs, PTHREAD_INHERIT_SCHED);
	pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attrs, 65536);
	pthread_atfork(NULL, NULL, NULL);

	ret = pthread_create(&net_thread, &attrs, net_quorum_thread, NULL);
	if (thread)
		*thread = net_thread;

	return ret;
}
