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

#include "peers.h"
#include "message.h"
#include <linux/errno.h>

#include "eth.h"

#include <linux/inet.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/types.h>

struct peer
{
  int           id;
  eth_address   addr[ETH_ALEN];
  struct peers* peers;
};

struct peers
{
  int                peers_count, clients_count;
  struct peer**      peers;   /* peers we connected to */
  struct peer**      clients; /* peers we accepted connections from */
  struct net_device* dev;
};

int* peers_received_ok = NULL;
int  ok_received;

static struct peer* make_peer(struct peers* p, int id, eth_address* in);
static void         free_peer(struct peer* p);
static void         free_all_peers(struct peer** p, int count);

static void
check_id(struct peers* p, struct evpaxos_config* config, int id)
{
  paxos_log_debug("check Id %d\n", id);
  if (id < 0)
    return;

  eth_address* ad1 = evpaxos_acceptor_address(config, id);
  eth_address* ad2 = evpaxos_proposer_address(config, id);
  if (ad1 != NULL) {
    paxos_log_debug(
      "Comparing "
      "\n%02x:%02x:%02x:%02x:%02x:%02x\n%02x:%02x:%02x:%02x:%02x:%02x\n",
      p->dev->dev_addr[0], p->dev->dev_addr[1], p->dev->dev_addr[2],
      p->dev->dev_addr[3], p->dev->dev_addr[4], p->dev->dev_addr[5], ad1[0],
      ad1[1], ad1[2], ad1[3], ad1[4], ad1[5]);
    if (memcmp(p->dev->dev_addr, ad1, ETH_ALEN) == 0)
      return;
  }

  if (ad2 != NULL) {
    paxos_log_debug(
      "Comparing "
      "\n%02x:%02x:%02x:%02x:%02x:%02x\n%02x:%02x:%02x:%02x:%02x:%02x\n",
      p->dev->dev_addr[0], p->dev->dev_addr[1], p->dev->dev_addr[2],
      p->dev->dev_addr[3], p->dev->dev_addr[4], p->dev->dev_addr[5], ad2[0],
      ad2[1], ad2[2], ad2[3], ad2[4], ad2[5]);
    if (memcmp(p->dev->dev_addr, ad2, ETH_ALEN) == 0)
      return;
  }

  paxos_log_error("Warning: address %02x:%02x:%02x:%02x:%02x:%02x is not "
                  "present in the config file",
                  p->dev->dev_addr[0], p->dev->dev_addr[1], p->dev->dev_addr[2],
                  p->dev->dev_addr[3], p->dev->dev_addr[4],
                  p->dev->dev_addr[5]);
}

struct peers*
peers_new(struct evpaxos_config* config, int id, char* if_name)
{
  struct peers* p = pmalloc(sizeof(struct peers));
  p->peers_count = 0;
  p->clients_count = 0;
  p->peers = NULL;
  p->clients = NULL;

  p->dev = eth_init(if_name);
  if (p->dev == NULL) {
    printk(KERN_ERR "Interface not found: %s\n", if_name);
    pfree(p);
    return NULL;
  }
  check_id(p, config, id);
  return p;
}

void
peers_free(struct peers* p)
{
  eth_destroy(p->dev);
  free_all_peers(p->peers, p->peers_count);
  free_all_peers(p->clients, p->clients_count);
  pfree(p);
}

int
peers_count(struct peers* p)
{
  return p->peers_count;
}

eth_address*
get_addr(struct peer* p)
{
  return p->addr;
}

void
peers_foreach_acceptor(struct peers* p, peer_iter_cb cb, void* arg)
{
  int i;
  for (i = 0; i < p->peers_count; ++i) {
    cb(p->dev, p->peers[i], arg);
  }
}

void
peers_foreach_client(struct peers* p, peer_iter_cb cb, void* arg)
{
  int i;
  for (i = 0; i < p->clients_count; ++i)
    cb(p->dev, p->clients[i], arg);
}

struct peer*
peers_get_acceptor(struct peers* p, int id)
{
  int i;
  for (i = 0; p->peers_count; ++i)
    if (p->peers[i]->id == id)
      return p->peers[i];
  return NULL;
}

// add all acceptors in the peers
void
add_acceptors_from_config(struct peers* p, struct evpaxos_config* conf)
{
  eth_address* addr;
  int          n = evpaxos_acceptor_count(conf);
  p->peers = prealloc(p->peers, sizeof(struct peer*) * n);

  for (int i = 0; i < n; i++) {
    addr = evpaxos_acceptor_address(conf, i);
    if (addr)
      p->peers[p->peers_count++] = make_peer(p, i, addr);
  }
  peers_received_ok = pmalloc(sizeof(int) * p->peers_count);
  memset(peers_received_ok, 0, sizeof(int) * p->peers_count);
}

