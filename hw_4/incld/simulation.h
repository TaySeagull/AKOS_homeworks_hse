#ifndef SIMULATION_H
#define SIMULATION_H
#include "model.h"
#include <limits.h>

#define GENERATED_CALL 0

int seed_events(Simulation *sim);

int check_invariants(Simulation *sim);

int run_simulation(Simulation *sim);

int schedule_event_checked(Simulation *sim, int time, EventType type, int id);

#endif
