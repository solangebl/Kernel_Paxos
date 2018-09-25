/*
 * Copyright (c) 2018-2019, University of Lugano
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

#include "common.h"
#include "acceptor_common.h"
#include "storage.h"
#include "storage_device.h"
#include <linux/if_ether.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>


static const int MAX_SIZE = 1000000;

static struct disk_storage*
disk_storage_new(int acceptor_id){

  // register char device
  int register_device = device_init();

  if(register_device==0){
    printk("Device inited");  
  }

  // REMOVE - I keep the old code so everything keeps working in the meantime
  printk("Calling disk new method");
  struct disk_storage* s = vmalloc(sizeof(struct disk_storage));

  s->st = vmalloc(MAX_SIZE * sizeof(struct store));
  memset(s->st, 0, MAX_SIZE * sizeof(struct store));
  s->trim_iid = 0;

  /*
  struct disk_storage_instance si;
  si.instance_id = 50;
  printk("Will save iid: %d", si.instance_id);
  device_put(si);
  // Wait until the data is saved to disk
  wait_for_completion(&comp);
  printk(KERN_INFO "Write woke me up, the data is safe in disk\n");
  */

  return s;
}

static int
disk_storage_open(void* handle)
{
  return 0;
}

static void
disk_storage_close(void* handle)
{
  int rmv = device_remove();

  if(rmv < 0){
      printk(KERN_INFO "Unable to unload\n");
  }

  struct disk_storage* s = handle;
  if (s) {
    vfree(s->st);
    vfree(s);
  }
}

static int
disk_storage_tx_begin(void* handle)
{
  return 0;
}

static int
disk_storage_tx_commit(void* handle)
{
  return 0;
}

static void
disk_storage_tx_abort(void* handle)
{}

// TODO: call storage_device function asking to get data back
static int
disk_storage_get(void* handle, iid_t iid, paxos_accepted* out)
{
  struct disk_storage* s = handle;
  int                 idx = iid % MAX_SIZE;

  if (s->st[idx].msg.iid == iid) {
    memcpy(out, &s->st[idx].msg, sizeof(paxos_accepted));
    if (out->value.paxos_value_len > 0) {
      out->value.paxos_value_val = pmalloc(out->value.paxos_value_len);
      memcpy(out->value.paxos_value_val, s->st[idx].data,
             out->value.paxos_value_len);
    }
    return 1;
  }

  return 0;
}

static int
disk_storage_put(void* handle, paxos_accepted* acc)
{
  struct disk_storage* s = handle;
  int                 idx = acc->iid % MAX_SIZE;

  // TODO: remove -- obsolete
  memcpy(&s->st[idx].msg, acc, sizeof(paxos_accepted));
  if (s->st[idx].msg.value.paxos_value_len > sizeof(s->st[idx].data)) {
    LOG_ERROR("Data will be truncated.");
    s->st[idx].msg.value.paxos_value_len = sizeof(s->st[idx].data);
  }
  memcpy(s->st[idx].data, acc->value.paxos_value_val,
         s->st[idx].msg.value.paxos_value_len);
  // end obsolete

  struct disk_storage_instance si;
  si.instance_id = idx;
  memcpy(&si.st.msg, acc, sizeof(paxos_accepted));
  if (si.st.msg.value.paxos_value_len > sizeof(si.st.data)) {
    LOG_ERROR("Data will be truncated.");
    si.st.msg.value.paxos_value_len = sizeof(si.st.data);
  }
  memcpy(si.st.data, acc->value.paxos_value_val,
         si.st.msg.value.paxos_value_len);
  // save message and data to disk
  printk("Will save iid: %d", si.instance_id);
  int test = device_put(si);
  if(test != 0){
    printk("Put returned with error, will have to wait");
    return test;
  }
  // Wait until the data is saved to disk
  //wait_for_completion(&comp);
  printk(KERN_INFO "Write woke me up, the data is safe in disk\n");

  return 0;
}

// CHECK WHAT THIS IS FOR
static int
disk_storage_trim(void* handle, iid_t iid)
{
  struct disk_storage* s = handle;
  s->trim_iid = iid;
  return 0;
}

static iid_t
disk_storage_get_trim_instance(void* handle)
{
  struct disk_storage* s = handle;
  return s->trim_iid;
}

void
storage_init_disk(struct storage* s, int acceptor_id)
{
  s->handle = disk_storage_new(acceptor_id);
  s->api.open = disk_storage_open;
  s->api.close = disk_storage_close;
  s->api.tx_begin = disk_storage_tx_begin;
  s->api.tx_commit = disk_storage_tx_commit;
  s->api.tx_abort = disk_storage_tx_abort;
  s->api.get = disk_storage_get;
  s->api.put = disk_storage_put;
  s->api.trim = disk_storage_trim;
  s->api.get_trim_instance = disk_storage_get_trim_instance;
}