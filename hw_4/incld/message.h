#ifndef MESSAGE_H
#define MESSAGE_H

#include "model.h"

/**
 * @enum MsgType
 * @brief Kinds of messages that the processes send to each other
 */
typedef enum
{
    MSG_WAKE = 0,         // clock -> call/team: an event of this entity has come
    MSG_NEW_CALL,         // call -> dispatcher: the call has arrived and waits
    MSG_CALL_CHANGED,     // call -> dispatcher: the priority of a waiting call has changed
    MSG_TEAM_FREE,        // team -> dispatcher: the team is available again
    MSG_ROUND,            // clock -> dispatcher: all reactions of this minute are done, assign teams
    MSG_ASSIGN,           // dispatcher -> team: serve the call
    MSG_CALL_FINISHED,    // team -> call: the help is finished
    MSG_SHUTDOWN,         // clock -> everybody: the simulation is over 

    // the messages below are used only by the message-queue version (no shared memory)
    MSG_SYNC,             // new copy of a call and/or team record for the mirror of the receiver
    MSG_SCHEDULE,         // any -> clock: put event into the queue
    MSG_REMOVE_DEADLINES, // any -> clock: remove the deadlines of call arg from the queue
    MSG_DONE,             // any -> clock: one counted message has been handled completely
    MSG_HELLO,            // client -> clock: a new process has registered
    MSG_WELCOME,          // clock -> client: configuration path and command-line overrides
    MSG_STATS             // client -> clock: final counters of the process before it exits
} MsgType;

/**
 * @enum Role
 * @brief Roles of the processes
 */
typedef enum
{
    ROLE_CLOCK = 0,  // the server: clock, event queue, invariants, signals, summary
    ROLE_DISPATCHER, // assigns free teams to waiting calls
    ROLE_TEAM,       // one team
    ROLE_CALL,       // one call
    ROLE_CALLERS,    // the family of all calls
    ROLE_COUNT
} Role;

/**
 * @enum BoxKind
 * @brief Address of a mailbox (or a message queue)
 */
typedef enum
{
    BOX_DISPATCHER = 0,
    BOX_TEAM,  // + team index
    BOX_CALL,  // + call index
    BOX_CLOCK  // only in the message-queue version
} BoxKind;

/**
 * @struct Settings
 * @brief Command-line overrides sent to the clients of the message-queue version
 */
typedef struct
{
    int delay_ms;
    int shift_limit;
    int strategy;
    int random_calls;
    unsigned int seed;
    pid_t clock_pid;
    char config_path[LINE_LEN];
    char log_path[LINE_LEN];
} Settings;

/**
 * @struct Counters
 * @brief Statistics collected by one process (summed by the clock at the end)
 */
typedef struct
{
    long total_wait;
    long total_service;
    int served;
    int cancelled;
    int unserved;
    int escalations;
    int dispatches;
    int received;
} Counters;

/**
 * @struct Message
 * @brief One message of a mailbox (shared memory) or of a message queue
 */
typedef struct
{
    int type;        // MsgType
    int event;       // EventType for MSG_WAKE / MSG_SCHEDULE
    int arg;         // index of a call or a team 
    int value;       // extra value
    int time;        // model time of the sender when the message was sent
    int sender_role; // Role of the sender
    int sender_idx;  // index of the sender

    // attachments
    int call_idx;
    int team_idx;
    int call_count;
    Call call;
    Team team;

    union
    {
        Settings settings; // MSG_WELCOME
        Counters counters; // MSG_STATS
    } u;
} Message;

#endif
