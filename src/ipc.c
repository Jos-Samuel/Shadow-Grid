#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "ipc.h"

char SHM_NAME[64];
char *shm_ptr;
pid_t logger_pid;

pthread_mutex_t ipc_lock = PTHREAD_MUTEX_INITIALIZER;

void log_event(const char *msg) {
    if (shm_ptr) {
        pthread_mutex_lock(&ipc_lock);
        int watchdog = 1000;
        while (shm_ptr[0] != '\0' && watchdog-- > 0) {
            usleep(100);
        }
        snprintf(shm_ptr, SHM_SIZE, "%s", msg);
        pthread_mutex_unlock(&ipc_lock);
    }
}
