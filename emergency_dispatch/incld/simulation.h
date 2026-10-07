#ifndef SIMULATION_H
#define SIMULATION_H
#include "model.h"
#include <limits.h>

int travel_time(const Simulation *sim, const Team *team, int target_district);

int cancel_or_unserve_expired(Simulation *sim);

int seed_events(Simulation *sim);

int run_simulation(Simulation *sim);

int install_signal(void);

int schedule_event_checked(Simulation *sim, int time, EventType type, int id);

#endif
