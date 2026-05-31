#ifndef NETWORK_H
#define NETWORK_H

#include <pthread.h>
#include <semaphore.h>
#include "state.h"

extern int client_sockets[MAX_PLAYERS];
extern pthread_mutex_t state_lock;
extern int event_seq;
extern sem_t client_sem;

void send_response(int sock, const char *msg);
void broadcast(const char *msg);
void broadcast_snapshot();
void *handle_client(void *arg);
void init_broadcast_queue();
void *broadcast_thread_func(void *arg);

#endif
