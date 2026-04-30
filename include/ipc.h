#ifndef IPC_H
#define IPC_H

#include <sys/types.h>

#define SHM_NAME_BASE "/game_log_shm_"
#define SHM_SIZE 4096

extern char SHM_NAME[64];
extern char *shm_ptr;
extern pid_t logger_pid;

void log_event(const char *msg);

#endif