void
printall(struct peers* p, char* name)
{
  paxos_log_info("%s", name);
  paxos_log_info("\tME address %02x:%02x:%02x:%02x:%02x:%02x",
                 p->dev->dev_addr[0], p->dev->dev_addr[1], p->dev->dev_addr[2],
                 p->dev->dev_addr[3], p->dev->dev_addr[4], p->dev->dev_addr[5]);
  paxos_log_info("PEERS we connect to");
  int i;
  for (i = 0; i < p->peers_count; i++) {
    paxos_log_info("\tid=%d address "
                   "%02x:%02x:%02x:%02x:%02x:%02x",
                   p->peers[i]->id, p->peers[i]->addr[0], p->peers[i]->addr[1],
                   p->peers[i]->addr[2], p->peers[i]->addr[3],
                   p->peers[i]->addr[4], p->peers[i]->addr[5]);
  }

  paxos_log_info("CLIENTS we receive connections ");
  paxos_log_info("(will be updated as message are received");

  for (i = 0; i < p->clients_count; i++) {
    paxos_log_info(
      "\tid=%d address %02x:%02x:%02x:%02x:%02x:%02x", p->clients[i]->id,
      p->clients[i]->addr[0], p->clients[i]->addr[1], p->clients[i]->addr[2],
      p->clients[i]->addr[3], p->clients[i]->addr[4], p->clients[i]->addr[5]);
  }
  paxos_log_info("");
}

int
peer_get_id(struct peer* p)
{
  return p->id;
}

// 0 if known, 1 if new
int
add_or_update_client(eth_address* addr, struct peers* p)
{
  int i;
  for (i = 0; i < p->clients_count; ++i) {
    if (memcmp(addr, p->clients[i]->addr, ETH_ALEN) == 0) {
      return 0;
    }
  }

  paxos_log_info("Added a new client, now %d clients", p->clients_count + 1);
  p->clients =
    prealloc(p->clients, sizeof(struct peer) * (p->clients_count + 1));
  p->clients[p->clients_count] = make_peer(p, p->clients_count, addr);
  p->clients_count++;
  return 1;
}

void
peer_send_del(struct net_device* dev, struct peer* p, void* arg)
{
  send_paxos_learner_del(dev, get_addr(p));
}

struct net_device*
get_dev(struct peers* p)
{
  return p->dev;
}

int
peers_missing_ok(struct peers* p)
{
  return (ok_received != p->peers_count);
}

void
peers_update_ok(struct peers* p, eth_address* addr)
{
  int i;
  for (i = 0; i < p->peers_count; ++i) {
    if (memcmp(addr, p->peers[i]->addr, ETH_ALEN) == 0 &&
        peers_received_ok[p->peers[i]->id] == 0) {
      paxos_log_debug("peers_received_ok[%d] = 1", p->peers[i]->id);
      peers_received_ok[p->peers[i]->id] = 1;
      ok_received++;
      break;
    }
  }
}

void
peers_delete_learner(struct peers* p, eth_address* addr)
{
  int i, j;
  for (i = 0; i < p->clients_count; ++i) {
    if (memcmp(addr, p->clients[i]->addr, ETH_ALEN) == 0) {
      pfree(p->clients[i]);
      for (j = i; j < p->clients_count - 1; ++j) {
        p->clients[j] = p->clients[j + 1];
        p->clients[j]->id = j;
      }
      p->clients_count--;
      p->clients =
        prealloc(p->clients, sizeof(struct peer*) * (p->clients_count));
      break;
    }
  }
}

void
peers_subscribe(struct peers* p)
{
  eth_listen(p->dev);
}

void
peers_add_subscription(struct peers* p, paxos_message_type type, peer_cb cb,
                       void* arg)
{
  eth_subscribe(p->dev, (uint16_t)type, cb, arg);
}

static struct peer*
make_peer(struct peers* peers, int id, eth_address* addr)
{
  struct peer* p = pmalloc(sizeof(struct peer));
  p->id = id;
  memcpy(p->addr, addr, ETH_ALEN);
  p->peers = peers;
  return p;
}

static void
free_all_peers(struct peer** p, int count)
{
  int i;
  for (i = 0; i < count; i++)
    free_peer(p[i]);
  if (count > 0)
    pfree(p);
}

static void
free_peer(struct peer* p)
{
  pfree(p);
}
