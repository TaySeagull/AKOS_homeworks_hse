#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H
#include "model.h"

int event_less(const Event *a, const Event *b);

int push_event(Simulation *sim, int time, EventType type, int id);

Event pop_event(Simulation *sim);

int remove_call_deadlines(Simulation *sim, int call_id);

int remove_call_arrivals(Simulation *sim);

#endif
