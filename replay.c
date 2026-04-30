#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PLAYERS 100

typedef struct {
    int active;
    int x, y;
    int health;
    int score;
} Player;

Player players[MAX_PLAYERS];

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

    printf("JOIN: Player %d\n", id);
}

void handle_move(int id, int x, int y) {
    if (id < 0 || id >= MAX_PLAYERS) return;
    players[id].x = x;
    players[id].y = y;

    printf("MOVE: Player %d -> (%d,%d)\n", id, x, y);
}

void handle_hit(int from, int to, int hp) {
    if (to < 0 || to >= MAX_PLAYERS) return;
    players[to].health = hp;

    printf("HIT: %d -> %d (HP=%d)\n", from, to, hp);
}

void handle_kill(int killer, int victim) {
    if (killer < 0 || killer >= MAX_PLAYERS || victim < 0 || victim >= MAX_PLAYERS) return;
    players[victim].active = 0;
    players[killer].score += 1;

    printf("KILL: %d killed %d\n", killer, victim);
}

void handle_miss(int from, char *dir) {
    printf("MISS: %d -> %s\n", from, dir);
}

void handle_quit(int id) {
    if (id < 0 || id >= MAX_PLAYERS) return;
    players[id].active = 0;

    printf("QUIT: Player %d\n", id);
}

void handle_respawn(int id) {
    if (id < 0 || id >= MAX_PLAYERS) return;
    players[id].health = 100;
    players[id].x = 0;
    players[id].y = 0;
    printf("RESPAWN: Player %d\n", id);
}

void process_line(char *line) {

    // skip timestamp prefix: [YYYY-MM-DD HH:MM:SS]
    char *event = strstr(line, "] ");
    if (!event) return;
    event += 2;

    // 🔹 NEW: Skip optional SEQ:N; prefix added in Phase 12
    char *seq_ptr = strstr(event, "SEQ:");
    if (seq_ptr == event) {
        char *semicolon = strchr(event, ';');
        if (semicolon) {
            event = semicolon + 1;
        }
    }

    if (strncmp(event, "EVENT:SERVER_START", 18) == 0) {
        init_players();
        printf("--- SERVER RESTARTED ---\n");

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
        char dir[16];
        sscanf(event, "EVENT:MISS;FROM:%d;DIR:%s", &from, dir);
        handle_miss(from, dir);

    } else if (strncmp(event, "EVENT:QUIT", 10) == 0) {
        int id;
        sscanf(event, "EVENT:QUIT;ID:%d", &id);
        handle_quit(id);
    }
}

int main() {
    FILE *fp = fopen("game.log", "r");
    if (!fp) {
        perror("game.log");
        return 1;
    }

    init_players();

    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        process_line(line);

        // optional delay for replay visualization
        usleep(300000);
    }

    fclose(fp);
    return 0;
}
