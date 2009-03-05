/*
 * Copyright (C) 2009, Neil Horman <nhorman@tuxdriver.com>
 * 
 * This program file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program in a file named COPYING; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 */

/*
 * Opens our netlink socket.  Returns the socket descriptor or < 0 on error
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <asm/types.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

#include "net_dropmon.h"

/*
 * This is just in place until the kernel changes get comitted
 */
#ifndef NETLINK_DRPMON
#define NETLINK_DRPMON 20
#endif


struct netlink_message {
	void *msg;
	struct nl_msg *nlbuf;
	int refcnt;
	int seq;
	void (*ack_cb)(struct netlink_message *amsg, struct netlink_message *msg, int err);
	struct netlink_message *next;
	struct netlink_message *prev;
};

struct netlink_message *head;

void handle_dm_alert_msg(struct netlink_message *msg, int err);
void handle_dm_config_msg(struct netlink_message *msg, int err);
void handle_dm_start_msg(struct netlink_message *amsg, struct netlink_message *msg, int err);
void handle_dm_stop_msg(struct netlink_message *amsg, struct netlink_message *msg, int err);


static void(*type_cb[_NET_DM_CMD_MAX])(struct netlink_message *, int err) = {
	NULL,
	handle_dm_alert_msg,
	handle_dm_config_msg,
	NULL,
	NULL
};

static struct nl_handle *nsd;
static int nsf;

enum {
	STATE_IDLE = 0,
	STATE_ACTIVATING,
	STATE_RECEIVING,
	STATE_RQST_DEACTIVATE,
	STATE_RQST_ACTIVATE,
	STATE_DEACTIVATING,
	STATE_FAILED,
	STATE_EXIT,
};

static int state = STATE_IDLE;


void sigint_handler(int signum)
{
	if ((state == STATE_RECEIVING) ||
	   (state == STATE_RQST_DEACTIVATE))
		state = STATE_RQST_DEACTIVATE;
	else
		printf("Got a sigint while not receiving\n");
	return;	
}

struct nl_handle *setup_netlink_socket()
{
	struct nl_handle *sd;
	int family;

	
	sd = nl_handle_alloc();

	genl_connect(sd);

	family = genl_ctrl_resolve(sd, "NET_DM");

	if (family < 0) {
		printf("Unable to find NET_DM family, dropwatch can't work\n");
		goto out_close;
	}

	nsf = family;

	nl_close(sd);
	nl_handle_destroy(sd);

	sd = nl_handle_alloc();
	nl_join_groups(sd, NET_DM_GRP_ALERT);

	nl_connect(sd, NETLINK_GENERIC);

	return sd;

out_close:
	nl_close(sd);
	nl_handle_destroy(sd);
	return NULL;

}

struct netlink_message *alloc_netlink_msg(uint32_t type, uint16_t flags, size_t size)
{
	struct netlink_message *msg;
	static uint32_t seq = 0;

	size += NLMSG_ALIGN(sizeof(struct netlink_message));
	size += sizeof(struct nlmsghdr);
	size = NLMSG_LENGTH(size);


	msg = (struct netlink_message *)malloc(sizeof(struct netlink_message));

	if (!msg)
		return NULL;

	msg->refcnt = 1;
	msg->nlbuf = nlmsg_alloc(); 
	msg->msg = genlmsg_put(msg->nlbuf, 0, seq, nsf, size, flags, type, 1);

	msg->ack_cb = NULL;
	msg->next = msg->prev = NULL;
	msg->seq = seq++;

	return msg;
}

void set_ack_cb(struct netlink_message *msg,
			void (*cb)(struct netlink_message *, struct netlink_message *, int))
{
	if (head == NULL)
		head = msg;
	else {
		msg->next = head;
		head = msg;
		msg->next->prev = msg;
	}

	msg->ack_cb = cb;
}

		
struct netlink_message *wrap_netlink_msg(struct nlmsghdr *buf)
{
	struct netlink_message *msg;

