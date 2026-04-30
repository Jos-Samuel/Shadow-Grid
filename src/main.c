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

void handle_sigint(int sig) {
    printf("\nShutting down server...\n");
    if (logger_pid > 0) kill(logger_pid, SIGTERM);
    munmap(shm_ptr, SHM_SIZE);
    shm_unlink(SHM_NAME);
    exit(0);
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
        while (1) {
            if (shm_ptr && strlen(shm_ptr) > 0) {
                time_t now = time(NULL);
                struct tm *t = localtime(&now);
                char timebuf[64];
                strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);
                struct flock lock;
                lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET; lock.l_start = 0; lock.l_len = 0;
                fcntl(fileno(logfile), F_SETLKW, &lock);
                fprintf(logfile, "[%s] %s", timebuf, shm_ptr);
                fflush(logfile);
                lock.l_type = F_UNLCK; fcntl(fileno(logfile), F_SETLK, &lock);
                shm_ptr[0] = '\0';
            }
            if (getppid() == 1) break;
            usleep(1000);
        }
        fclose(logfile);
        exit(0);
    }

    signal(SIGTERM, handle_sigint);
    log_event("EVENT:SERVER_START\n");

    while (1) {
        int *new_socket = malloc(sizeof(int));
        *new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (*new_socket < 0) {
            free(new_socket);
            continue;
        }
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, new_socket);
        pthread_detach(tid);
    }
    return 0;
}
