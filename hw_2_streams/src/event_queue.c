#include "event_queue.h"
#include "utils.h"

/**
 * @brief Compares two events based on their time and sequence number
 * @param one Pointer to the first event
 * @param two Pointer to the second event
 * @return 1 if the first event should come before the second, 0 otherwise
 */
int event_less(const Event *one, const Event *two)
{
    SOFT_ASSERT(one, "first event is NULL", 0);
    SOFT_ASSERT(two, "second event is NULL", 0);

    if (one->time != two->time)
        return one->time < two->time;

    return one->seq < two->seq;
}

/**
 * @brief Pushes an event to the event queue
 * @param sim Pointer to the simulation structure
 * @param time The time at which the event occurs
 * @param type The type of the event
 * @param id The ID of the event
 * @return 1 on success, 0 on failure
 */
int push_event(Simulation *sim, int time, EventType type, int id)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(time >= 0, "event time must be non-negative", 0);
    SOFT_ASSERT(type >= EV_CALL_ARRIVAL && type <= EV_CANCEL, "invalid event type", 0);
    SOFT_ASSERT(sim->event_count < MAX_EVENTS, "event queue is full", 0);

    Event event = {time, type, id, sim->next_seq++};
    int idx = sim->event_count++;
    sim->events[idx] = event;

    while (idx > 0)
    {
        int parent_event = (idx - 1) / 2;

        // Compare the current event with its parent
        if (!event_less(&sim->events[idx], &sim->events[parent_event]))
            break;

        Event temp = sim->events[idx]; // Store the current event temporarily

        // Swap the current event with its parent
        sim->events[idx] = sim->events[parent_event];
        sim->events[parent_event] = temp;

        idx = parent_event;
    }

    return 1;
}

/**
 * @brief Pops the event with the earliest time from the event queue
 * @param sim Pointer to the simulation structure
 * @return The event with the earliest time, or a default event if the queue is empty
 */
Event pop_event(Simulation *sim)
{
    Event error_event = {0, EV_CALL_ARRIVAL, 0, 0}; // Default event to return in case of an error

    SOFT_ASSERT(sim, "simulation is NULL", error_event);
    SOFT_ASSERT(sim->event_count > 0, "cannot pop from empty event queue", error_event);

    Event top_event = sim->events[0];

    // Move the last event to the top of the heap and reduce the event count
    sim->events[0] = sim->events[--sim->event_count];

    // Now we need to sift down the new top event cause it was the last event

    int idx = 0;
    for (;;) // infinite loop
    {
        int left = idx * 2 + 1;
        int right = left + 1;
        int b = idx;

        if (left < sim->event_count && event_less(&sim->events[left], &sim->events[b]))
            b = left;

        if (right < sim->event_count && event_less(&sim->events[right], &sim->events[b]))
            b = right;

        if (b == idx)
            break;

        Event temp = sim->events[idx]; // Store the current event temporarily
        // Swap the current event with the smaller child

        sim->events[idx] = sim->events[b];
        sim->events[b] = temp;

        idx = b;
    }

    return top_event;
}
