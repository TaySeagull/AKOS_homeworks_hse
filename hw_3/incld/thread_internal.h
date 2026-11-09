#ifndef THREAD_INTERNAL_H
#define THREAD_INTERNAL_H

#include "threads.h"

/**
 * @struct ThreadArgs
 * @brief Arguments passed to a call or team thread.
 */
typedef struct
{
    Simulation *sim;
    int index;
} ThreadArgs;

// Thread functions for call
void *call_thread(void *raw);

//Thread function for a team thread
void *team_thread(void *raw);

// Thread functions for dispatcher
void *dispatcher_thread(void *raw);

// Thread function for signal handling
void *signal_thread(void *raw);

#endif
