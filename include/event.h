#ifndef EVENT_H
#define EVENT_H

#include "protocol.h"

typedef struct {
    int type;
    char dir[16];
} Event;

Event parse_event(const char *buffer);

#endif