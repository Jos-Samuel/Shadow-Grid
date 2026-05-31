#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <strings.h>
#include <fcntl.h>
#include "network.h"
#include "state.h"
#include "event.h"
#include "ipc.h"
#include "auth.h"
#include "common.h"

int client_sockets[MAX_PLAYERS];
pthread_mutex_t state_lock;
int event_seq = 0;

#define BCAST_QUEUE_SIZE 1024
static char *bcast_queue[BCAST_QUEUE_SIZE];
static int bcast_head = 0;
static int bcast_tail = 0;
static sem_t bcast_sem_empty;
static sem_t bcast_sem_full;
static pthread_mutex_t bcast_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_broadcast_queue() {
    sem_init(&bcast_sem_empty, 0, BCAST_QUEUE_SIZE);
    sem_init(&bcast_sem_full, 0, 0);
    for (int i = 0; i < BCAST_QUEUE_SIZE; i++) bcast_queue[i] = NULL;
}

void *broadcast_thread_func(void *arg) {
    (void)arg;
    while (1) {
        sem_wait(&bcast_sem_full);
        pthread_mutex_lock(&bcast_mutex);
        char *msg = bcast_queue[bcast_tail];
        bcast_tail = (bcast_tail + 1) % BCAST_QUEUE_SIZE;
        pthread_mutex_unlock(&bcast_mutex);
        sem_post(&bcast_sem_empty);

        if (msg) {
            pthread_mutex_lock(&state_lock);
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].active && client_sockets[i] != -1) {
                    if (send(client_sockets[i], msg, strlen(msg), 0) <= 0) {
                    // Don't close the socket here — let handle_client detect the error
                    // on its next read() and perform the cleanup itself, avoiding a double-close race.
                    client_sockets[i] = -1;
                }
                }
            }
            pthread_mutex_unlock(&state_lock);
            free(msg);
        }
    }
    return NULL;
}

void send_response(int sock, const char *msg) {
    send(sock, msg, strlen(msg), 0);
}

void broadcast(const char *msg) {
    char *msg_copy = strdup(msg);
    if (!msg_copy) return;
    sem_wait(&bcast_sem_empty);
    pthread_mutex_lock(&bcast_mutex);
    bcast_queue[bcast_head] = msg_copy;
    bcast_head = (bcast_head + 1) % BCAST_QUEUE_SIZE;
    pthread_mutex_unlock(&bcast_mutex);
    sem_post(&bcast_sem_full);
}

void broadcast_snapshot() {
    char snapshot[8192];
    pthread_mutex_lock(&state_lock);
    build_snapshot(snapshot, sizeof(snapshot));
    pthread_mutex_unlock(&state_lock);
    broadcast(snapshot);
}

