#ifndef SEMS_H
#define SEMS_H

#include <semaphore.h>
#include "model.h"

#define BOX_IDX_DISPATCHER 0
#define BOX_IDX_TEAM0 1                          // team index
#define BOX_IDX_CALLERS (1 + MAX_TEAMS)          // family of calls (independent version)
#define BOX_IDX_CALL0 (2 + MAX_TEAMS)            // call index (fork versions)
#define BOX_COUNT (2 + MAX_TEAMS + MAX_CALLS)

/**
 * @enum SemId
 * @brief Numbers of the semaphores
 */
typedef enum
{
    SEM_WORLD = 0,        // binary: lock of the shared model
    SEM_OUTPUT,           // binary: lock of the console and the log file
    SEM_QUIET_LOCK,       // binary: lock of the counter of unhandled messages
    SEM_QUIET_IDLE,       // posted when the counter of unhandled messages drops to 0
    SEM_READY,            // counting: client processes that finished registration
    SEM_EXITED,           // counting: client processes that finished their work
    SEM_SENT,             // counting: messages sent so far
    SEM_CLAIM_DISPATCHER, // 1 = the role of the dispatcher is free
    SEM_CLAIM_CALLERS,    // 1 = the role of the callers family is free
    SEM_CLAIM_TEAM0,      // + team index: 1 = the team is not played by a process yet
    SEM_BOX0 = SEM_CLAIM_TEAM0 + MAX_TEAMS, // 3 per mailbox: lock, filled, free
    SEM_TOTAL = SEM_BOX0 + 3 * BOX_COUNT
} SemId;

#define SEM_BOX_LOCK(box) (SEM_BOX0 + 3 * (box))
#define SEM_BOX_FILLED(box) (SEM_BOX0 + 3 * (box) + 1)
#define SEM_BOX_FREE(box) (SEM_BOX0 + 3 * (box) + 2)

// Sets the prefix of the names
void sems_setup(const char *prefix, sem_t *shared_array);

// Creates a semaphore with the initial value
int sems_create(int id, unsigned value);

// Returns the semaphore
sem_t *sem_ref(int id);

// Checks if the semaphore exists
int sems_exist(int id);

// Destroys / unlinks all semaphores created by this process
void sems_destroy_all(void);

// Name of the IPC object for the report
const char *sems_kind(void);

// Waits for a semaphore, repeats after EINTR
int sem_wait_retry(sem_t *sem);

// Waits with a timeout in milliseconds
int sem_wait_ms(sem_t *sem, int ms);

#endif
