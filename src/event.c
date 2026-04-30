#include <stdio.h>
#include <string.h>
#include "event.h"

Event parse_event(const char *buffer) {
    Event e;
    e.type = EVENT_UNKNOWN;
    memset(e.dir, 0, sizeof(e.dir));

    if (strncmp(buffer, "TYPE:JOIN", 9) == 0) {
        e.type = EVENT_JOIN;
    } else if (strncmp(buffer, "TYPE:MOVE", 9) == 0) {
        e.type = EVENT_MOVE;
        const char *dir_ptr = strstr(buffer, "DIR:");
        if (dir_ptr) sscanf(dir_ptr, "DIR:%15s", e.dir);
    } else if (strncmp(buffer, "TYPE:SHOOT", 10) == 0) {
        e.type = EVENT_SHOOT;
        const char *dir_ptr = strstr(buffer, "DIR:");
        if (dir_ptr) sscanf(dir_ptr, "DIR:%15s", e.dir);
    } else if (strncmp(buffer, "TYPE:ROLE", 9) == 0) {
        e.type = EVENT_ROLE;
        const char *role_ptr = strstr(buffer, "SET:");
        if (role_ptr) sscanf(role_ptr, "SET:%15s", e.dir);
    } else if (strncmp(buffer, "TYPE:STATUS", 11) == 0) {
        e.type = EVENT_STATUS;
    } else if (strncmp(buffer, "TYPE:RESPAWN", 12) == 0) {
        e.type = EVENT_RESPAWN;
    } else if (strncmp(buffer, "TYPE:QUIT", 9) == 0) {
        e.type = EVENT_QUIT;
    }

    return e;
}
