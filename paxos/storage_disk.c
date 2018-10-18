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
#include <linux/if_ether.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/syscalls.h>
#include <linux/mutex.h>

#include "disk_thread.h"

#define CHR_NAME_U "kacceptor_u"

extern struct mutex diskBuff_m;

static const int MAX_SIZE = 1000000;
static int majorNumberU;
// The device-driver class struct pointer
static struct device*    charDeviceU = NULL;
static struct class* charClassU = NULL;
// set major to 0 so the kernel assigns a number
static unsigned int major = 0;
// mutex for transactions
static struct mutex char_mutex;

// char device logging vars - remove
static int times = 0;
static int times_written = 0;

typedef struct requestData {
    int type;
    int iid;
    store_t s;
} Request;

// char device requests management
static char diskBuff[sizeof(Request)] = {0};
static int bufferSize = sizeof(diskBuff);

int doRequest = 0;
static Request user_request;

static int dev_open(struct inode *ino,struct file *filp){

    printk(KERN_ALERT"User file open");

    times++;
    printk("The device was opened %d times\n", times);
    return 0;
}

static ssize_t
dev_read(struct file* filep, char* buffer, size_t len, loff_t* offset)
{
    //printk(KERN_INFO "Entered to read what's available\n");
    int error_count;

    // to from len
    error_count = copy_to_user(buffer, &user_request, bufferSize);

    if (error_count != 0) {
        printk(KERN_INFO "send fewer characters to the user\n");
        return -1;
    }

    doRequest = 0;
    return bufferSize;
}

static void complete_saved(void){
  complete(&saved_data);
}

static ssize_t
dev_write(struct file *filp, const char *buff, size_t len, loff_t *off){

    int count;

    count = copy_from_user(diskBuff, buff, bufferSize);
    printk("Left to copy from user: %d\n", count);
    if(count==0){
        Request *test_req = (Request *) diskBuff;
        
        switch (test_req->type){
          case 2:
            times_written++;
            printk("The device was written %d times, will update saved data \n", times_written);
            // wake up kthread
            complete_saved();
            break;
          default:
            printk(KERN_INFO "Entered the default case");
            break;
        }
        
    }
    return count;
    
}

static unsigned int
dev_poll(struct file* file, poll_table* wait)
{
    //LOG_INFO("User space app asking to poll\n");
    if (doRequest == 1){
        //LOG_INFO("POLLIN available");
        return POLLIN;
    }
    //poll_wait(file, &access_wait_u, wait);

    return 0;
}

// Char device operation functions for user space char device
struct file_operations storage_fops_u = {
    .owner = THIS_MODULE,
    .read = dev_read,
    .write = dev_write,
    .open = dev_open,
    .poll = dev_poll
 //   .release = device_release
};

static int
device_prepare_request_data(store_t s) {

  user_request.type = 0;
  user_request.iid = 1;
  user_request.s = s;
  
  complete(&available_data);
  
  return 0;

}

static struct disk_storage*
disk_storage_new(int acceptor_id){

  // reg the device driver and queue for the user space app
  majorNumberU = register_chrdev(major, CHR_NAME_U, &storage_fops_u);
  if(majorNumberU<0){
    printk(KERN_INFO "Could not register user major number \n");
  }

  dev_t myDevU = MKDEV(majorNumberU, 0);
  register_chrdev_region(myDevU, 1, "kacc_u");
  charClassU = class_create(THIS_MODULE, "chardrvu");
  charDeviceU = device_create(charClassU, NULL, myDevU, NULL, "kacc_u");

  init_waitqueue_head(&access_wait_u);
  printk(KERN_INFO "Registered user device with major number: %d \n", majorNumberU);

  // create the thread that will synchronize with user space
  int res = thread_create();
  if(res!=0){
    printk(KERN_INFO "Error creating the thread\n");
  }
  
  // I will read data from memory during normal execution
  struct disk_storage* s = vmalloc(sizeof(struct disk_storage));

  s->st = vmalloc(MAX_SIZE * sizeof(struct store));
  memset(s->st, 0, MAX_SIZE * sizeof(struct store));
  s->trim_iid = 0;

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
  device_destroy(charClassU, MKDEV(majorNumberU, 0)); // remove the device
  class_unregister(charClassU); // unregister the device class
  class_destroy(charClassU);    // remove the device class

  unregister_chrdev(majorNumberU, CHR_NAME_U); // remove char device
  thread_destroy(); // remove disk thread

  mutex_destroy(&char_mutex);
  
  printk(KERN_INFO "My module is unloaded\n");

  struct disk_storage* s = handle;
  if (s) {
    vfree(s->st);
    vfree(s);
  }
}

static int
disk_storage_tx_begin(void* handle)
{
   mutex_lock(&char_mutex);
  
  return 0;
}

static int
disk_storage_tx_commit(void* handle)
{
   mutex_unlock(&char_mutex);
  return 0;
}

static void
disk_storage_tx_abort(void* handle)
{
   mutex_unlock(&char_mutex);
}

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

  memcpy(&s->st[idx].msg, acc, sizeof(paxos_accepted));
  if (s->st[idx].msg.value.paxos_value_len > sizeof(s->st[idx].data)) {
    LOG_ERROR("Data will be truncated.");
    s->st[idx].msg.value.paxos_value_len = sizeof(s->st[idx].data);
  }
  memcpy(s->st[idx].data, acc->value.paxos_value_val,
         s->st[idx].msg.value.paxos_value_len);
  
  int test = device_prepare_request_data(*s->st);
  if(test != 0){
    printk("Put returned with error, will have to wait");
    return test;
  }
  
  printk(KERN_INFO "Saved data: %d\n", si.instance_id);

  return 0;
}

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

