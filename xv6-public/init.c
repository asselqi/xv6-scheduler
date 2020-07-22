// init: The initial user-level program

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
//#define SINGLESHOT

char *argv[] = { "sh", 0 };


#ifdef SINGLESHOT
void check_proc_order();
int main(void)
{
  //int pid, wpid;

  if(open("console", O_RDWR) < 0){
    mknod("console", 1, 1);
    open("console", O_RDWR);
  }
  dup(0);  // stdout
  dup(0);  // stderr
  check_proc_order();
  halt();
  return 0;
}
#else

int
main(void)
{
  int pid, wpid;

  if(open("console", O_RDWR) < 0){
    mknod("console", 1, 1);
    open("console", O_RDWR);
  }
  dup(0);  // stdout
  dup(0);  // stderr

#ifdef SINGLESHOT

    // exec the tester and don't wait for it
    pid = fork();
    if(pid < 0){
      printf(1, "init: fork failed\n");
      exit();
    }
    if (pid == 0) {
        exec("open_files", argv);
        printf(1, "init: exec sh failed\n");
        exit();
    }

    sleep(100);
    // exec the lsof
    pid = fork();
    if (pid < 0) {
        printf(1, "init: fork failed\n");
        exit();
    }
    if (pid == 0) {
        exec("lsof", argv);
        printf(1, "init: exec sh failed\n");
        exit();
    }
    wait();
    wait();
    halt();
#endif


    for(;;){
        printf(1, "init: starting sh\n");
        pid = fork();
    if(pid < 0){
            printf(1, "init: fork failed\n");
            exit();
        }
    if(pid == 0){
            exec("sh", argv);
            printf(1, "init: exec sh failed\n");
            exit();
        }
    while((wpid=wait()) >= 0 && wpid != pid)
      printf(1, "zombie!\n");
  }
}
#endif