void *handle_client(void *arg) {
    int client_socket = *(int *)arg;
    int player_id = -1;
    free(arg);

    char buffer[BUFFER_SIZE * 2];
    int buf_len = 0;

    send_response(client_socket, "RESP:OK;MSG:CONNECTED\n");

    while (1) {
        int n = read(client_socket, buffer + buf_len, sizeof(buffer) - buf_len - 1);
        if (n <= 0) {
            if (player_id >= 0) {
                pthread_mutex_lock(&state_lock);
                if (players[player_id].active) {
                    remove_player(player_id);  // Clears active, squad_id, and WFG edges
                    client_sockets[player_id] = -1;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "UPDATE:LEFT;ID:%d\n", player_id);
                    pthread_mutex_unlock(&state_lock);
                    broadcast(msg);
                    broadcast_snapshot();
                } else {
                    pthread_mutex_unlock(&state_lock);
                }
            }
            close(client_socket);
            break;
        }

        buf_len += n;
        buffer[buf_len] = '\0';
        char *line_start = buffer;
        char *newline;
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            Event e = parse_event(line_start);
            if (e.type == EVENT_UNKNOWN) {
                send_response(client_socket, "RESP:ERR;MSG:UNKNOWN\n");
                line_start = newline + 1;
                continue;
            }

            char reply[256] = "";
            char update[256] = "";
            char update_extra[256] = "";
            char logmsg[256] = "";
            int do_broadcast = 0;
            int do_broadcast_extra = 0;
            int do_snapshot = 0;
            int victim_died_sock = -1;
            int kick_sock = -1;  // Set only for KICK — triggers forced TCP shutdown

            pthread_mutex_lock(&state_lock);
            event_seq++;
            switch (e.type) {
            case EVENT_ROLE: {
                if (player_id < 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:JOIN_FIRST\n");
                else {
                    if (strcasecmp(e.dir, "PLAYER") == 0) { players[player_id].role = ROLE_PLAYER; snprintf(reply, sizeof(reply), "RESP:OK;MSG:ROLE_UPDATED\n"); }
                    else if (strcasecmp(e.dir, "SPECTATOR") == 0) { players[player_id].role = ROLE_SPECTATOR; snprintf(reply, sizeof(reply), "RESP:OK;MSG:ROLE_UPDATED\n"); }
                    else if (strcasecmp(e.dir, "ADMIN") == 0) {
                        if (strcmp(e.pass, ADMIN_PASS) == 0) { players[player_id].role = ROLE_ADMIN; snprintf(reply, sizeof(reply), "RESP:OK;MSG:ROLE_UPDATED\n"); }
                        else snprintf(reply, sizeof(reply), "RESP:ERR;MSG:UNAUTHORIZED\n");
                    } else snprintf(reply, sizeof(reply), "RESP:ERR;MSG:INVALID_ROLE\n");
                }
                break;
            }
            case EVENT_JOIN: {
                if (player_id == -1) player_id = register_player();
                if (player_id >= 0) {
                    client_sockets[player_id] = client_socket;
                    snprintf(reply, sizeof(reply), "RESP:OK;MSG:JOINED;ID:%d;POS:(%d,%d)\n", player_id, players[player_id].x, players[player_id].y);
                    snprintf(update, sizeof(update), "UPDATE:JOINED;ID:%d\n", player_id);
                    do_broadcast = 1; do_snapshot = 1; snprintf(logmsg, sizeof(logmsg), "EVENT:JOIN;ID:%d\n", player_id);
                } else snprintf(reply, sizeof(reply), "RESP:ERR;MSG:SERVER_FULL\n");
                break;
            }
            case EVENT_MOVE: {
                if (player_id < 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:JOIN_FIRST\n");
                else if (players[player_id].role == ROLE_SPECTATOR) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:NOT_ALLOWED\n");
                else if (!players[player_id].active || players[player_id].health <= 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:DEAD\n");
                else {
                    apply_move(player_id, e.dir);
                    snprintf(logmsg, sizeof(logmsg), "EVENT:MOVE;ID:%d;POS:(%d,%d)\n", player_id, players[player_id].x, players[player_id].y);
                    snprintf(reply, sizeof(reply), "RESP:OK;MSG:MOVED;POS:(%d,%d)\n", players[player_id].x, players[player_id].y);
                    snprintf(update, sizeof(update), "UPDATE:PLAYER:%d;POS:(%d,%d)\n", player_id, players[player_id].x, players[player_id].y);
                    do_broadcast = 1; do_snapshot = 1;
                }
                break;
            }
            case EVENT_SHOOT: {
                if (player_id < 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:JOIN_FIRST\n");
                else if (players[player_id].role == ROLE_SPECTATOR) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:NOT_ALLOWED\n");
                else if (!players[player_id].active || players[player_id].health <= 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:DEAD\n");
                else {
                    int victim_id = find_target_in_direction(player_id, e.dir);
                    if (victim_id == -1) { snprintf(logmsg, sizeof(logmsg), "EVENT:MISS;FROM:%d;DIR:%s\n", player_id, e.dir); snprintf(reply, sizeof(reply), "RESP:OK;MSG:MISS\n"); do_snapshot = 1; }
                    else if (players[victim_id].role == ROLE_ADMIN) {
                        snprintf(reply, sizeof(reply), "RESP:OK;MSG:HIT_BLOCKED\n");
                        snprintf(logmsg, sizeof(logmsg), "EVENT:BLOCK;FROM:%d;TO:%d\n", player_id, victim_id);
                        do_snapshot = 1;
                    } else if (players[player_id].squad_id != -1 && players[player_id].squad_id == players[victim_id].squad_id) {
                        snprintf(reply, sizeof(reply), "RESP:OK;MSG:FRIENDLY_FIRE_BLOCKED\n");
                        snprintf(logmsg, sizeof(logmsg), "EVENT:FRIENDLY_FIRE;FROM:%d;TO:%d\n", player_id, victim_id);
                    } else {
                        players[victim_id].health -= SHOT_DAMAGE;
                        if (players[victim_id].health < 0) players[victim_id].health = 0;
                        snprintf(logmsg, sizeof(logmsg), "EVENT:HIT;FROM:%d;TO:%d;HP:%d\n", player_id, victim_id, players[victim_id].health);
                        snprintf(update, sizeof(update), "UPDATE:HIT;FROM:%d;TO:%d;HP:%d\n", player_id, victim_id, players[victim_id].health);
                        do_broadcast = 1;
                        if (players[victim_id].health == 0) {
                            players[player_id].score += 1; victim_died_sock = client_sockets[victim_id];
                            snprintf(update_extra, sizeof(update_extra), "UPDATE:KILL;KILLER:%d;VICTIM:%d;SCORE:%d\n", player_id, victim_id, players[player_id].score);
                            do_broadcast_extra = 1; snprintf(reply, sizeof(reply), "RESP:OK;MSG:HIT_KILL;TARGET:%d;SCORE:%d\n", victim_id, players[player_id].score);
                            char kill_log[128]; snprintf(kill_log, sizeof(kill_log), "EVENT:KILL;KILLER:%d;VICTIM:%d\n", player_id, victim_id);
                            strncat(logmsg, kill_log, sizeof(logmsg) - strlen(logmsg) - 1);
                        } else snprintf(reply, sizeof(reply), "RESP:OK;MSG:HIT;TARGET:%d;HP:%d\n", victim_id, players[victim_id].health);
                        do_snapshot = 1;
                    }
                }
                break;
            }
            case EVENT_STATUS: if (player_id < 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:JOIN_FIRST\n"); else build_status(player_id, reply, sizeof(reply)); break;
            case EVENT_RESPAWN:
                if (player_id < 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:JOIN_FIRST\n"); else if (players[player_id].health > 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:ALIVE\n");
                else { players[player_id].health = 100; players[player_id].x = 0; players[player_id].y = 0; snprintf(logmsg, sizeof(logmsg), "EVENT:RESPAWN;ID:%d\n", player_id); snprintf(reply, sizeof(reply), "RESP:OK;MSG:RESPAWNED\n"); snprintf(update, sizeof(update), "UPDATE:RESPAWN;ID:%d\n", player_id); do_broadcast = 1; do_snapshot = 1; }
                break;
            case EVENT_QUIT:
                if (player_id >= 0) { remove_player(player_id); client_sockets[player_id] = -1; snprintf(update, sizeof(update), "UPDATE:LEFT;ID:%d\n", player_id); do_broadcast = 1; do_snapshot = 1; snprintf(logmsg, sizeof(logmsg), "EVENT:QUIT;ID:%d\n", player_id); }
                snprintf(reply, sizeof(reply), "RESP:OK;MSG:BYE\n"); break;
            case EVENT_KICK: {
                if (player_id < 0 || players[player_id].role != ROLE_ADMIN) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:UNAUTHORIZED\n");
                else if (e.target_id < 0 || e.target_id >= MAX_PLAYERS || !players[e.target_id].active) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:INVALID_TARGET\n");
                else {
                    players[e.target_id].health = 0; victim_died_sock = client_sockets[e.target_id]; remove_player(e.target_id); client_sockets[e.target_id] = -1;
                    kick_sock = victim_died_sock;  // Force-close this socket after notifying the client
                    snprintf(update, sizeof(update), "UPDATE:LEFT;ID:%d\n", e.target_id); do_broadcast = 1; do_snapshot = 1;
                    snprintf(logmsg, sizeof(logmsg), "EVENT:KICK;BY:%d;TARGET:%d\n", player_id, e.target_id); snprintf(reply, sizeof(reply), "RESP:OK;MSG:KICKED\n");
                }
                break;
            }
            case EVENT_HEAL_ALL: {
                if (player_id < 0 || players[player_id].role != ROLE_ADMIN) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:UNAUTHORIZED\n");
                else {
                    for (int i = 0; i < MAX_PLAYERS; i++) { if (players[i].active && players[i].health > 0) players[i].health = 100; }
                    snprintf(reply, sizeof(reply), "RESP:OK;MSG:HEALED_ALL\n"); do_snapshot = 1; snprintf(logmsg, sizeof(logmsg), "EVENT:HEAL_ALL;BY:%d\n", player_id);
                }
                break;
            }
            case EVENT_SMITE: {
                if (player_id < 0 || players[player_id].role != ROLE_ADMIN) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:UNAUTHORIZED\n");
                else if (e.target_id < 0 || e.target_id >= MAX_PLAYERS || !players[e.target_id].active || players[e.target_id].health <= 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:INVALID_TARGET\n");
                else {
                    players[e.target_id].health = 0; victim_died_sock = client_sockets[e.target_id];
                    snprintf(update, sizeof(update), "UPDATE:HIT;FROM:%d;TO:%d;HP:0\n", player_id, e.target_id); do_broadcast = 1; do_snapshot = 1;
                    snprintf(logmsg, sizeof(logmsg), "EVENT:SMITE;BY:%d;TARGET:%d\n", player_id, e.target_id); snprintf(reply, sizeof(reply), "RESP:OK;MSG:SMITED\n");
                    char kill_log[128]; snprintf(kill_log, sizeof(kill_log), "EVENT:KILL;KILLER:%d;VICTIM:%d\n", player_id, e.target_id); strncat(logmsg, kill_log, sizeof(logmsg) - strlen(logmsg) - 1);
                    players[player_id].score += 1;  // Increment before broadcasting so SCORE is correct
                    snprintf(update_extra, sizeof(update_extra), "UPDATE:KILL;KILLER:%d;VICTIM:%d;SCORE:%d\n", player_id, e.target_id, players[player_id].score); do_broadcast_extra = 1;
                }
                break;
            }
            case EVENT_CREATE_SQUAD: {
                if (player_id < 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:JOIN_FIRST\n");
                else if (players[player_id].role != ROLE_ADMIN) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:UNAUTHORIZED\n");
                else if (e.target_count == 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:NO_TARGETS\n");
                else {
                    int squad[12];
                    squad[0] = player_id;
                    int count = 1;
                    for (int i = 0; i < e.target_count; i++) squad[count++] = e.targets[i];
                    for (int i = 0; i < count - 1; i++) {
                        for (int j = i + 1; j < count; j++) {
                            if (squad[i] > squad[j]) { int tmp = squad[i]; squad[i] = squad[j]; squad[j] = tmp; }
                        }
                    }
                    int unique_squad[12]; int u_count = 0;
                    for (int i = 0; i < count; i++) { if (i == 0 || squad[i] != squad[i - 1]) unique_squad[u_count++] = squad[i]; }
                    // Validate all IDs before locking any mutex to prevent out-of-bounds array access
                    int all_valid = 1;
                    for (int i = 0; i < u_count; i++) {
                        if (unique_squad[i] < 0 || unique_squad[i] >= MAX_PLAYERS) { all_valid = 0; break; }
                    }
                    if (!all_valid) {
                        snprintf(reply, sizeof(reply), "RESP:ERR;MSG:INVALID_TARGET\n");
                    } else {
                        for (int i = 0; i < u_count; i++) pthread_mutex_lock(&player_locks[unique_squad[i]]);
                        int new_squad = player_id + 100;
                        for (int i = 0; i < u_count; i++) {
                            int id = unique_squad[i];
                            if (id >= 0 && id < MAX_PLAYERS && players[id].active) players[id].squad_id = new_squad;
                        }
                        for (int i = u_count - 1; i >= 0; i--) pthread_mutex_unlock(&player_locks[unique_squad[i]]);
                        snprintf(reply, sizeof(reply), "RESP:OK;MSG:SQUAD_CREATED;ID:%d\n", new_squad);
                        snprintf(logmsg, sizeof(logmsg), "EVENT:CREATE_SQUAD;BY:%d;SQUAD:%d\n", player_id, new_squad);
                    }
                }
                break;
            }
            case EVENT_INVITE: {
                if (player_id < 0) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:JOIN_FIRST\n");
                else if (players[player_id].role != ROLE_ADMIN) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:UNAUTHORIZED\n");
                else if (e.target_id < 0 || e.target_id >= MAX_PLAYERS || !players[e.target_id].active) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:INVALID_TARGET\n");
                else if (player_id == e.target_id) snprintf(reply, sizeof(reply), "RESP:ERR;MSG:CANNOT_INVITE_SELF\n");
                else {
                    int target_id = e.target_id;
                    pthread_mutex_lock(&player_locks[player_id]);
                    WFG[player_id][target_id] = 1;  // Register wait edge under state_lock

                    // Release state_lock before spinning so:
                    //   (a) other player threads are not frozen during the wait, and
                    //   (b) the deadlock reaper can acquire state_lock to run DFS and set deadlock_abort.
                    pthread_mutex_unlock(&state_lock);

                    int success = 0;
                    while (1) {
                        if (deadlock_abort[player_id]) { deadlock_abort[player_id] = 0; break; }
                        if (pthread_mutex_trylock(&player_locks[target_id]) == 0) { success = 1; break; }
                        usleep(1000);
                    }

                    // Re-acquire state_lock to safely clear WFG and mutate squad state.
                    pthread_mutex_lock(&state_lock);
                    WFG[player_id][target_id] = 0;  // Clear wait edge under state_lock

                    if (success) {
                        // Re-validate: target may have disconnected while we were spinning.
                        if (!players[target_id].active) {
                            pthread_mutex_unlock(&player_locks[target_id]);
                            pthread_mutex_unlock(&player_locks[player_id]);
                            snprintf(reply, sizeof(reply), "RESP:ERR;MSG:INVALID_TARGET\n");
                        } else {
                            int s_id = players[player_id].squad_id;
                            if (s_id == -1) { s_id = player_id + 100; players[player_id].squad_id = s_id; }
                            players[target_id].squad_id = s_id;
                            pthread_mutex_unlock(&player_locks[target_id]);
                            pthread_mutex_unlock(&player_locks[player_id]);
                            snprintf(reply, sizeof(reply), "RESP:OK;MSG:INVITED\n");
                            snprintf(logmsg, sizeof(logmsg), "EVENT:INVITE;FROM:%d;TO:%d\n", player_id, target_id);
                        }
                    } else {
                        pthread_mutex_unlock(&player_locks[player_id]);
                        snprintf(reply, sizeof(reply), "RESP:ERR;MSG:INVITE_FAILED_DEADLOCK_BROKEN\n");
                    }
                }
                break;
            }
            }
            
            if (logmsg[0] != '\0') {
                FILE *wal = fopen("wal.log", "a");
                if (wal) {
                    struct flock fl;
                    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 0;
                    fcntl(fileno(wal), F_SETLKW, &fl);
                    time_t now = time(NULL);
                    struct tm *t = localtime(&now);
                    char timebuf[64];
                    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);
                    fprintf(wal, "[%s] SEQ:%d;%s", timebuf, event_seq, logmsg);
                    fflush(wal);
                    fl.l_type = F_UNLCK;
                    fcntl(fileno(wal), F_SETLK, &fl);
                    fclose(wal);
                }
            }
            
            pthread_mutex_unlock(&state_lock);

            if (reply[0] != '\0') send_response(client_socket, reply);
            if (do_broadcast) broadcast(update);
            if (do_broadcast_extra) broadcast(update_extra);
            if (do_snapshot) broadcast_snapshot();
            if (victim_died_sock != -1) send_response(victim_died_sock, "RESP:ERR;MSG:YOU_DIED\n");
            // For kicked clients: force-close the TCP connection so their thread exits and releases the semaphore
            if (kick_sock != -1) shutdown(kick_sock, SHUT_RDWR);
            if (logmsg[0] != '\0') { char seqbuf[512]; snprintf(seqbuf, sizeof(seqbuf), "SEQ:%d;%s", event_seq, logmsg); log_event(seqbuf); }
            if (e.type == EVENT_QUIT) { close(client_socket); sem_post(&client_sem); return NULL; }
            line_start = newline + 1;
        }
        int remaining = buf_len - (line_start - buffer);
        if (remaining > 0) memmove(buffer, line_start, remaining);
        buf_len = remaining;
    }
    sem_post(&client_sem);
    return NULL;
}
