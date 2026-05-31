#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include "common.h"
#include "state.h"
#include "network.h"
#include "ipc.h"
sem_t client_sem;

void handle_sigint(int sig) {
    (void)sig;
    printf("\nShutting down server...\n");
    if (logger_pid > 0) kill(logger_pid, SIGTERM);
    munmap(shm_ptr, SHM_SIZE);
    shm_unlink(SHM_NAME);
    sem_destroy(&client_sem);
    exit(0);
}

void *checkpoint_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(60); // Run checkpoint every 60 seconds
        
        pthread_mutex_lock(&state_lock);
        int cp_lsn = event_seq;
        
        FILE *fp = fopen("game_state.dat", "wb");
        if (fp) {
            fwrite(&cp_lsn, sizeof(int), 1, fp);
            fwrite(players, sizeof(Player), MAX_PLAYERS, fp);
            fclose(fp);
            
            // Also append a checkpoint entry to wal.log
            FILE *wal = fopen("wal.log", "a");
            if (wal) {
                struct flock fl;
                fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 0;
                fcntl(fileno(wal), F_SETLKW, &fl);
                time_t now = time(NULL);
                struct tm *t = localtime(&now);
                char timebuf[64];
                strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);
                
                fprintf(wal, "[%s] SEQ:%d;EVENT:CHECKPOINT\n", timebuf, cp_lsn);
                fflush(wal);
                fl.l_type = F_UNLCK;
                fcntl(fileno(wal), F_SETLK, &fl);
                fclose(wal);
            }
        }
        pthread_mutex_unlock(&state_lock);
    }
    return NULL;
}

int dfs(int v, int visited[], int recStack[]) {
    if (!visited[v]) {
        visited[v] = 1;
        recStack[v] = 1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (WFG[v][i]) {
                if (!visited[i] && dfs(i, visited, recStack) != -1) return i;
                else if (recStack[i]) return i;
            }
        }
    }
    recStack[v] = 0;
    return -1;
}

void *deadlock_reaper_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(2);
        pthread_mutex_lock(&state_lock);
        int visited[MAX_PLAYERS] = {0};
        int recStack[MAX_PLAYERS] = {0};
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!visited[i]) {
                int victim = dfs(i, visited, recStack);
                if (victim != -1) {
                    deadlock_abort[victim] = 1;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&state_lock);
    }
    return NULL;
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    listen(server_fd, 10);

    printf("Server running on port %d...\n", PORT);

    pthread_mutex_init(&state_lock, NULL);
    memset(players, 0, sizeof(players));
    for (int i = 0; i < MAX_PLAYERS; i++) client_sockets[i] = -1;

    snprintf(SHM_NAME, sizeof(SHM_NAME), "%s%d", SHM_NAME_BASE, PORT);
    shm_unlink(SHM_NAME);
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, SHM_SIZE);
    shm_ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    logger_pid = fork();
    if (logger_pid == 0) {
        close(server_fd);
        FILE *logfile = fopen("game.log", "a");
        
        char log_buffer[16384] = {0};
        size_t current_len = 0;
        time_t last_flush_time = time(NULL);
        const double FLUSH_INTERVAL = 2.0;

        while (1) {
            time_t now = time(NULL);
            int parent_died = (getppid() == 1);
            int has_data = (shm_ptr && shm_ptr[0] != '\0');
            size_t data_len = has_data ? strlen(shm_ptr) : 0;
            
            if (current_len > 0 && (parent_died || difftime(now, last_flush_time) >= FLUSH_INTERVAL || current_len + data_len + 64 > sizeof(log_buffer))) {
                struct flock lock;
                lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_start = 0; lock.l_len = 0;
                fcntl(fileno(logfile), F_SETLKW, &lock);
                
                fwrite(log_buffer, 1, current_len, logfile);
                fflush(logfile);
                
                lock.l_type = F_UNLCK; fcntl(fileno(logfile), F_SETLK, &lock);
                
                current_len = 0;
                last_flush_time = now;
            }

            if (has_data) {
                struct tm *t = localtime(&now);
                char timebuf[64];
                strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);
                
                int written = snprintf(log_buffer + current_len, sizeof(log_buffer) - current_len, 
                                     "[%s] %s", timebuf, shm_ptr);
                if (written > 0 && (size_t)written < (sizeof(log_buffer) - current_len)) {
                    current_len += written;
                }
                shm_ptr[0] = '\0';
            }

            if (parent_died) break;
            usleep(1000);
        }
        fclose(logfile);
        exit(0);
    }

    signal(SIGTERM, handle_sigint);
    log_event("EVENT:SERVER_START\n");

    sem_init(&client_sem, 0, 100);

    init_broadcast_queue();
    pthread_t bcast_tid;
    pthread_create(&bcast_tid, NULL, broadcast_thread_func, NULL);
    pthread_detach(bcast_tid);

    init_locks();
    pthread_t dl_tid;
    pthread_create(&dl_tid, NULL, deadlock_reaper_thread, NULL);
    pthread_detach(dl_tid);

    pthread_t cp_tid;
    pthread_create(&cp_tid, NULL, checkpoint_thread, NULL);
    pthread_detach(cp_tid);

    while (1) {
        sem_wait(&client_sem);
        int *new_socket = malloc(sizeof(int));
        *new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (*new_socket < 0) {
            free(new_socket);
            sem_post(&client_sem);
            continue;
        }
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, new_socket);
        pthread_detach(tid);
    }
    return 0;
}
