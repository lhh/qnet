#include <stdio.h>
#include <pthread.h>
#include <net_tie.h>
#include <syslog.h>
#include <libcman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define DEFAULT_TOKEN 10000
#define DEFAULT_INTERVAL 1000
#define MIN_TOKEN 5000		/* Minimum token timeout (milliseconds) */
#define MIN_INTERVAL 250	/* Ping interval minimum (milliseconds) */


static int allow_soft = 0;
static int running = 1;


void
usage(char *name, int retval)
{
	printf("usage: %s -a <host> [options]\n", name);
	printf(" -s       Make one node + IP tiebreaker sufficient to \n");
	printf("          form a quorum (DANGEROUS)\n");
	printf(" -f       Do not fork\n");
	printf(" -i <x>   Starting ping interval hint (milliseconds)\n");
	printf(" -t <x>   Token timeout (milliseconds)\n");
	exit(retval);
}


void
sigusr1_handler(int sig)
{
	allow_soft = !!allow_soft;
}


void
exit_handler(int sig)
{
	running = 0;
}


int
node_count(cman_handle_t ch)
{
	cman_node_t *cman_nodes = NULL;
	int x = 0, retnodes, ret = 0;

	x = cman_get_node_count(ch);
	if (x <= 0)
		return 0;

	cman_nodes = malloc(sizeof(cman_node_t) * x);
	if (!cman_nodes)
		return 0;

	if (cman_get_nodes(ch, x, &retnodes, cman_nodes) < 0) {
		free(cman_nodes);
		return 0;
	}

	for (x = 0; x < retnodes; x++) {
		if (cman_nodes[x].cn_member)
			++retnodes;
	}

	free(cman_nodes);
	return retnodes;
}


int
main(int argc, char **argv)
{
	char *ip_addr = NULL;
	int op;
	int allow_soft = 0, quorum = 0, count = 0, have_net;
	int x, token = DEFAULT_TOKEN, interval = DEFAULT_INTERVAL, errors = 0;
	pthread_t thread;
	cman_handle_t ch;

	while ((op = getopt(argc, argv, "a:t:i:sfh?")) != EOF) {
		switch(op) {
		case 'a':
			ip_addr = strdup(optarg);
			break;
		case 't':
			token = atoi(optarg);
			if (token < MIN_TOKEN) {
				printf("Token value must be at least %dms",
				       MIN_TOKEN);
				errors++;
			}
			break;
		case 'i':
			interval = atoi(optarg);
			if (interval < MIN_INTERVAL) {
				printf("Ping interval must be at "
				       "least %dms\n", MIN_INTERVAL);
				errors++;
			}
			break;
		case 's':
			allow_soft = 1;
			break;
		case '?':
		case 'h':
			usage(argv[0], !!errors);
		}
	}

	if (!ip_addr)
		++errors;
	if (errors)
		usage(argv[0], 1);

	if (geteuid() != 0) {
		printf("You are not root.\n");
		return 1;
	}

	do {
		ch = cman_admin_init(NULL);
		if (!ch)
			sleep(1);
	} while (!ch);

	signal(SIGINT, exit_handler);
	signal(SIGQUIT, exit_handler);
	signal(SIGTERM, exit_handler);
	signal(SIGUSR1, sigusr1_handler);

	net_tiebreaker_init(ip_addr, token * 1000, interval * 1000);
	net_create_quorum_thread(&thread);
	if (cman_register_quorum_device(ch, "QNet", 1) < 0) {
		printf("CMAN registration failed...!?\n");
		exit(1);
	}

	while (running) {
		usleep(interval*1000);
		quorum = cman_is_quorate(ch);
		count = node_count(ch);
		have_net = net_tiebreaker();

		if (!quorum) {
			if (have_net && count == 1 && allow_soft) {
				quorum = 1;
			}
		} else {
			if (!have_net && count == 1) {
				quorum = 0;
				/* take some action for loss of quorum */
			}
		}

		cman_poll_quorum_device(ch, quorum);
	}

	cman_unregister_quorum_device(ch);
	cman_finish(ch);
	net_cancel_quorum_thread();

	return 0;
}
