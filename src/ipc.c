#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ipc.h"

char SHM_NAME[64];
char *shm_ptr;
pid_t logger_pid;

void log_event(const char *msg) {
    if (shm_ptr) {
        int watchdog = 1000;
        while (shm_ptr[0] != '\0' && watchdog-- > 0) {
            usleep(100);
        }
        snprintf(shm_ptr, SHM_SIZE, "%s", msg);
    }
}
