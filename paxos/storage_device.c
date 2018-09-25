#include "storage_device.h"

#define MODULE_NAME "kacceptor"


typedef struct requestData {
    int type;
    int iid;
    store_t s;
} Request;

static int majorNumberU;
static int times = 0;
// The device-driver class struct pointer
static struct device*    charDeviceU = NULL;
static struct class* charClassU = NULL;

// set major to 0 so the kernel assigns a number
static unsigned int major = 0;

static char localBuff[sizeof(Request)] = {0};
static int bufferSize = sizeof(localBuff);
static wait_queue_head_t access_wait_u;
int doRequest = 0;
static Request user_request;
static struct mutex char_mutex;

// Char device operation functions for user space char device
struct file_operations storage_fops_u = {
    .owner = THIS_MODULE,
    .read = dev_read,
    .write = dev_write,
    .open = dev_open,
    .poll = dev_poll
 //   .release = device_release
};

// ON OPEN - Count times opened
int dev_open(struct inode *ino,struct file *filp){

    printk(KERN_ALERT"User file open");

    times++;
    printk("The device was opened %d times\n", times);
    return 0;
}

ssize_t
dev_read(struct file* filep, char* buffer, size_t len, loff_t* offset)
{
    printk(KERN_INFO "Entered to read what's available\n");
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

ssize_t
dev_write(struct file *filp, const char *buff, size_t len, loff_t *off){

    int count;

    count = copy_from_user(localBuff, buff, bufferSize);
    if(count==0){
      Request *test_req = (Request *) localBuff;
      
      switch (test_req->type){
          case 2:
              printk(KERN_INFO "The data was successfully saved");
              mutex_unlock(&char_mutex);
              break;
          default:
              printk(KERN_INFO "Entered the default case");
              break;
      }
      
    }
    return count;
    
}

unsigned int
dev_poll(struct file* file, poll_table* wait)
{
    LOG_INFO("User space app asking to poll\n");
    if (doRequest == 1){
        LOG_INFO("POLLIN available");
        return POLLIN;
    }
    poll_wait(file, &access_wait_u, wait);

    return 0;
}

int 
device_init(void) {

    // reg the device driver and queue for the user space app
    majorNumberU = register_chrdev(major, CHR_NAME_U, &storage_fops_u);
    if(majorNumberU<0){
      printk(KERN_INFO "Could not register user major number \n");
      return -1;
    }

    dev_t myDevU = MKDEV(majorNumberU, 0);
    register_chrdev_region(myDevU, 1, "kacc_u");
    charClassU = class_create(THIS_MODULE, "chardrvu");
    charDeviceU = device_create(charClassU, NULL, myDevU, NULL, "kacc_u");

    init_waitqueue_head(&access_wait_u);
    printk(KERN_INFO "Registered user device with major number: %d \n", majorNumberU);

    mutex_init(&char_mutex);

    return SUCCESS;
}

int
//device_put(struct disk_storage *s) {
device_put(struct disk_storage_instance s) {

  if(!mutex_trylock(&char_mutex)){
    printk(KERN_INFO "Someone already asked for this. Wait!\n");
    return -1;
  }

  printk(KERN_INFO "Device put got instance id: %d \n", s.instance_id);

  user_request.type = 0;
  user_request.iid = s.instance_id;
  user_request.s = s.st;
  
  // set doRequest to 1
  doRequest = 1;
  wake_up(&access_wait_u);

  return SUCCESS;

}

struct disk_storage*
device_get(void){
  printk(KERN_INFO "Going to get what's been saved");

  user_request.type = 1;
  doRequest = 1;

  wake_up(&access_wait_u);

  struct disk_storage* s = vmalloc(sizeof(struct disk_storage));
  return s;

}

int
device_remove(void){

  device_destroy(charClassU, MKDEV(majorNumberU, 0)); // remove the device
  class_unregister(charClassU); // unregister the device class
  class_destroy(charClassU);    // remove the device class

  unregister_chrdev(majorNumberU, CHR_NAME_U);

  mutex_destroy(&char_mutex);
  
  printk(KERN_INFO "My module is unloaded\n");
  return SUCCESS;
  
}