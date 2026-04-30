#ifndef NETWORK_H
#define NETWORK_H

#include <pthread.h>
#include "state.h"

extern int client_sockets[MAX_PLAYERS];
extern pthread_mutex_t state_lock;
extern int event_seq;

void send_response(int sock, const char *msg);
void broadcast(const char *msg);
void broadcast_snapshot();
void *handle_client(void *arg);

#endif
