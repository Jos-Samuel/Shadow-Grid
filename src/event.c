#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "event.h"

Event parse_event(const char *buffer) {
    Event e;
    e.type = EVENT_UNKNOWN;
    memset(e.dir, 0, sizeof(e.dir));
    memset(e.pass, 0, sizeof(e.pass));
    e.target_id = -1;
    e.target_count = 0;

    if (strncmp(buffer, "TYPE:JOIN", 9) == 0) {
        e.type = EVENT_JOIN;
    } else if (strncmp(buffer, "TYPE:MOVE", 9) == 0) {
        e.type = EVENT_MOVE;
        const char *dir_ptr = strstr(buffer, "DIR:");
        if (dir_ptr) sscanf(dir_ptr, "DIR:%15[^;\r\n]", e.dir);
    } else if (strncmp(buffer, "TYPE:SHOOT", 10) == 0) {
        e.type = EVENT_SHOOT;
        const char *dir_ptr = strstr(buffer, "DIR:");
        if (dir_ptr) sscanf(dir_ptr, "DIR:%15[^;\r\n]", e.dir);
    } else if (strncmp(buffer, "TYPE:ROLE", 9) == 0) {
        e.type = EVENT_ROLE;
        const char *role_ptr = strstr(buffer, "SET:");
        if (role_ptr) sscanf(role_ptr, "SET:%15[^;\r\n]", e.dir);
        const char *pass_ptr = strstr(buffer, "PASS:");
        if (pass_ptr) sscanf(pass_ptr, "PASS:%31[^;\r\n]", e.pass);
    } else if (strncmp(buffer, "TYPE:STATUS", 11) == 0) {
        e.type = EVENT_STATUS;
    } else if (strncmp(buffer, "TYPE:RESPAWN", 12) == 0) {
        e.type = EVENT_RESPAWN;
    } else if (strncmp(buffer, "TYPE:QUIT", 9) == 0) {
        e.type = EVENT_QUIT;
    } else if (strncmp(buffer, "TYPE:KICK", 9) == 0) {
        e.type = EVENT_KICK;
        const char *tgt_ptr = strstr(buffer, "TARGET:");
        if (tgt_ptr) sscanf(tgt_ptr, "TARGET:%d", &e.target_id);
    } else if (strncmp(buffer, "TYPE:HEAL_ALL", 13) == 0) {
        e.type = EVENT_HEAL_ALL;
    } else if (strncmp(buffer, "TYPE:SMITE", 10) == 0) {
        e.type = EVENT_SMITE;
        const char *tgt_ptr = strstr(buffer, "TARGET:");
        if (tgt_ptr) sscanf(tgt_ptr, "TARGET:%d", &e.target_id);
    } else if (strncmp(buffer, "TYPE:CREATE_SQUAD", 17) == 0) {
        e.type = EVENT_CREATE_SQUAD;
        const char *tgt_ptr = strstr(buffer, "TARGETS:");
        if (tgt_ptr) {
            char tgt_str[64] = {0};
            sscanf(tgt_ptr, "TARGETS:%63[^;\r\n]", tgt_str);
            char *token = strtok(tgt_str, ",");
            while (token && e.target_count < 10) {
                e.targets[e.target_count++] = atoi(token);
                token = strtok(NULL, ",");
            }
        }
    } else if (strncmp(buffer, "TYPE:INVITE", 11) == 0) {
        e.type = EVENT_INVITE;
        const char *tgt_ptr = strstr(buffer, "TARGET:");
        if (tgt_ptr) sscanf(tgt_ptr, "TARGET:%d", &e.target_id);
    }

    return e;
}
