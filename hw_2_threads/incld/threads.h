#ifndef THREADS_H
#define THREADS_H

#include "model.h"

#define ENTITY_BOX_SIZE 16 // mailbox of a call or a team
#define DISPATCHER_BOX_SIZE (2 * MAX_CALLS + MAX_TEAMS + 16) // mailbox of the dispatcher

// Initializes all synchronization objects required by the simulation threads
int init_thread_sync(Simulation *sim);

// Destroys all synchronization objects and releases associated resources
void destroy_thread_sync(Simulation *sim);

// Blocks signals that will be handled by the dedicated signal thread
int block_signals(void);

// Creates and starts the dispatcher, signal, and team threads
int start_service_threads(Simulation *sim);

// Creates and starts a separate thread for the specified call
int start_call_thread(Simulation *sim, int call_idx);

// Waits for the specified call thread to finish
int join_call_thread(Simulation *sim, int call_idx);

// Requests termination of all threads and waits for their completion
void stop_all_threads(Simulation *sim);

// Sends a message to a mailbox and updates the pending operation counter
void send_message(Simulation *sim, Mailbox *box, MsgType type, int event, int arg);

#endif
