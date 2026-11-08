#ifndef SYNC_H
#define SYNC_H

#include <pthread.h>
#include <semaphore.h>

/**
 * @enum MsgType
 * @brief Kinds of messages that threads send to each other
 */
typedef enum
{
    MSG_WAKE = 0,      // clock -> call/team: an event of this entity has come (Message.event)
    MSG_NEW_CALL,      // call -> dispatcher: the call has arrived and waits (arg = call index)
    MSG_CALL_CHANGED,  // call -> dispatcher: the priority of a waiting call has changed
    MSG_TEAM_FREE,     // team -> dispatcher: the team is available again (arg = team index)
    MSG_ROUND,         // clock -> dispatcher: all reactions of this minute are done, assign teams
    MSG_ASSIGN,        // dispatcher -> team: serve the call (arg = call index)
    MSG_CALL_FINISHED, // team -> call: the help is finished, the call thread can end
    MSG_SHUTDOWN       // clock -> everybody: the simulation is over
} MsgType;

/**
 * @struct Message
 * @brief One message in a mailbox
 */
typedef struct
{
    MsgType type;
    int event; // EventType for MSG_WAKE
    int arg;   // index of a call or a team
} Message;

/**
 * @struct Mailbox
 * @brief Bounded FIFO queue of messages of one thread
 */
typedef struct
{
    Message *items;
    int capacity;
    int head;  // index of the oldest message
    int count; // number of messages in the box
    pthread_mutex_t lock;

#ifdef USE_CONDVAR
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
#else
    sem_t filled; // number of messages that can be received
    sem_t free;   // number of free places
#endif

} Mailbox;

/**
 * @struct Quiet
 * @brief Counter of messages that were sent but are not fully handled yet
 */
typedef struct
{
    pthread_mutex_t lock;
    int pending;
#ifdef USE_CONDVAR
    pthread_cond_t zero;
#else
    sem_t idle; // posted exactly once when pending drops to 0
#endif
} Quiet;

// Initializes a mailbox with the specified capacity
int mailbox_init(Mailbox *box, int capacity);

// Releases all resources allocated for the mailbox
void mailbox_destroy(Mailbox *box);

// Adds a message to the mailbox, blocking if it is full
int mailbox_put(Mailbox *box, Message message);

// Retrieves a message from the mailbox, blocking if it is empty
Message mailbox_get(Mailbox *box);

// Initializes the synchronization mechanism for tracking pending operations
int quiet_init(Quiet *quiet);

// Releases all resources allocated for the Quiet synchronization object
void quiet_destroy(Quiet *quiet);

// Increments the counter of pending operations
void quiet_add(Quiet *quiet);

// Decrements the counter of pending operations and signals when it reaches zero
void quiet_done(Quiet *quiet);

// Blocks until all pending operations have been completed
void quiet_wait(Quiet *quiet);

// Returns the name of the synchronization mechanism used by the program
const char *sync_flavour(void);

#endif
