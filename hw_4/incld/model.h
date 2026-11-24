#ifndef MODEL_H
#define MODEL_H
#include <stdio.h>
#include <sys/types.h>

#define MAX_DISTRICTS 16 // the max number of districts in the simulation
#define MAX_TEAMS 32     // the max number of teams in the simulation
#define MAX_CALLS 128    // the max number of calls in the simulation
#define MAX_EVENTS 1024  // the max number of events in the simulation
#define NAME_LEN 32      // the max length of names for districts, teams, and specializations
#define LINE_LEN 256     // the max length of a line in the configuration file
#define NO_CALL (-1)     // value of Team.call_id when the team serves nothing
#define NO_TEAM (-1)     // value of Call.assigned_team when no team is assigned

/**
 * @enum Specialization
 * @brief Shows the specialization of a team or call
 */
typedef enum
{
    SPEC_MEDIC = 0,
    SPEC_REANIM,
    SPEC_FIRE,
    SPEC_POLICE,
    SPEC_COUNT

} Specialization;

/**
 * @enum CallState
 * @brief Shows the state of a call
 */
typedef enum
{
    CALL_PENDING,
    CALL_WAITING,
    CALL_DISPATCHED,
    CALL_EN_ROUTE,
    CALL_IN_SERVICE,
    CALL_COMPLETED,
    CALL_CANCELLED,
    CALL_UNSERVED

} CallState;

/**
 * @enum TeamState
 * @brief Shows the state of a team
 */
typedef enum
{
    TEAM_AVAILABLE,
    TEAM_EN_ROUTE,
    TEAM_AT_SCENE,
    TEAM_RETURNING

} TeamState;

/**
 * @enum Strategy
 * @brief Shows the strategy for dispatching teams
 */
typedef enum
{
    STRATEGY_NEAREST = 0,
    STRATEGY_FASTEST
} Strategy;

/**
 * @enum EventType
 * @brief Shows the type of an event
 */
typedef enum
{
    EV_CALL_ARRIVAL,
    EV_ESCALATION,
    EV_TEAM_ARRIVAL,
    EV_SERVICE_DONE,
    EV_RETURN_DONE,
    EV_CANCEL,
    EV_WAIT_TIMEOUT
} EventType;

/**
 * @enum StopReason
 * @brief Why the simulation loop has stopped
 */
typedef enum
{
    STOP_ALL_PROCESSED = 0, // the event queue became empty
    STOP_SHIFT_LIMIT,       // the next event is after the end of the shift
    STOP_INTERRUPTED,       // SIGINT / SIGTERM from the user
    STOP_INTAKE_CLOSED,     // SIGQUIT
    STOP_PROCESS_FAILURE    // a process of the simulation died unexpectedly
} StopReason;

/**
 * @struct District
 * @brief District in the simulation
 */
typedef struct
{
    char name[NAME_LEN];
    int travel[MAX_DISTRICTS];
    int travel_count; // how many travel values were given in the configuration line
} District;

/**
 * @struct Team
 * @brief Team in the simulation
 */
typedef struct
{
    int id;
    Specialization spec;
    int speed;
    int district;
    int base;
    TeamState state;
    int call_id;

    // process of the team
    pid_t pid;   // pid of the process that plays this team
    int started; // 1 if the process is running and registered

} Team;

/**
 * @struct Call
 * @brief Call in the simulation
 */
typedef struct
{
    int id;
    int district;
    int x, y; // coordinates of the call location
    int spec;
    int priority;
    int arrival;
    int service_duration;
    int max_wait;
    int escalation_after;
    int cancel_after;
    CallState state;
    int assigned_team;
    int dispatch_time;
    int service_start;
    int completion_time;

    // process of the call
    pid_t pid;   // pid of the process that plays this call
    int started; // 1 if the process that plays the call is running

} Call;

/**
 * @struct Event
 * @brief Event in the simulation
 */
typedef struct
{
    int time;
    EventType type;
    int id;
    unsigned long seq; // sequence number for when events have the same time
} Event;

/**
 * @struct Simulation
 * @brief Main simulation structure
 */
typedef struct
{
    District districts[MAX_DISTRICTS];
    int district_count;

    Team teams[MAX_TEAMS];
    int team_count;

    Call calls[MAX_CALLS];
    int call_count;

    Event events[MAX_EVENTS];
    int event_count;

    unsigned long next_seq;
    int now;
    int delay_ms;
    int shift_limit; // shift length in minutes, 0 = unlimited
    int escalation_enabled;
    int return_to_base;
    Strategy strategy;
    long total_wait;
    long total_service;
    int served;
    int cancelled;
    int unserved;
    int escalations;
    int dispatches;
    int completed_events;
    int received;          // calls that really arrived
    int invariant_errors;  // number of detected invariant violations (must stay 0)
    StopReason stop_reason;

    // random call generator
    int random_calls;      // 1 = generate calls randomly instead of / in addition to [CALLS]
    unsigned int seed;     // seed of the generator, gives a reproducible run
    unsigned int rng;      // current state of rand_r()
    int random_interval;   // mean interval between generated calls, minutes
    int next_call_id;      // id of the next generated call

    int intake_closed;     // 1 after SIGQUIT: new calls are not accepted any more

    // processes
    pid_t clock_pid;        // pid of the clock process, clients forward signals to it
    pid_t dispatcher_pid;   // pid of the dispatcher process
    pid_t callers_pid;      // pid of the "callers" family process
    int processes_started;  // number of started/registered processes
    char log_path[LINE_LEN];// log file, opened by every process with O_APPEND

} Simulation;

#endif
