#ifndef EVENT_H
#define EVENT_H

#include "protocol.h"

typedef struct {
    int type;
    char dir[16];
    char pass[32];
    int target_id;
    int targets[10];
    int target_count;
} Event;

Event parse_event(const char *buffer);

#endif