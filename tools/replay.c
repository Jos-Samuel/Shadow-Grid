#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PLAYERS 100
#define MAP_H 20
#define MAP_W 20

typedef struct {
    int active;
    int x, y;
    int health;
    int score;
} Player;

Player players[MAX_PLAYERS];
char last_action[256] = "Replay started...";

void init_players() {
    memset(players, 0, sizeof(players));
}

void handle_join(int id) {
    if (id < 0 || id >= MAX_PLAYERS) return;
    players[id].active = 1;
    players[id].x = 0;
    players[id].y = 0;
    players[id].health = 100;
    players[id].score = 0;
    snprintf(last_action, sizeof(last_action), "Player %d joined.", id);
}

void handle_move(int id, int x, int y) {
    if (id < 0 || id >= MAX_PLAYERS) return;
    players[id].x = x;
    players[id].y = y;
    snprintf(last_action, sizeof(last_action), "Player %d moved to (%d,%d).", id, x, y);
}

void handle_hit(int from, int to, int hp) {
    if (to < 0 || to >= MAX_PLAYERS) return;
    players[to].health = hp;
    snprintf(last_action, sizeof(last_action), "Player %d hit Player %d (HP=%d).", from, to, hp);
}

void handle_kill(int killer, int victim) {
    if (killer < 0 || killer >= MAX_PLAYERS || victim < 0 || victim >= MAX_PLAYERS) return;
    players[victim].active = 0;
    players[killer].score += 1;
    snprintf(last_action, sizeof(last_action), "Player %d killed Player %d.", killer, victim);
}

void handle_miss(int from, char *dir) {
    snprintf(last_action, sizeof(last_action), "Player %d shot %s and missed.", from, dir);
}

void handle_quit(int id) {
    if (id < 0 || id >= MAX_PLAYERS) return;
    players[id].active = 0;
    snprintf(last_action, sizeof(last_action), "Player %d quit.", id);
}

void handle_respawn(int id) {
    if (id < 0 || id >= MAX_PLAYERS) return;
    players[id].health = 100;
    players[id].x = 0;
    players[id].y = 0;
    snprintf(last_action, sizeof(last_action), "Player %d respawned.", id);
}

void process_line(char *line) {
    char *event = strstr(line, "] ");
    if (!event) {
        // No timestamp prefix — strncat'd continuation event (e.g. KILL after HIT).
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "EVENT:KILL", 10) == 0) {
            int killer, victim;
            if (sscanf(line, "EVENT:KILL;KILLER:%d;VICTIM:%d", &killer, &victim) == 2)
                handle_kill(killer, victim);
        }
        return;
    }
    event += 2;

    char *seq_ptr = strstr(event, "SEQ:");
    if (seq_ptr == event) {
        char *semicolon = strchr(event, ';');
        if (semicolon) {
            event = semicolon + 1;
        }
    }

    if (strncmp(event, "EVENT:SERVER_START", 18) == 0) {
        init_players();
        snprintf(last_action, sizeof(last_action), "SERVER RESTARTED");
    } else if (strncmp(event, "EVENT:JOIN", 10) == 0) {
        int id;
        sscanf(event, "EVENT:JOIN;ID:%d", &id);
        handle_join(id);
    } else if (strncmp(event, "EVENT:RESPAWN", 13) == 0) {
        int id;
        sscanf(event, "EVENT:RESPAWN;ID:%d", &id);
        handle_respawn(id);
    } else if (strncmp(event, "EVENT:MOVE", 10) == 0) {
        int id, x, y;
        sscanf(event, "EVENT:MOVE;ID:%d;POS:(%d,%d)", &id, &x, &y);
        handle_move(id, x, y);
    } else if (strncmp(event, "EVENT:HIT", 9) == 0) {
        int from, to, hp;
        sscanf(event, "EVENT:HIT;FROM:%d;TO:%d;HP:%d", &from, &to, &hp);
        handle_hit(from, to, hp);
    } else if (strncmp(event, "EVENT:KILL", 10) == 0) {
        int killer, victim;
        sscanf(event, "EVENT:KILL;KILLER:%d;VICTIM:%d", &killer, &victim);
        handle_kill(killer, victim);
    } else if (strncmp(event, "EVENT:MISS", 10) == 0) {
        int from;
        char dir[16] = {0};
        sscanf(event, "EVENT:MISS;FROM:%d;DIR:%15s", &from, dir);
        handle_miss(from, dir);
    } else if (strncmp(event, "EVENT:QUIT", 10) == 0) {
        int id;
        sscanf(event, "EVENT:QUIT;ID:%d", &id);
        handle_quit(id);
    }
}

void draw_grid() {
    printf("\033[2J\033[H");
    printf("=== SHADOW-GRID REPLAY ===\n");
    printf("Last Action: %s\n", last_action);
    printf("--------------------------\n");
    
    for (int i = 0; i < MAP_H; i++) {
        for (int j = 0; j < MAP_W; j++) {
            int found = -1;
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (players[p].active && players[p].health > 0 && players[p].x == i && players[p].y == j) {
                    found = p;
                    break;
                }
            }
            if (found != -1) {
                printf("%d ", found % 10);
            } else {
                printf(". ");
            }
        }
        printf("\n");
    }
    printf("--------------------------\n");
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (players[p].active) {
            printf("Player %d: HP=%d, Score=%d, Pos=(%d,%d)\n", p, players[p].health, players[p].score, players[p].x, players[p].y);
        }
    }
    fflush(stdout);
}

int main() {
    FILE *fp = fopen("game.log", "r");
    if (!fp) {
        perror("game.log");
        return 1;
    }

    init_players();

    long last_start_pos = 0;
    char line[512];
    long current_pos = ftell(fp);
    
    while (fgets(line, sizeof(line), fp)) {
        char *event = strstr(line, "] ");
        if (event) {
            event += 2;
            char *seq_ptr = strstr(event, "SEQ:");
            if (seq_ptr == event) {
                char *semicolon = strchr(event, ';');
                if (semicolon) event = semicolon + 1;
            }
            if (strncmp(event, "EVENT:SERVER_START", 18) == 0) {
                last_start_pos = current_pos;
            }
        }
        current_pos = ftell(fp);
    }
    
    fseek(fp, last_start_pos, SEEK_SET);

    while (fgets(line, sizeof(line), fp)) {
        process_line(line);
        draw_grid();
        usleep(300000);
    }

    fclose(fp);
    return 0;
}