	msg = (struct netlink_message *)malloc(sizeof(struct netlink_message));
	if (msg) {
		msg->refcnt = 1;
		msg->msg = buf;
		msg->nlbuf = NULL;
	}

	return msg;
}

int free_netlink_msg(struct netlink_message *msg)
{
	int refcnt;

	msg->refcnt--;

	refcnt = msg->refcnt;

	if (!refcnt) {
		if (msg->nlbuf)
			nlmsg_free(msg->nlbuf);
		else
			free(msg->msg);
		free(msg);
	}

	return refcnt;
}

int send_netlink_message(struct netlink_message *msg)
{
	return nl_send(nsd, msg->nlbuf);
}

struct netlink_message *recv_netlink_message(int *err)
{
	static unsigned char *buf;
	struct netlink_message *msg;
	struct genlmsghdr *glm;
	struct sockaddr_nl nla;
	int type;
	int rc;

	*err = 0;
restart:
	printf("Trying to get a netlink msg\n");
	do {
		rc = nl_recv(nsd, &nla, &buf, NULL);
		printf("Got a netlink message\n");
		if (rc < 0) {	
			switch (errno) {
			case EINTR:
				/*
				 * Take a pass throught the state loop
				 */
				return NULL;
				break;
			default:
				perror("Receive operation failed:");
				return NULL;
				break;
			}
		}
	} while (rc == 0);

	msg = wrap_netlink_msg((struct nlmsghdr *)buf);

	type = ((struct nlmsghdr *)msg->msg)->nlmsg_type;

	/*
	 * Note the NLMSG_ERROR is overloaded
	 * Its also used to deliver ACKs
	 */
	if (type == NLMSG_ERROR) {
		struct netlink_message *am;
		struct nlmsgerr *errm = nlmsg_data(msg->msg);
		am = head;
		while (am != NULL) {
			if (am->seq == errm->msg.nlmsg_seq) {
				if (am->prev != NULL)
					am->prev->next = am->next;
				if (am->next != NULL)
					am->next->prev = am->prev;
				if ((am->next == NULL) && (am->prev == NULL))
					head = NULL;
				am->ack_cb(msg, am, errm->error);
				break;
			}
		}
		free_netlink_msg(am);
		free_netlink_msg(msg);
		return NULL;
	}

	glm = nlmsg_data(msg->msg);
	type = glm->cmd;
	
	if ((type > NET_DM_CMD_MAX) ||
	    (type <= NET_DM_CMD_UNSPEC)) {
		printf("Received message of unknown type %d\n", 
			type);
		free_netlink_msg(msg);
		return NULL;
	}

	return msg;	
}

void process_rx_message(void)
{
	struct netlink_message *msg;
	int err;
	int type;

	msg = recv_netlink_message(&err);

	if (msg) {
		struct nlmsghdr *nlh = msg->msg;
		struct genlmsghdr *glh = nlmsg_data(nlh);
		type  = glh->cmd; 
		type_cb[type](msg, err);
	}
	return;
}



/*
 * These are the received message handlers
 */
void handle_dm_alert_msg(struct netlink_message *msg, int err)
{
	int i;
	struct nlmsghdr *nlh = msg->msg;
	struct genlmsghdr *glh = nlmsg_data(nlh);

	struct net_dm_alert_msg *alert = genlmsg_data(glh);

	if (state != STATE_RECEIVING)
		goto out_free;

	printf("Got Drop notifications\n");


	for (i=0; i < alert->entries; i++) {
		void *location;
		memcpy(&location, alert->points[i].pc, sizeof(void *));
		printf ("%d drops at location %p\n", alert->points[i].count, location);
	}	

out_free:
	free_netlink_msg(msg);
}

void handle_dm_config_msg(struct netlink_message *msg, int err)
{
	printf("Got a config message\n");
}

