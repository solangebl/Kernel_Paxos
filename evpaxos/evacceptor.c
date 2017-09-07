/*
 * Copyright (c) 2013-2014, University of Lugano
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holders nor the names of it
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "evpaxos.h"
#include "acceptor.h"
#include "message.h"
#include "peers.h"
#include <linux/slab.h>
#include <linux/udp.h>

struct evacceptor
{
	struct peers* peers;
	struct acceptor* state;
	struct udp_service * k;
};

struct evacceptor * accep = NULL;

void paxos_acceptor_listen(udp_service * k, struct evacceptor * ev){
	peers_listen(ev->peers, k);
}

static void send_acceptor_paxos_message(struct socket * s, struct sockaddr_in * bev, void* msg){
	send_paxos_message(s, bev, msg, "Acceptor:");
}

static void
peer_send_paxos_message(struct peer* p, void* arg)
{
	send_acceptor_paxos_message(get_send_socket(p), get_sockaddr(p), arg);
}

/*
	Received a prepare request (phase 1a).
*/
static void
evacceptor_handle_prepare(struct peer* p, paxos_message* msg, void* arg)
{
	paxos_message out;
	paxos_prepare* prepare = &msg->u.prepare;
	struct evacceptor* a = (struct evacceptor*)arg;
	paxos_log_debug("Acceptor: Received PREPARE");
	if (acceptor_receive_prepare(a->state, prepare, &out) != 0) {
		send_acceptor_paxos_message( get_send_socket(p),get_sockaddr(p), &out);
		paxos_message_destroy(&out);
		paxos_log_info("Acceptor: sent promise for iid %d", prepare->iid);
	}
}

/*
	Received a accept request (phase 2a).
*/
static void
evacceptor_handle_accept(struct peer* p, paxos_message* msg, void* arg)
{
	paxos_message out;
	paxos_accept* accept = &msg->u.accept;
	struct evacceptor* a = (struct evacceptor*)arg;
	paxos_log_info("Acceptor: Received ACCEPT REQUEST");
	if (acceptor_receive_accept(a->state, accept, &out) != 0) {
		if (out.type == PAXOS_ACCEPTED) {
			paxos_log_debug("Acceptor: Sent ACCEPTED to all proposers and learners");
			peers_foreach_client(a->peers, peer_send_paxos_message, &out);
		} else if (out.type == PAXOS_PREEMPTED) {
			paxos_log_debug("Acceptor: Sent PREEMPTED to all proposers ");
			send_acceptor_paxos_message(get_send_socket(p), get_sockaddr(p), &out);
		}
		paxos_message_destroy(&out);
	}
}

static void
evacceptor_handle_repeat(struct peer* p, paxos_message* msg, void* arg)
{
	iid_t iid;
	paxos_accepted accepted;
	paxos_repeat* repeat = &msg->u.repeat;
	struct evacceptor* a = (struct evacceptor*)arg;
	paxos_log_info("Acceptor: Handle repeat for iids %d-%d", repeat->from, repeat->to);
	for (iid = repeat->from; iid <= repeat->to; ++iid) {
		if (acceptor_receive_repeat(a->state, iid, &accepted)) {
			paxos_log_debug("Acceptor: sent a repeated PAXOS_ACCEPTED %d to learner", iid);
			send_paxos_accepted(get_send_socket(p), get_sockaddr(p), &accepted);
			paxos_accepted_destroy(&accepted);
		}
	}
}

static void
evacceptor_handle_trim(struct peer* p, paxos_message* msg, void* arg)
{
	paxos_log_debug("Acceptor: Received PAXOS_TRIM. Deleting the old instances");
	paxos_trim* trim = &msg->u.trim;
	struct evacceptor* a = (struct evacceptor*)arg;
	acceptor_receive_trim(a->state, trim);
}

static void
evacceptor_handle_hi(struct peer* p, paxos_message* msg, void* arg)
{
	paxos_log_info("Acceptor: Received PAXOS_LEARNER_HI. Sending OK");
	send_paxos_acceptor_ok(get_send_socket(p), get_sockaddr(p), NULL);
}

static void
evacceptor_handle_del(struct peer* p, paxos_message* msg, void* arg)
{
	paxos_log_info("Acceptor: Received PAXOS_LEARNER_DEL.");
	peers_delete_learner(p);
}

static void
send_acceptor_state(unsigned long arg)
{
	paxos_log_debug("Acceptor: send_acceptor_state");
	struct evacceptor* a = (struct evacceptor*)arg;
	paxos_message msg = {.type = PAXOS_ACCEPTOR_STATE};
	acceptor_set_current_state(a->state, &msg.u.state);
	peers_foreach_client(a->peers, peer_send_paxos_message, &msg);
}


struct evacceptor*
evacceptor_init_internal(int id, struct evpaxos_config* c, struct peers* p, udp_service * k)
{
	struct evacceptor* acceptor;

	acceptor = kmalloc(sizeof(struct evacceptor), GFP_ATOMIC | __GFP_REPEAT);
	memset(acceptor, 0, sizeof(struct evacceptor));
	acceptor->state = acceptor_new(id);
	acceptor->peers = p;
	acceptor->k = k;
	accep = acceptor;

	peers_subscribe(p, PAXOS_PREPARE, evacceptor_handle_prepare, acceptor);
	peers_subscribe(p, PAXOS_ACCEPT, evacceptor_handle_accept, acceptor);
	peers_subscribe(p, PAXOS_REPEAT, evacceptor_handle_repeat, acceptor);
	peers_subscribe(p, PAXOS_TRIM, evacceptor_handle_trim, acceptor);
	peers_subscribe(p, PAXOS_LEARNER_HI, evacceptor_handle_hi, acceptor);
	peers_subscribe(p, PAXOS_LEARNER_DEL, evacceptor_handle_del, acceptor);

	k->timer_cb[ACC_TIM] = send_acceptor_state;
	k->data[ACC_TIM] = (unsigned long) acceptor;
	k->timeout_jiffies[ACC_TIM] = msecs_to_jiffies(1000);
	return acceptor;
}

struct evacceptor*
evacceptor_init(int id, udp_service * k)
{
	struct evpaxos_config* config = evpaxos_config_read();
	if (config  == NULL)
		return NULL;

	int acceptor_count = evpaxos_acceptor_count(config);
	if (id < 0 || id >= acceptor_count) {
		paxos_log_error("Acceptor: Invalid acceptor id: %d.", id);
		paxos_log_error("Acceptor: Should be between 0 and %d", acceptor_count);
		evpaxos_config_free(config);
		return NULL;
	}
	struct sockaddr_in send_add = evpaxos_acceptor_address(config,id);
	struct peers* peers = peers_new(&send_add, config, id);
	if(peers_sock_init(peers,k) >= 0){
		struct evacceptor* acceptor = evacceptor_init_internal(id, config, peers, k);
		evpaxos_config_free(config);
		return acceptor;
	}
	evpaxos_config_free(config);
	return NULL;
}

void
evacceptor_free_internal(struct evacceptor* a)
{
	acceptor_free(a->state);
	kfree(a);
}

void
evacceptor_free(struct evacceptor* a)
{
	printall(a->peers, "ACCEPTOR");
	peers_free(a->peers);
	evacceptor_free_internal(a);
}
