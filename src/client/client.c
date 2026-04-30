#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "common.h"
#include "protocol.h"

int main() {
    int sock;
    struct sockaddr_in serv_addr;

    char buffer[BUFFER_SIZE];
    char input[BUFFER_SIZE];

    sock = socket(AF_INET, SOCK_STREAM, 0);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    int n = read(sock, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        printf("Failed to connect or server disconnected\n");
        close(sock);
        return 1;
    }
    buffer[n] = '\0';
    printf("Server: %s", buffer);

    send(sock, "TYPE:JOIN\n", 10, 0);

    n = read(sock, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        printf("Failed to read JOIN response\n");
        close(sock);
        return 1;
    }
    buffer[n] = '\0';
    printf("Server: %s", buffer);

    fd_set readfds;
    int maxfd = (sock > STDIN_FILENO ? sock : STDIN_FILENO) + 1;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        select(maxfd, &readfds, NULL, NULL, NULL);

        // server messages
        if (FD_ISSET(sock, &readfds)) {
            int n = read(sock, buffer, sizeof(buffer)-1);
            if (n <= 0) {
                printf("Disconnected from server\n");
                break;
            }
            buffer[n] = '\0';
            char *line = strtok(buffer, "\n");
            while (line != NULL) {
                if (strncmp(line, "STATE:", 6) == 0) {
                    printf("\n[STATE] %s\n", line);
                } else if (strncmp(line, "UPDATE:", 7) == 0) {
                    printf("\n[UPDATE] %s\n", line);
                } else {
                    printf("\n[SERVER] %s\n", line);
                }
                line = strtok(NULL, "\n");
            }
            printf("Enter command: ");
            fflush(stdout);
        }

        // user input
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(input, sizeof(input), stdin) == NULL) {
                break;
            }

            if (strncmp(input, "move ", 5) == 0) {
                char msg[BUFFER_SIZE];
                input[strcspn(input, "\n")] = '\0';
                char *dir = input + 5;
                snprintf(msg, sizeof(msg), "TYPE:MOVE;DIR:%s\n", dir);
                send(sock, msg, strlen(msg), 0);

            } else if (strncmp(input, "role ", 5) == 0) {
                char msg[BUFFER_SIZE];
                input[strcspn(input, "\n")] = '\0';
                char *role = input + 5;

                snprintf(msg, sizeof(msg), "TYPE:ROLE;SET:%s\n", role);
                send(sock, msg, strlen(msg), 0);

            } else if (strncmp(input, "shoot ", 6) == 0) {
                char msg[BUFFER_SIZE];
                input[strcspn(input, "\n")] = '\0';
                char *dir = input + 6;

                if (strlen(dir) == 0) {
                    printf("Usage: shoot up|down|left|right\n");
                    continue;
                }

                snprintf(msg, sizeof(msg), "TYPE:SHOOT;DIR:%s\n", dir);
                send(sock, msg, strlen(msg), 0);

            } else if (strncmp(input, "status", 6) == 0) {
                send(sock, "TYPE:STATUS\n", 12, 0);

            } else if (strncmp(input, "respawn", 7) == 0) {
                send(sock, "TYPE:RESPAWN\n", 13, 0);

            } else if (strncmp(input, "quit", 4) == 0) {
                send(sock, "TYPE:QUIT\n", 10, 0);
                break;

            } else {
                printf("Invalid command\n");
            }
        }
    }

    close(sock);
    return 0;
}