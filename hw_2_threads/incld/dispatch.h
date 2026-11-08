#ifndef DISPATCH_H
#define DISPATCH_H

#include "model.h"

int travel_time(const Simulation *sim, const Team *team, int target);

int team_can_serve(const Team *team, const Call *call);

int choose_team(Simulation *sim, const Call *call);

/**
 * @struct Assignment
 * @brief Decision of the dispatcher: team index -> call index
 */
typedef struct
{
    int team;
    int call;
} Assignment;

int plan_dispatch(Simulation *sim, Assignment *out, int max);

#endif