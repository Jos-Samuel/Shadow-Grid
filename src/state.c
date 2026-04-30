#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "state.h"
#include "protocol.h"
#include "common.h"

Player players[MAX_PLAYERS];

int count_active_players() {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active) count++;
    }
    return count;
}

int register_player() {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].active) {
            players[i].active = 1;
            players[i].id = i;
            players[i].x = 0;
            players[i].y = 0;
            players[i].health = 100;
            players[i].score = 0;
            players[i].role = ROLE_PLAYER;
            return i;
        }
    }
    return -1;
}

void remove_player(int id) {
    if (id >= 0 && id < MAX_PLAYERS) {
        players[id].active = 0;
    }
}

void apply_move(int id, const char *dir) {
    if (strcasecmp(dir, "UP") == 0) players[id].x--;
    else if (strcasecmp(dir, "DOWN") == 0) players[id].x++;
    else if (strcasecmp(dir, "LEFT") == 0) players[id].y--;
    else if (strcasecmp(dir, "RIGHT") == 0) players[id].y++;

    if (players[id].x < 0) players[id].x = 0;
    if (players[id].y < 0) players[id].y = 0;
    if (players[id].x >= MAP_H) players[id].x = MAP_H - 1;
    if (players[id].y >= MAP_W) players[id].y = MAP_W - 1;
}

int find_target_in_direction(int shooter_id, const char *dir) {
    int sx = players[shooter_id].x;
    int sy = players[shooter_id].y;

    int best_id = -1;
    int best_dist = 1000000;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == shooter_id) continue;
        if (!players[i].active || players[i].health <= 0) continue;

        if (strcasecmp(dir, "UP") == 0) {
            if (players[i].y == sy && players[i].x < sx) {
                int dist = sx - players[i].x;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_id = i;
                }
            }
        } else if (strcasecmp(dir, "DOWN") == 0) {
            if (players[i].y == sy && players[i].x > sx) {
                int dist = players[i].x - sx;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_id = i;
                }
            }
        } else if (strcasecmp(dir, "LEFT") == 0) {
            if (players[i].x == sx && players[i].y < sy) {
                int dist = sy - players[i].y;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_id = i;
                }
            }
        } else if (strcasecmp(dir, "RIGHT") == 0) {
            if (players[i].x == sx && players[i].y > sy) {
                int dist = players[i].y - sy;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_id = i;
                }
            }
        }
    }

    return best_id;
}

void build_status(int id, char *out, size_t n) {
    snprintf(out, n,
             "RESP:OK;MSG:STATUS;ACTIVE:%d;YOU:(%d,%d);HP:%d;SCORE:%d\n",
             count_active_players(),
             players[id].x,
             players[id].y,
             players[id].health,
             players[id].score);
}

void build_snapshot(char *out, size_t n) {
    int offset = 0;
    offset += snprintf(out + offset, n - offset, "STATE:");

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active) {
            int written = snprintf(out + offset, n - offset,
                "ID:%d;POS:(%d,%d);HP:%d;SCORE:%d|",
                i, players[i].x, players[i].y, players[i].health, players[i].score);
            
            if (written > 0 && written < (int)(n - offset)) {
                offset += written;
            } else {
                break;
            }
        }
    }

    if (offset < (int)n) {
        snprintf(out + offset, n - offset, "\n");
    }
}
