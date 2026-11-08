#ifndef DISPATCH_H
#define DISPATCH_H

#include "model.h"

int travel_time(const Simulation *sim, const Team *team, int target);

int team_can_serve(const Team *team, const Call *call);

int choose_team(Simulation *sim, const Call *call);

int try_dispatch(Simulation *sim);

#endif