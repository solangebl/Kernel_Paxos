#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#define TIMEOUT 10
#define SUCCESS 0

struct paxos_value
{
  int   paxos_value_len;
  char* paxos_value_val;
};
typedef struct paxos_value paxos_value;

struct paxos_accepted
{
  uint32_t    aid;
  uint32_t    iid;
  uint32_t    promise_iid;
  uint32_t    ballot;
  uint32_t    value_ballot;
  paxos_value value;
};
typedef struct paxos_accepted paxos_accepted;

typedef struct store
{
  paxos_accepted msg;
  char           data[1500*8];
} store_t;

typedef struct requestData {
    int type;
    int iid;
    store_t s;
} Request;

static int stop = 0;
static char userBuff[sizeof(Request)] = {0};
static int bufferSize = sizeof(userBuff);

void
stop_execution(int signo)
{
  stop = 1;
}

/********************* DISK FUNCTIONS *************************/

// effect: save data from buffer to file
// return: status
static int 
save_data(void){
  
  FILE *fp;
  Request *test_req = (Request *) userBuff;
  printf("Value ballot: %d\n", test_req->s.msg.value_ballot);
  
  char file_name[sizeof("/tmp/acceptor_data/") + sizeof(test_req->iid) ];
  sprintf(file_name, "/tmp/acceptor_data/%d", test_req->iid);
  printf("New file name: %s\n", file_name);
  // open file in append mode
  fp = fopen(file_name, "wb");
  if(fp!=NULL){
    printf("buffer size: %d\n", bufferSize);
    fwrite(test_req, bufferSize, 1, fp);
    printf("File was written\n");
    fclose(fp);

    return SUCCESS;
  }

  return -1;
}

// effect: save read data to buffer
// return: status
static int 
get_data(Request *req){
  
  FILE *fp;

  char file_name[sizeof("/tmp/acceptor_data/") + sizeof(req->iid) ];
  sprintf(file_name, "/tmp/acceptor_data/%d", req->iid);
  printf("Will retrieve data from: %s\n", file_name);
  // open file in read mode
  fp = fopen(file_name, "rb");
  if(fp!=NULL){
    fread(userBuff, bufferSize, 1, fp);
    printf("File was read\n");
    printf("Size of read: %lu\n", sizeof(userBuff));
    fclose(fp);
    return SUCCESS;
  }

  return -1;
}

/****************** FILE OPERATIONS ********************/
/*
  effect: read kernel data to buffer
  return: status
*/
static int
read_file(size_t fd)
{
    int len = read(fd, userBuff, 200);
    if (len < 0)
      return -1;

    if (len == 0) {
        printf("Stopped by kernel module\n");
        stop = 1;
        return -1;
    }

    // cast buffer to Request
    //Request *test_req = (Request *) userBuff;
    
    return SUCCESS;
    // return test_req->type;

}

static int
write_file(size_t fd)
{
  // write buffer to fd
  int len = write(fd, userBuff, bufferSize);

  if (len < 0)
      return -1;

  if (len == 0) {
      printf("Stopped by kernel module\n");
      stop = 1;
      return -1;
  }

  return SUCCESS;
}

int main(int argc, char *argv[]){

    size_t fd = open("/dev/kacc_u", O_RDWR);

    if(fd<0){
        printf("Error opening char device\n");
        return 1;
    }

    printf("Opened the char device\n");

    // create the directory to store data
    int cdir = mkdir("/tmp/acceptor_data/", S_IRUSR | S_IWUSR | S_IXUSR);
    if(cdir<0){
        printf("Error creating temp directory\n");
        return 1;
    }

    struct pollfd pol[1]; // one event: file
    pol[0].fd = fd;
    pol[0].events = POLLIN;

    signal(SIGINT, stop_execution);
    
    // wait until data is available
    while(!stop){
      poll(pol, 1, TIMEOUT*1000);
      if(pol[0].revents & POLLIN){

        printf("Kernel asking for something\n");
        // read file and save data
        int read = read_file(fd);

        if(read!=SUCCESS){
          printf("There was an error with the request\n");
          break;
        }

        Request *req = (Request *) userBuff;
        printf("Read the file and got this type: %d\n", req->type);
        switch(req->type){
          // kernel asked to save data
          case 0:
          {
            int saved = save_data();
    
            if(saved==SUCCESS){
              // let the kernel know the data was saved
              printf("Data was saved\n");
              // TODO: method to send back confirmation
              Request t_req;
              t_req.type = 2;
              int wr = write(fd, &t_req, sizeof(Request));
              if(wr == 0){
                printf("Nothing was written to char device");
              }

              if(wr == -1){
                printf("There was an error writing to char device");
              }

            }
            break;
          }
          // kernel asked to get data back
          case 1:
          {
            int ret = get_data(req);
            if(ret==SUCCESS){
              ret = write_file(fd);
              if(ret==SUCCESS){
                printf("Wrote the thing back\n");
              }
            }
            break;
          }
        }

      }
    }

    return 0;
}
