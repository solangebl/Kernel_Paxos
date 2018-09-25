#ifndef K_FILE
#define K_FILE

#include "acceptor_common.h"
#include <linux/fs.h>      // Needed by filp
#include <linux/syscalls.h>
#include <linux/uaccess.h>   // Needed by segment descriptors
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/vmalloc.h>

#define SUCCESS 0
#define CHR_NAME_U "kacceptor_u"

// for flags and mode, see http://man7.org/linux/man-pages/man2/open.2.html

int dev_open(struct inode *,struct file *);
ssize_t dev_read(struct file*, char*, size_t, loff_t*);
unsigned int dev_poll(struct file*, poll_table*);
ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
/*void file_close(struct file* file);
int  file_sync(struct file* file);
*/

// Char device registration functions
int device_init(void);
int device_put(struct disk_storage_instance);
struct disk_storage* device_get(void);
int device_remove(void);

#endif
