#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MAX_PLAYERS 100
#define MAP_H 20
#define MAP_W 20

typedef struct {
    int id;
    int active;
    int x, y;
    int health;
    int score;
    int role;
    int squad_id;
} Player;

Player players[MAX_PLAYERS];

// Maintain a snapshot of state before each SEQ to allow undo calculation
#define MAX_LOGS 10000
Player state_history[MAX_LOGS][MAX_PLAYERS];
char log_lines[MAX_LOGS][256];
int log_seqs[MAX_LOGS];
int log_count = 0;

int current_lsn = 0;

void init_players() {
    memset(players, 0, sizeof(players));
}

void apply_event(char *event) {
    if (strncmp(event, "EVENT:SERVER_START", 18) == 0) {
        init_players();
    } else if (strncmp(event, "EVENT:JOIN", 10) == 0) {
        int id;
        if (sscanf(event, "EVENT:JOIN;ID:%d", &id) == 1) {
            players[id].active = 1;
            players[id].x = 0; players[id].y = 0;
            players[id].health = 100; players[id].score = 0;
        }
    } else if (strncmp(event, "EVENT:RESPAWN", 13) == 0) {
        int id;
        if (sscanf(event, "EVENT:RESPAWN;ID:%d", &id) == 1) {
            players[id].health = 100; players[id].x = 0; players[id].y = 0;
        }
    } else if (strncmp(event, "EVENT:MOVE", 10) == 0) {
        int id, x, y;
        if (sscanf(event, "EVENT:MOVE;ID:%d;POS:(%d,%d)", &id, &x, &y) == 3) {
            players[id].x = x; players[id].y = y;
        }
    } else if (strncmp(event, "EVENT:HIT", 9) == 0) {
        int from, to, hp;
        if (sscanf(event, "EVENT:HIT;FROM:%d;TO:%d;HP:%d", &from, &to, &hp) == 3) {
            players[to].health = hp;
        }
    } else if (strncmp(event, "EVENT:KILL", 10) == 0) {
        int killer, victim;
        if (sscanf(event, "EVENT:KILL;KILLER:%d;VICTIM:%d", &killer, &victim) == 2) {
            players[victim].active = 0;
            players[killer].score += 1;
        }
    } else if (strncmp(event, "EVENT:QUIT", 10) == 0) {
        int id;
        if (sscanf(event, "EVENT:QUIT;ID:%d", &id) == 1) {
            players[id].active = 0;
        }
    } else if (strncmp(event, "EVENT:SMITE", 11) == 0) {
        // SMITE kills instantly; its accompanying KILL line has no timestamp prefix
        // so build_history cannot parse it — absorb both effects here.
        int by, target;
        if (sscanf(event, "EVENT:SMITE;BY:%d;TARGET:%d", &by, &target) == 2) {
            players[target].health = 0;
            players[target].active = 0;
            players[by].score += 1;
        }
    } else if (strncmp(event, "EVENT:HEAL_ALL", 14) == 0) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].active && players[i].health > 0) players[i].health = 100;
        }
    } else if (strncmp(event, "EVENT:KICK", 10) == 0) {
        int by, target;
        if (sscanf(event, "EVENT:KICK;BY:%d;TARGET:%d", &by, &target) == 2) {
            players[target].active = 0;
        }
    }
}

void load_checkpoint(int *cp_lsn) {
    FILE *fp = fopen("game_state.dat", "rb");
    if (fp) {
        fread(cp_lsn, sizeof(int), 1, fp);
        fread(players, sizeof(Player), MAX_PLAYERS, fp);
        fclose(fp);
        printf("Loaded checkpoint at LSN: %d\n", *cp_lsn);
    } else {
        *cp_lsn = 0;
        init_players();
        printf("No checkpoint found. Starting from scratch.\n");
    }
}

void save_checkpoint(int cp_lsn) {
    FILE *fp = fopen("game_state.dat", "wb");
    if (fp) {
        fwrite(&cp_lsn, sizeof(int), 1, fp);
        fwrite(players, sizeof(Player), MAX_PLAYERS, fp);
        fclose(fp);
        printf("Checkpoint saved at LSN: %d\n", cp_lsn);
    }
}

