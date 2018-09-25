#ifndef COMMON_ACC
#define COMMON_ACC

#include "storage.h"
#include <linux/if_ether.h>
#include <linux/completion.h>

typedef struct store
{
  paxos_accepted msg;
  char           data[ETH_DATA_LEN - sizeof(paxos_accepted)];
} store_t;

// TODO: add char device info
struct disk_storage
{
  store_t* st;
  iid_t    trim_iid; // just for compatibility
};

struct disk_storage_instance{
  iid_t instance_id;
  store_t st;
};


#endif