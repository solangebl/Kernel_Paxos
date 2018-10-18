#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/uaccess.h>

#include "disk_thread.h"

// this completion tells the thread there's new data in the buffer

struct completion available_data;
struct completion saved_data;
wait_queue_head_t access_wait_u;

extern int doRequest;

static struct mutex diskBuff_m;

extern char diskBuff;
static struct task_struct *out_id;

static int output_thread(void *arg)
{
  int cnt = 0;

  while (!kthread_should_stop()) {
    // wait for new data to arrive
    wait_for_completion(&available_data);

    // once it's available wake up user process
    printk("Data is available for the thread");
    
    doRequest = 1;
    wake_up(&access_wait_u);

    // wait for data to be saved
    wait_for_completion(&saved_data);
    printk("Data was saved, thread will continue");

    /*
    mutex_lock(&diskBuff_m);
    printk("I'm the thread");
    mutex_unlock(&diskBuff_m);
    //msleep(500);
    */
  }

  return 0;
}

int 
thread_create(void) {

  mutex_init(&diskBuff_m);
  init_completion(&available_data);
  init_completion(&saved_data);

  out_id = kthread_run(output_thread, NULL, "out_thread");
  if (IS_ERR(out_id)) {
    printk("Error creating kernel thread!\n");

    return PTR_ERR(out_id);
  }

  return 0;
}

void thread_destroy(void)
{
  kthread_stop(out_id);
}

int get_data(char *p, int size)
{
  int len;

  mutex_lock(&diskBuff_m);
  /*
  len = strlen(buff);
  if (len > size) {
    len = size;
  }
  if (copy_to_user(p, buff, len)) {
    len = -1;
  }
  */
  mutex_unlock(&diskBuff_m);

  return len;
}