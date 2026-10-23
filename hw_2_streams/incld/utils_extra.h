#ifndef UTILS_EXTRA_H
#define UTILS_EXTRA_H
#include "model.h"

const char *spec_name(Specialization spec);

const char *call_state_name(CallState state);

const char *team_state_name(TeamState state);

const char *strategy_name(Strategy strategy);

int parse_spec(const char *text);

int district_index(const Simulation *sim, const char *name);

int find_call_index_by_id(const Simulation *sim, int call_id);

int find_team_index_by_id(const Simulation *sim, int team_id);

int log_event(Simulation *sim, const char *format, ...);

int sleep_visual(const Simulation *sim);

#endif
