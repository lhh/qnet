#ifndef _NET_TIE_H
#define _NET_TIE_H

#define TOTEM_TOKEN_DEFAULT 10000

/* from cluquorumd_NET.c */
int net_create_quorum_thread(pthread_t * thread);
int net_cancel_quorum_thread(void);
int net_tiebreaker_init(char *tiebreaker_ip, int totem, int interval);
int net_tiebreaker(void);

#endif