void handle_dm_start_msg(struct netlink_message *amsg, struct netlink_message *msg, int err)
{
	if (err != 0) {
		char *erm = strerror(err*-1);
		printf("Failed activation request, error: %s\n", erm);
		state = STATE_FAILED;
		goto out;
	}
	
	if (state == STATE_ACTIVATING) {
		struct sigaction act;
		memset(&act, 0, sizeof(struct sigaction));
		act.sa_handler = sigint_handler;
		act.sa_flags = SA_RESETHAND;

		printf("Kernel monitoring activated.\n");
		printf("Issue Ctrl-C to stop monitoring\n");
		sigaction(SIGINT, &act, NULL);

		state = STATE_RECEIVING;
	} else {
		printf("Odd, the kernel told us that it activated and we didn't ask\n");
		state = STATE_FAILED;
	}
out:
	free_netlink_msg(msg);
	return;
}

void handle_dm_stop_msg(struct netlink_message *amsg, struct netlink_message *msg, int err)
{
	printf("Got a stop message\n");
	if (err == 0)
		state = STATE_IDLE;
	free_netlink_msg(msg);
}

int enable_drop_monitor()
{
	struct netlink_message *msg;

	msg = alloc_netlink_msg(NET_DM_CMD_START, NLM_F_REQUEST|NLM_F_ACK, 0);

	set_ack_cb(msg, handle_dm_start_msg);
	
	return send_netlink_message(msg);
}

int disable_drop_monitor()
{
	struct netlink_message *msg;

	msg = alloc_netlink_msg(NET_DM_CMD_STOP, NLM_F_REQUEST|NLM_F_ACK, 0);

	set_ack_cb(msg, handle_dm_stop_msg);

	return send_netlink_message(msg);
}

void enter_command_line_mode()
{
	char *input;

	do {
		input = readline("dropwatch> ");

		if (!strcmp(input,"start")) {
			state = STATE_RQST_ACTIVATE;
			break;
		}

		if (!strcmp(input, "stop")) {
			state = STATE_RQST_DEACTIVATE;
			break;
		}

		if (!strcmp(input, "exit")) {
			state = STATE_EXIT;
			break;
		}

		free(input);
	} while(1);
}

void enter_state_loop(void)
{

	int should_rx = 0;

	while (1) {
		switch(state) {

		case STATE_IDLE:
			should_rx = 0;
			enter_command_line_mode();
			break;
		case STATE_RQST_ACTIVATE:
			printf("Enabling monitoring...\n");
			if (enable_drop_monitor() < 0) {
				perror("Unable to send activation msg:");
				state = STATE_FAILED;
			} else {
				state = STATE_ACTIVATING;
				should_rx = 1;
			}
			
			break;

		case STATE_ACTIVATING:
			printf("Waiting for activation ack....\n");
			break;
		case STATE_RECEIVING:
			break;
		case STATE_RQST_DEACTIVATE:
			printf("Deactivation requested, turning off monitoring\n");
			if (disable_drop_monitor() < 0) {
				perror("Unable to send deactivation msg:");
				state = STATE_FAILED;
			} else
				state = STATE_DEACTIVATING;

			break;
		case STATE_DEACTIVATING:
			printf("Waiting for deactivation ack...\n");
			break;

		case STATE_EXIT:
		case STATE_FAILED:
			should_rx = 0;
			return;
		default:
			printf("Unknown state received!  exiting!\n");
			state = STATE_FAILED;
			should_rx = 0;
			break;
		}

		/*
		 * After we process our state loop, look to see if we have messages
		 */
		if (should_rx)
			process_rx_message();
	}
}

int main (int argc, char **argv)
{

	/*
	 * open up the netlink socket that we need to talk to our dropwatch socket
	 */
	nsd = setup_netlink_socket();

	if (nsd == NULL) {
		printf("Cleaning up on socket creation error\n");
		goto out;
	}


	enter_state_loop();
	printf("Shutting down ...\n");
done:
	close(nsd);
	exit(0);
out:
	exit(1);
}