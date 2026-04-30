#include "auth.h"
#include "protocol.h"

int can_move(int role) {
    return (role == ROLE_PLAYER || role == ROLE_ADMIN);
}

int can_shoot(int role) {
    return (role == ROLE_PLAYER || role == ROLE_ADMIN);
}
