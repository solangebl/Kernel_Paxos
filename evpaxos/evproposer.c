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

#include "eth.h"
#include "evpaxos.h"
#include "message.h"
#include "peers.h"
#include "proposer.h"
#include <linux/slab.h>

struct evproposer
{
  int               id;
  int               preexec_window;
  struct proposer*  state;
  struct peers*     peers;
  struct timer_list stats_ev;
  struct timeval    stats_interval;
};

static void
peer_send_prepare(struct net_device* dev, struct peer* p, void* arg)
{
  send_paxos_prepare(dev, get_addr(p), arg);
}

static void
peer_send_accept(struct net_device* dev, struct peer* p, void* arg)
{
  send_paxos_accept(dev, get_addr(p), arg);
}

static void
proposer_preexecute(struct evproposer* p)
{
  int           i;
  paxos_prepare pr;
  int           count = p->preexec_window - proposer_prepared_count(p->state);

  paxos_log_debug("Proposer: Preexec %d - prepared count %d", p->preexec_window,
                  proposer_prepared_count(p->state));

  if (count <= 0)
    return;
  for (i = 0; i < count; i++) {
    proposer_prepare(p->state, &pr);
    peers_foreach_acceptor(p->peers, peer_send_prepare, &pr);
  }
  paxos_log_debug("Proposer: Opened %d new instances", count);
}

static void
try_accept(struct evproposer* p)
{
  paxos_accept accept;
  int          i = 0;
  while (proposer_accept(p->state, &accept)) {
    i = 1;
    peers_foreach_acceptor(p->peers, peer_send_accept, &accept);
  }
  if (i == 1) {
    paxos_log_debug("Proposer: Sending accept to all acceptors");
  }
  proposer_preexecute(p);
}

static void
evproposer_handle_promise(paxos_message* msg, void* arg, eth_address* src)
{
  paxos_log_debug("Proposer: received PROMISE");
  struct evproposer* proposer = arg;
  paxos_prepare      prepare;
  paxos_promise*     pro = &msg->u.promise;
  int preempted = proposer_receive_promise(proposer->state, pro, &prepare);
  if (preempted)
    peers_foreach_acceptor(proposer->peers, peer_send_prepare, &prepare);
  try_accept(proposer);
}

static void
evproposer_handle_accepted(paxos_message* msg, void* arg, eth_address* src)
{
  struct evproposer* proposer = arg;
  paxos_accepted*    acc = &msg->u.accepted;
  if (proposer_receive_accepted(proposer->state, acc)) {
    paxos_log_debug("Proposer: received ACCEPTED");
    try_accept(proposer);
  }
}

static void
evproposer_handle_preempted(paxos_message* msg, void* arg, eth_address* src)
{
  paxos_log_debug(KERN_INFO "Proposer: received PREEMPTED");
  struct evproposer* proposer = arg;
  paxos_prepare      prepare;
  int                preempted =
    proposer_receive_preempted(proposer->state, &msg->u.preempted, &prepare);
  if (preempted) {
    peers_foreach_acceptor(proposer->peers, peer_send_prepare, &prepare);
    try_accept(proposer);
  }
}

static void
evproposer_handle_client_value(paxos_message* msg, void* arg, eth_address* src)
{
  struct evproposer*         proposer = arg;
  struct paxos_client_value* v = &msg->u.client_value;

  proposer_propose(proposer->state, v->value.paxos_value_val,
                   v->value.paxos_value_len);
  paxos_log_debug("Proposer: received a CLIENT VALUE");
  try_accept(proposer);
}

static void
evproposer_handle_acceptor_state(paxos_message* msg, void* arg,
                                 eth_address* src)
{
  paxos_log_debug("Proposer: received a ACCEPTOR STATE");
  struct evproposer*           proposer = arg;
  struct paxos_acceptor_state* acc_state = &msg->u.state;
  proposer_receive_acceptor_state(proposer->state, acc_state);
}