void build_history(int start_lsn) {
    (void)start_lsn;
    FILE *fp = fopen("wal.log", "r");
    if (!fp) {
        printf("Could not open wal.log\n");
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *event = strstr(line, "] ");
        if (!event) {
            // No timestamp prefix — this is a strncat'd continuation event (e.g. KILL after HIT).
            // Apply it to advance state but do not assign a seq or store in log_lines.
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] != '\0') apply_event(line);
            continue;
        }
        event += 2;
        
        int seq = 0;
        char *seq_ptr = strstr(event, "SEQ:");
        if (seq_ptr == event) {
            sscanf(event, "SEQ:%d", &seq);
            char *semicolon = strchr(event, ';');
            if (semicolon) event = semicolon + 1;
        }
        
        if (strncmp(event, "EVENT:SERVER_START", 18) == 0) {
            init_players();
            current_lsn = 0;
            // Reset all history arrays so SEQ numbers from the previous session
            // do not collide with and corrupt entries from this new session.
            log_count = 0;
            memset(log_lines, 0, sizeof(log_lines));
            memset(log_seqs, 0, sizeof(log_seqs));
            memset(state_history, 0, sizeof(state_history));
        }
        
        if (seq > current_lsn) {
            current_lsn = seq;
        }

        if (seq > 0 && seq < MAX_LOGS && log_count < MAX_LOGS) {
            memcpy(state_history[seq], players, sizeof(players));
            strncpy(log_lines[seq], event, sizeof(log_lines[seq]));
            log_seqs[log_count++] = seq;
        }

        apply_event(event);
    }
    fclose(fp);
}

void do_undo(int target_seq) {
    if (target_seq <= 0 || target_seq > current_lsn) {
        printf("Invalid SEQ for undo.\n");
        return;
    }

    char *event = log_lines[target_seq];
    if (strlen(event) == 0) {
        printf("SEQ %d not found in log history.\n", target_seq);
        return;
    }

    printf("Undoing SEQ %d: %s", target_seq, event);

    Player *old_state = state_history[target_seq];

    char clr_msg[256] = "";

    if (strncmp(event, "EVENT:MOVE", 10) == 0) {
        int id, x, y;
        sscanf(event, "EVENT:MOVE;ID:%d;POS:(%d,%d)", &id, &x, &y);
        snprintf(clr_msg, sizeof(clr_msg), "EVENT:MOVE;ID:%d;POS:(%d,%d);CLR:%d\n", 
                 id, old_state[id].x, old_state[id].y, target_seq);
    } else if (strncmp(event, "EVENT:JOIN", 10) == 0) {
        int id;
        sscanf(event, "EVENT:JOIN;ID:%d", &id);
        snprintf(clr_msg, sizeof(clr_msg), "EVENT:QUIT;ID:%d;CLR:%d\n", id, target_seq);
    } else if (strncmp(event, "EVENT:QUIT", 10) == 0) {
        int id;
        sscanf(event, "EVENT:QUIT;ID:%d", &id);
        snprintf(clr_msg, sizeof(clr_msg), "EVENT:JOIN;ID:%d;CLR:%d\n", id, target_seq);
    } else if (strncmp(event, "EVENT:HIT", 9) == 0) {
        int from, to, hp;
        sscanf(event, "EVENT:HIT;FROM:%d;TO:%d;HP:%d", &from, &to, &hp);
        snprintf(clr_msg, sizeof(clr_msg), "EVENT:HIT;FROM:%d;TO:%d;HP:%d;CLR:%d\n", 
                 from, to, old_state[to].health, target_seq);
    } else if (strncmp(event, "EVENT:KILL", 10) == 0) {
        int killer, victim;
        sscanf(event, "EVENT:KILL;KILLER:%d;VICTIM:%d", &killer, &victim);
        snprintf(clr_msg, sizeof(clr_msg), "EVENT:RESPAWN;ID:%d;CLR:%d\n", victim, target_seq);
    } else {
        printf("Undo not supported for this event type.\n");
        return;
    }

    if (clr_msg[0] != '\0') {
        int new_seq = current_lsn + 1;
        FILE *fp = fopen("wal.log", "a");
        if (fp) {
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            char timebuf[64];
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);
            
            fprintf(fp, "[%s] SEQ:%d;%s", timebuf, new_seq, clr_msg);
            current_lsn = new_seq;  // Advance LSN so next CLR gets a unique sequence number
            fclose(fp);
            printf("Appended CLR: SEQ:%d;%s", new_seq, clr_msg);
        }
    }
}

