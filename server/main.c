#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <arpa/inet.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>

int exploit(int argc, char *argv[]);
int rmi(int argc, char *argv[]);

int
main(int argc, char *argv[])
{
    uid_t euid = geteuid(); 
    if (euid == 0) {
        int ret = rmi(argc, argv);
        printf("Ret: %d\n", ret);
        return ret;
    } else {
        return exploit(argc, argv);
    }
}
