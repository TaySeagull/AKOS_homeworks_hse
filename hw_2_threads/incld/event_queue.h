#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H
#include "model.h"

int event_less(const Event *a, const Event *b);

int push_event(Simulation *sim, int time, EventType type, int id);

Event pop_event(Simulation *sim);

// removes all events related to the given call_id (escalation, cancel, wait_timeout)
int remove_call_deadlines(Simulation *sim, int call_id);

// removes all call arrival events from the event queue
int remove_call_arrivals(Simulation *sim);

#endif
