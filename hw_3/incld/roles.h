#ifndef ROLES_H
#define ROLES_H

#include "message.h"
#include "model.h"

// Main loop of a dispatcher, team, call or callers process
int role_loop(Simulation *sim, Role role, int idx);

#endif
