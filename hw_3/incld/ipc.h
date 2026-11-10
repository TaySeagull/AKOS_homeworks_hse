#ifndef IPC_H
#define IPC_H

#include "message.h"
#include "model.h"

#define ENTITY_BOX_SIZE 16                                   // mailbox of a team or of one call
#define DISPATCHER_BOX_SIZE (2 * MAX_CALLS + MAX_TEAMS + 16) // mailbox of the dispatcher
#define CALLERS_BOX_SIZE (4 * MAX_CALLS + 16)                // mailbox of the "callers" family
#define SESSION_LEN 32                                       // max length of a session name

// Creates all IPC objects, copies the loaded model into them, returns the model to work with
Simulation *ipc_server_init(const Simulation *loaded, const char *session);

// Remembers the path of the configuration (sent to the clients of the message-queue version)
void ipc_remember_config(const char *path);

// Starts (fork versions) or waits for (independent versions) the dispatcher and team processes
int ipc_start_processes(Simulation *sim);

// Starts the process that plays a call (fork) or marks it as played by the "callers" family
int ipc_start_call(Simulation *sim, int call_idx);

// Stops the process of a closed call before its slot is reused by the random generator
int ipc_stop_call(Simulation *sim, int call_idx);

// Sends MSG_SHUTDOWN to all processes and waits for them, collects their statistics
void ipc_stop_all(Simulation *sim);

// Removes all IPC objects (shared memory, semaphores, queues); safe to call more than once
void ipc_server_cleanup(void);

// Quiet counter: the clock waits until every sent message has been handled
void ipc_quiet_hold(Simulation *sim);
void ipc_quiet_release(Simulation *sim);

// Returns 1 when all messages are handled, 0 if a process died while waiting
int ipc_quiet_wait(Simulation *sim);

// Name of the IPC scheme (printed in the header and in the summary)
const char *ipc_flavour(void);

// How the calls are played: "1 process per call" or "1 callers process"
const char *ipc_calls_layout(void);

// Attaches to the IPC objects of the session and claims a role; *idx is the team index
Simulation *ipc_client_attach(Role role, int *idx, const char *session, int wait_seconds);

// Detaches from the IPC objects, sends the final statistics
void ipc_client_detach(Simulation *sim);

// Process-shared lock of the model
void world_lock(Simulation *sim);
void world_unlock(Simulation *sim);

// Process-shared lock of the console and the log file
void ipc_output_lock(void);
void ipc_output_unlock(void);

// Sends a message that is counted by the quiet counter 
int ipc_send(Simulation *sim, BoxKind kind, int idx, Message *message);

// Short form of ipc_send() without attachments
int ipc_send_simple(Simulation *sim, BoxKind kind, int idx, MsgType type, int event, int arg, int value);

// Receives the next message of the calling process
Message ipc_receive(Simulation *sim);

// Must be called after a counted message has been handled completely
void ipc_done(Simulation *sim);

// Schedules an event
int ipc_schedule(Simulation *sim, int time, EventType type, int id);

// Removes the deadlines of a call from the event queue
int ipc_remove_deadlines(Simulation *sim, int call_id);

// Sends the new state of a call / team record to the mirrors of the other processes
void ipc_publish_call(Simulation *sim, int call_idx);
void ipc_publish_team(Simulation *sim, int team_idx);

// Applies the attachments of a received message to the mirror
void ipc_apply(Simulation *sim, const Message *message);

// Role and index of the calling process
void ipc_set_self(Role role, int idx);
Role ipc_self_role(void);
int ipc_self_idx(void);

#endif