void automatic_recovery() {
    int cp_lsn = 0;
    
    // 1. Build history to populate state_history and log_lines for all LSNs
    build_history(0);
    
    // Save the final LSN before recovery starts
    int crash_lsn = current_lsn;
    
    printf("=== PHASE 1: ANALYSIS ===\n");
    // 2. Load the checkpoint to see the starting state and cp_lsn
    load_checkpoint(&cp_lsn);
    printf("Found CHECKPOINT at LSN: %d\n", cp_lsn);
    
    // Initialize transaction table
    // 0 = inactive, 1 = active (loser), 2 = committed (winner)
    int tx_status[MAX_PLAYERS];
    int started_before_cp[MAX_PLAYERS];
    memset(tx_status, 0, sizeof(tx_status));
    memset(started_before_cp, 0, sizeof(started_before_cp));
    
    // Any player active at the checkpoint started before the checkpoint
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active) {
            started_before_cp[i] = 1;
            tx_status[i] = 1; // Default to active (T3) unless we see a QUIT
        }
    }
    
    // Scan log from cp_lsn + 1 to crash_lsn to identify T1, T2, T3, T4
    for (int seq = cp_lsn + 1; seq <= crash_lsn; seq++) {
        char *event = log_lines[seq];
        if (strlen(event) == 0) continue;
        
        int id = -1;
        if (strncmp(event, "EVENT:JOIN", 10) == 0) {
            if (sscanf(event, "EVENT:JOIN;ID:%d", &id) == 1) {
                started_before_cp[id] = 0;
                tx_status[id] = 1; // Started after checkpoint, active (T4)
            }
        } else if (strncmp(event, "EVENT:QUIT", 10) == 0) {
            if (sscanf(event, "EVENT:QUIT;ID:%d", &id) == 1) {
                tx_status[id] = 2; // Committed (T1 or T2)
            }
        } else if (strncmp(event, "EVENT:MOVE", 10) == 0) {
            sscanf(event, "EVENT:MOVE;ID:%d", &id);
            if (id >= 0 && tx_status[id] == 0) {
                tx_status[id] = 1; // Implicit active
            }
        } else if (strncmp(event, "EVENT:HIT", 9) == 0) {
            int from, to;
            if (sscanf(event, "EVENT:HIT;FROM:%d;TO:%d", &from, &to) == 2) {
                if (tx_status[from] == 0) tx_status[from] = 1;
                if (tx_status[to] == 0) tx_status[to] = 1;
            }
        } else if (strncmp(event, "EVENT:KILL", 10) == 0) {
            int killer, victim;
            if (sscanf(event, "EVENT:KILL;KILLER:%d;VICTIM:%d", &killer, &victim) == 2) {
                if (tx_status[killer] == 0) tx_status[killer] = 1;
                if (tx_status[victim] == 0) tx_status[victim] = 1;
            }
        }
    }
    
    printf("\n--- Classifying Transactions based on Korth Scenarios ---\n");
    int has_tx = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (tx_status[i] > 0 || started_before_cp[i]) {
            has_tx = 1;
            if (started_before_cp[i] && tx_status[i] == 2) {
                printf("T%d (Player %d - Started BEFORE Checkpoint, Committed BEFORE Crash): Action -> Winner, Redo.\n", i, i);
            } else if (!started_before_cp[i] && tx_status[i] == 2) {
                printf("T%d (Player %d - Started AFTER Checkpoint, Committed BEFORE Crash): Action -> Winner, Redo.\n", i, i);
            } else if (started_before_cp[i] && tx_status[i] == 1) {
                printf("T%d (Player %d - Started BEFORE Checkpoint, ACTIVE at Crash): Action -> ROLLBACK Triggered (Undo).\n", i, i);
            } else if (!started_before_cp[i] && tx_status[i] == 1) {
                printf("T%d (Player %d - Started AFTER Checkpoint, ACTIVE at Crash): Action -> ROLLBACK Triggered (Undo).\n", i, i);
            }
        }
    }
    if (!has_tx) {
        printf("No active or committed transactions found.\n");
    }
    printf("=========================\n\n");
    
    printf("=== PHASE 2: REDO (Repeating History) ===\n");
    // Replay log from cp_lsn + 1 to crash_lsn
    for (int seq = cp_lsn + 1; seq <= crash_lsn; seq++) {
        char *event = log_lines[seq];
        if (strlen(event) == 0) continue;
        
        printf("Redoing operation LSN:%d -> %s", seq, event);
        apply_event(event);
    }
    printf("Redo complete. Intermediate LSN: %d\n", crash_lsn);
    printf("=========================================\n\n");
    
    printf("=== PHASE 3: UNDO (Rollback Active Transactions) ===\n");
    // Scan backward to undo active (loser) transactions
    for (int seq = crash_lsn; seq > cp_lsn; seq--) {
        char *event = log_lines[seq];
        if (strlen(event) == 0) continue;
        
        int id = -1;
        if (strncmp(event, "EVENT:MOVE", 10) == 0) {
            sscanf(event, "EVENT:MOVE;ID:%d", &id);
        } else if (strncmp(event, "EVENT:JOIN", 10) == 0) {
            sscanf(event, "EVENT:JOIN;ID:%d", &id);
        } else if (strncmp(event, "EVENT:QUIT", 10) == 0) {
            sscanf(event, "EVENT:QUIT;ID:%d", &id);
        } else if (strncmp(event, "EVENT:HIT", 9) == 0) {
            int from, to;
            sscanf(event, "EVENT:HIT;FROM:%d;TO:%d", &from, &to);
            id = from;
        } else if (strncmp(event, "EVENT:KILL", 10) == 0) {
            int killer, victim;
            sscanf(event, "EVENT:KILL;KILLER:%d;VICTIM:%d", &killer, &victim);
            id = killer;
        }
        
        if (id >= 0 && tx_status[id] == 1) { // It is a loser!
            printf("Undoing LSN:%d for T%d: %s", seq, id, event);
            do_undo(seq);
        }
    }
    
    // Finally abort all active transactions by writing an abort log
    FILE *wal = fopen("wal.log", "a");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (tx_status[i] == 1) {
            int new_seq = current_lsn + 1;
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            char timebuf[64];
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);
            
            if (wal) {
                fprintf(wal, "[%s] SEQ:%d;EVENT:ABORT;ID:%d\n", timebuf, new_seq, i);
                current_lsn = new_seq;
            }
            printf("Transaction T%d (Player %d) is fully rolled back (ABORTED).\n", i, i);
            players[i].active = 0;
        }
    }
    if (wal) fclose(wal);
    printf("====================================================\n\n");
    
    // Save a clean checkpoint after successful recovery
    save_checkpoint(current_lsn);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <checkpoint|redo|undo|recover> [SEQ]\n", argv[0]);
        return 1;
    }

    int cp_lsn = 0;

    if (strcmp(argv[1], "checkpoint") == 0) {
        build_history(0);
        save_checkpoint(current_lsn);
    } else if (strcmp(argv[1], "redo") == 0) {
        load_checkpoint(&cp_lsn);
        build_history(0); 
        printf("Redo complete. Current LSN: %d\n", current_lsn);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].active) {
                printf("Player %d: POS=(%d,%d) HP=%d\n", i, players[i].x, players[i].y, players[i].health);
            }
        }
    } else if (strcmp(argv[1], "undo") == 0) {
        if (argc < 3) {
            printf("Provide SEQ to undo.\n");
            return 1;
        }
        int target_seq = atoi(argv[2]);
        build_history(0);
        do_undo(target_seq);
    } else if (strcmp(argv[1], "recover") == 0) {
        automatic_recovery();
    } else {
        printf("Unknown command.\n");
    }

    return 0;
}
