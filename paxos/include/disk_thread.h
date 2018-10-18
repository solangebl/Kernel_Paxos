#ifndef __DISK_THREAD_H__
#define __DISK_THREAD_H__
int thread_create(void);
void thread_destroy(void);
int save_data(char *p, int size);
extern struct completion available_data;
extern struct completion saved_data;
extern wait_queue_head_t access_wait_u;
#endif	/* __DISK_THREAD_H__ */