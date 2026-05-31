#ifndef STATE_H
#define STATE_H

#include <stddef.h>
#include <pthread.h>
#include "common.h"

#define MAX_PLAYERS 100
#define MAP_W 20
#define MAP_H 20

typedef struct {
    int id;
    int active;
    int x, y;
    int health;
    int score;
    int role;
    int squad_id;
} Player;

extern Player players[MAX_PLAYERS];
extern pthread_mutex_t player_locks[MAX_PLAYERS];
extern int WFG[MAX_PLAYERS][MAX_PLAYERS];
extern int deadlock_abort[MAX_PLAYERS];

int count_active_players();
int register_player();
void remove_player(int id);
void apply_move(int id, const char *dir);
int find_target_in_direction(int shooter_id, const char *dir);
void build_status(int id, char *out, size_t n);
void build_snapshot(char *out, size_t n);
void init_locks();

#endif