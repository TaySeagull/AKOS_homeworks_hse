#ifndef SIMULATION_H
#define SIMULATION_H
#include "model.h"
#include <limits.h>

#define GENERATED_CALL 0 // id of an EV_CALL_ARRIVAL event that must create a random call

int cancel_or_unserve_expired(Simulation *sim);

int seed_events(Simulation *sim);

// checks the invariants of the simulation and returns 1 if they are valid, 0 otherwise
int check_invariants(Simulation *sim);

int run_simulation(Simulation *sim);

int install_signal(void);

// returns 1 if a stop is requested (SIGINT or SIGQUIT), 0 otherwise
int stop_is_requested(void);

int schedule_event_checked(Simulation *sim, int time, EventType type, int id);

#endif