static void
evproposer_preexec_once(struct evproposer* arg)
{
  if (arg)
    proposer_preexecute(arg);
}

static void
evproposer_check_timeouts(unsigned long arg)
{
  paxos_log_debug("Proposer: evproposer_check_timeouts");

  struct evproposer*       p = (struct evproposer*)arg;
  struct timeout_iterator* iter = proposer_timeout_iterator(p->state);
  paxos_log_debug("Proposer: Instances timed out in phase 1 or 2.");

  paxos_prepare pr;
  while (timeout_iterator_prepare(iter, &pr)) {
    paxos_log_debug("Proposer: Instance %d timed out in phase 1.", pr.iid);
    peers_foreach_acceptor(p->peers, peer_send_prepare, &pr);
  }

  paxos_accept ar;
  while (timeout_iterator_accept(iter, &ar)) {
    paxos_log_debug("Proposer: Instance %d timed out in phase 2.", ar.iid);
    peers_foreach_acceptor(p->peers, peer_send_accept, &ar);
  }

  timeout_iterator_free(iter);
  mod_timer(&p->stats_ev, jiffies + timeval_to_jiffies(&p->stats_interval));
}

struct evproposer*
evproposer_init_internal(int id, struct evpaxos_config* c, struct peers* peers)
{

  struct evproposer* proposer;
  int                acceptor_count;
  acceptor_count = evpaxos_acceptor_count(c);

  proposer = pmalloc(sizeof(struct evproposer));
  if (proposer == NULL)
    return NULL;
  memset(proposer, 0, sizeof(struct evproposer));

  proposer->id = id;
  proposer->preexec_window = paxos_config.proposer_preexec_window;
  proposer->state = proposer_new(proposer->id, acceptor_count);
  proposer->peers = peers;

  peers_subscribe(peers, PAXOS_PROMISE, evproposer_handle_promise, proposer);
  peers_subscribe(peers, PAXOS_ACCEPTED, evproposer_handle_accepted, proposer);
  peers_subscribe(peers, PAXOS_PREEMPTED, evproposer_handle_preempted,
                  proposer);
  peers_subscribe(peers, PAXOS_CLIENT_VALUE, evproposer_handle_client_value,
                  proposer);
  peers_subscribe(peers, PAXOS_ACCEPTOR_STATE, evproposer_handle_acceptor_state,
                  proposer);

  evproposer_preexec_once(proposer);
  setup_timer(&proposer->stats_ev, evproposer_check_timeouts,
              (unsigned long)proposer);
  proposer->stats_interval =
    (struct timeval){ paxos_config.proposer_timeout, 0 };
  mod_timer(&proposer->stats_ev,
            jiffies + timeval_to_jiffies(&proposer->stats_interval));

  return proposer;
}

struct evproposer*
evproposer_init(int id, char* if_name, char* path)
{
  struct evpaxos_config* config = evpaxos_config_read(path);
  if (config == NULL)
    return NULL;

  if (id < 0 || id >= evpaxos_proposer_count(config)) {
    printk(KERN_ERR "Invalid proposer id: %d", id);
    return NULL;
  }

  struct peers* peers = peers_new(config, id, if_name);
  if (peers == NULL)
    return NULL;

  add_acceptors_from_config(peers, config);
  printall(peers, "Proposer");
  struct evproposer* p = evproposer_init_internal(id, config, peers);
  evpaxos_config_free(config);
  return p;
}

void
evproposer_free_internal(struct evproposer* p)
{
  del_timer(&p->stats_ev);
  proposer_free(p->state);
  pfree(p);
}

void
evproposer_free(struct evproposer* p)
{
  printall(p->peers, "PROPOSER");
  peers_free(p->peers);
  evproposer_free_internal(p);
}

void
evproposer_set_instance_id(struct evproposer* p, unsigned iid)
{
  proposer_set_instance_id(p->state, iid);
}
