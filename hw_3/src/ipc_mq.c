#define _XOPEN_SOURCE 700

#include "ipc.h"
#include "config.h"
#include "event_queue.h"
#include "sems.h"
#include "signals.h"
#include "simulation.h"
#include "utils.h"
#include "utils_extra.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef IPC_MQ
#error "ipc_mq.c is only for the message-queue version"
#endif

#define QUEUE_DEPTH 10      // default limit /proc/sys/fs/mqueue/msg_max for an ordinary user
#define WAIT_STEP_MS 1000   // step of the waiting loops
#define SEND_STEP_MS 20     // the clock retries a send to a full queue after draining its own queue
#define EXIT_WAIT_MS 5000   // how long the clock waits for the end of a client

// queue numbers
#define Q_CLOCK 0
#define Q_DISPATCHER 1
#define Q_CALLERS 2
#define Q_TEAM0 3
#define Q_COUNT (Q_TEAM0 + MAX_TEAMS)

static Simulation model;              // the model of the clock / the mirror of a client
static char prefix[64];               // "/ed35mq_<session>_"
static char session_name[SESSION_LEN] = "main";
static char config_path[LINE_LEN];    // absolute path of the configuration (server)
static mqd_t in_queue = (mqd_t)-1;    // queue of this process (read)
static mqd_t out_queue[Q_COUNT];      // queues of the other processes (write), opened on first use
static unsigned char out_open[Q_COUNT];
static unsigned char created[Q_COUNT];// queues created by this process (server)
static int is_server = 0;
static pid_t server_pid = 0;
static Role self_role = ROLE_CLOCK;
static int self_idx = 0;
static long done_total = 0;           // clock: number of MSG_DONE answers received
static int stats_received = 0;        // clock: number of MSG_STATS received at the end
static int hello_received = 0;        // clock: number of registered clients


/**
 * @brief Sets the role and index of this process
 * @param role The role of this process
 * @param idx The index of this process (team index for ROLE_TEAM, call index for
 */
void ipc_set_self(Role role, int idx)
{
    self_role = role;
    self_idx = idx;
}

/**
 * @brief Returns the role of this process
 * @return The role of this process
 */
Role ipc_self_role(void)
{
    return self_role;
}

/**
 * @brief Returns the index of this process
 * @return The index of this process
 */
int ipc_self_idx(void)
{
    return self_idx;
}

/**
 * @brief Returns the flavour of this IPC implementation
 * @return The flavour of this IPC implementation
 */
const char *ipc_flavour(void)
{
    return "independent + named POSIX sems + POSIX mq";
}

/**
 * @brief Returns the layout of the caller processes
 * @return The layout of the caller processes
 */
const char *ipc_calls_layout(void)
{
    return "1 callers process for all calls";
}

/**
 * @brief Remembers the path of the configuration file
 * @param path The path of the configuration file
 */
void ipc_remember_config(const char *path)
{
    char resolved[PATH_MAX];
    const char *source = realpath(path, resolved) ? resolved : path;
    size_t length = strlen(source);

    if (length >= sizeof(config_path))
        length = sizeof(config_path) - 1;

    memcpy(config_path, source, length);
    config_path[length] = '\0';
}

/**
 * @brief Name of a queue
 * @param queue The queue number
 * @param buf Buffer to store the name
 * @param size Size of the buffer
 */
static void queue_name(int queue, char *buf, size_t size)
{
    if (queue == Q_CLOCK)
        snprintf(buf, size, "%sq_clock", prefix);
    else if (queue == Q_DISPATCHER)
        snprintf(buf, size, "%sq_dispatcher", prefix);
    else if (queue == Q_CALLERS)
        snprintf(buf, size, "%sq_callers", prefix);
    else
        snprintf(buf, size, "%sq_team%d", prefix, queue - Q_TEAM0);
}

/**
 * @brief Number of the queue of an address
 * @param kind The kind of the address
 * @param idx The index of the address
 * @return The queue number
 */
static int queue_of(BoxKind kind, int idx)
{
    switch (kind)
    {
    case BOX_DISPATCHER:
        return Q_DISPATCHER;
    case BOX_TEAM:
        return Q_TEAM0 + idx;
    case BOX_CALL:
        return Q_CALLERS; // the callers family plays all calls
    case BOX_CLOCK:
    default:
        return Q_CLOCK;
    }
}

/**
 * @brief Queue of a role
 * @param role The role
 * @param idx The index
 * @return The queue number
 */
static int queue_of_role(Role role, int idx)
{
    switch (role)
    {
    case ROLE_DISPATCHER:
        return Q_DISPATCHER;
    case ROLE_TEAM:
        return Q_TEAM0 + idx;
    case ROLE_CALLERS:
    case ROLE_CALL:
        return Q_CALLERS;
    default:
        return Q_CLOCK;
    }
}

/**
 * @brief Absolute time "now + ms" for the timed functions
 * @param ms The number of milliseconds to add
 * @return The absolute time
 */
static struct timespec deadline_after(int ms)
{
    struct timespec when;
    clock_gettime(CLOCK_REALTIME, &when);

    when.tv_sec += ms / 1000;
    when.tv_nsec += (long)(ms % 1000) * 1000000L;

    if (when.tv_nsec >= 1000000000L)
    {
        when.tv_sec++;
        when.tv_nsec -= 1000000000L;
    }

    return when;
}

/**
 * @brief Opens (once) a queue for writing
 * @return Descriptor or (mqd_t)-1
 */
static mqd_t writer(int queue)
{
    if (!out_open[queue])
    {
        char name[96];
        queue_name(queue, name, sizeof(name));

        mqd_t fd = mq_open(name, O_WRONLY);
        if (fd == (mqd_t)-1)
            return fd;

        out_queue[queue] = fd;
        out_open[queue] = 1;
    }

    return out_queue[queue];
}

/**
 * @brief Creates a queue (server); a stale queue of a killed run is removed first 
 * @param queue The queue number
 * @return 1 on success, 0 on failure
 */
static int queue_create(int queue)
{
    char name[96];
    queue_name(queue, name, sizeof(name));

    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = QUEUE_DEPTH;
    attr.mq_msgsize = sizeof(Message);

    mqd_t fd = mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);

    if (fd == (mqd_t)-1 && errno == EEXIST)
    {
        mq_unlink(name);
        fd = mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    }

    if (fd == (mqd_t)-1)
    {
        perror(name);
        return 0;
    }

    mq_close(fd);
    created[queue] = 1;
    return 1;
}

/**
 * @brief Removes a queue (server)
 * @param queue The queue number
 */
void world_lock(Simulation *sim)
{
    (void)sim; // every process owns its copy of the model, nothing to lock
}

/**
 * @brief Unlocks the world (server)
 * @param sim The simulation
 */
void world_unlock(Simulation *sim)
{
    (void)sim;
}

/**
 * @brief Locks the output (server)
 */
void ipc_output_lock(void)
{
    sem_wait_retry(sem_ref(SEM_OUTPUT));
}

/**
 * @brief Unlocks the output (server)
 */
void ipc_output_unlock(void)
{
    sem_t *sem = sem_ref(SEM_OUTPUT);
    if (sem)
        sem_post(sem);
}


/**
 * @brief Copies the attachments of a message into the mirror of this process
 * @param sim The simulation
 * @param message The message to apply
 */
void ipc_apply(Simulation *sim, const Message *message)
{
    if (sim == NULL || message == NULL)
        return;

    // the clock owns the model time; the others take it from every message
    if (self_role != ROLE_CLOCK)
        sim->now = message->time;

    if (message->call_count > sim->call_count && message->call_count <= MAX_CALLS)
        sim->call_count = message->call_count;

    if (message->call_idx >= 0 && message->call_idx < MAX_CALLS)
    {
        Call *call = &sim->calls[message->call_idx];
        pid_t pid = call->pid;
        int started = call->started; // process information is local

        *call = message->call;
        call->pid = pid;
        call->started = started;
    }

    if (message->team_idx >= 0 && message->team_idx < MAX_TEAMS)
    {
        Team *team = &sim->teams[message->team_idx];
        pid_t pid = team->pid;
        int started = team->started;

        *team = message->team;
        team->pid = pid;
        team->started = started;
    }
}

/**
 * @brief Clock: handles a message from its own queue
 * @param sim The simulation
 * @param message The message to handle
 */
static void clock_handle(Simulation *sim, const Message *message)
{
    switch (message->type)
    {
    case MSG_DONE:
        done_total++;
        break;

    case MSG_SCHEDULE:
        schedule_event_checked(sim, message->value, (EventType)message->event, message->arg);
        break;

    case MSG_REMOVE_DEADLINES:
        remove_call_deadlines(sim, message->arg);
        break;

    case MSG_SYNC:
        ipc_apply(sim, message);
        break;

    case MSG_HELLO:
        hello_received++;
        sim->processes_started++;

        if (message->sender_role == ROLE_DISPATCHER)
            sim->dispatcher_pid = message->value;
        else if (message->sender_role == ROLE_CALLERS)
            sim->callers_pid = message->value;
        else if (message->sender_role == ROLE_TEAM && message->sender_idx >= 0 && message->sender_idx < MAX_TEAMS)
        {
            sim->teams[message->sender_idx].pid = message->value;
            sim->teams[message->sender_idx].started = 1;
        }
        break;

    case MSG_STATS:
    {
        const Counters *counters = &message->u.counters;
        sim->total_wait += counters->total_wait;
        sim->total_service += counters->total_service;
        sim->served += counters->served;
        sim->cancelled += counters->cancelled;
        sim->unserved += counters->unserved;
        sim->escalations += counters->escalations;
        sim->dispatches += counters->dispatches;
        sim->received += counters->received;
        stats_received++;
        break;
    }

    default:
        break;
    }
}

/**
 * @brief Clock: handles everything that is in its queue now (does not wait)
 * @param sim The simulation
 */
static void clock_drain(Simulation *sim)
{
    Message message;
    struct timespec now = deadline_after(0);

    while (mq_timedreceive(in_queue, (char *)&message, sizeof(message), NULL, &now) >= 0)
        clock_handle(sim, &message);
}


/**
 * @brief Puts a message into a queue
 * @param queue The queue number
 * @param message The message to send
 * @return 1 on success, 0 on failure
 */
static int queue_send(int queue, const Message *message)
{
    mqd_t fd = writer(queue);
    SOFT_ASSERT(fd != (mqd_t)-1, "cannot open a message queue", 0);

    if (self_role != ROLE_CLOCK)
    {
        while (mq_send(fd, (const char *)message, sizeof(*message), 0) != 0)
            if (errno != EINTR)
                return 0;
        return 1;
    }

    for (;;)
    {
        struct timespec when = deadline_after(SEND_STEP_MS);

        if (mq_timedsend(fd, (const char *)message, sizeof(*message), 0, &when) == 0)
            return 1;

        if (errno != ETIMEDOUT && errno != EINTR)
            return 0;

        clock_drain(&model);
    }
}

/**
 * @brief Fills the common fields of a message
 * @param sim The simulation
 * @param message The message to fill
 */
static void stamp(Simulation *sim, Message *message)
{
    message->time = sim ? sim->now : 0;
    message->sender_role = self_role;
    message->sender_idx = self_idx;
    message->call_count = sim ? sim->call_count : 0;
}

/**
 * @brief Sends a message
 * @param sim The simulation
 * @param kind The box kind
 * @param idx The box index
 * @param message The message to send
 * @return 1 on success, 0 on failure
 */
int ipc_send(Simulation *sim, BoxKind kind, int idx, Message *message)
{
    SOFT_ASSERT(message, "message is NULL", 0);

    stamp(sim, message);

    int queue = queue_of(kind, idx);

    if (queue != Q_CLOCK)
        sem_post(sem_ref(SEM_SENT)); // counted before it can be handled

    return queue_send(queue, message);
}

/**
 * @brief Sends a simple message
 * @param sim The simulation
 * @param kind The box kind
 * @param idx The box index
 * @param type The message type
 * @param event The event
 * @param arg The argument
 * @param value The value
 * @return 1 on success, 0 on failure
 */
int ipc_send_simple(Simulation *sim, BoxKind kind, int idx, MsgType type, int event, int arg, int value)
{
    Message message;
    memset(&message, 0, sizeof(message));

    message.type = type;
    message.event = event;
    message.arg = arg;
    message.value = value;
    message.call_idx = -1;
    message.team_idx = -1;

    return ipc_send(sim, kind, idx, &message);
}

/**
 * @brief Sends a message that is not counted
 * @param sim The simulation
 * @param queue The queue number
 * @param message The message to send
 * @return 1 on success, 0 on failure
 */
static int send_uncounted(Simulation *sim, int queue, Message *message)
{
    stamp(sim, message);
    return queue_send(queue, message);
}

/**
 * @brief Receives a message
 * @param sim The simulation
 * @return The received message
 */
Message ipc_receive(Simulation *sim)
{
    (void)sim;

    Message message;
    memset(&message, 0, sizeof(message));

    for (;;)
    {
        if (mq_receive(in_queue, (char *)&message, sizeof(message), NULL) >= 0)
            return message;

        if (errno != EINTR)
        {
            perror("mq_receive");
            message.type = MSG_SHUTDOWN;
            return message;
        }
    }
}

/**
 * @brief Signals the completion of the simulation
 * @param sim The simulation
 */
void ipc_done(Simulation *sim)
{
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = MSG_DONE;
    message.call_idx = -1;
    message.team_idx = -1;

    send_uncounted(sim, Q_CLOCK, &message);
}

/**
 * @brief Schedules an event
 * @param sim The simulation
 * @param time The time of the event
 * @param type The event type
 * @param id The event ID
 * @return 1 on success, 0 on failure
 */
int ipc_schedule(Simulation *sim, int time, EventType type, int id)
{
    if (self_role == ROLE_CLOCK)
        return schedule_event_checked(sim, time, type, id);

    SOFT_ASSERT(time > sim->now, "a new event must be later than the current minute", 0);

    Message message;
    memset(&message, 0, sizeof(message));
    message.type = MSG_SCHEDULE;
    message.event = type;
    message.arg = id;
    message.value = time;
    message.call_idx = -1;
    message.team_idx = -1;

    return send_uncounted(sim, Q_CLOCK, &message);
}

/**
 * @brief Removes deadlines for a call
 * @param sim The simulation
 * @param call_id The call ID
 * @return 1 on success, 0 on failure
 */
int ipc_remove_deadlines(Simulation *sim, int call_id)
{
    if (self_role == ROLE_CLOCK)
        return remove_call_deadlines(sim, call_id);

    Message message;
    memset(&message, 0, sizeof(message));
    message.type = MSG_REMOVE_DEADLINES;
    message.arg = call_id;
    message.call_idx = -1;
    message.team_idx = -1;

    return send_uncounted(sim, Q_CLOCK, &message);
}

/**
 * @brief Sends a MSG_SYNC to one queue
 * @param sim The simulation
 * @param queue The queue number
 * @param message The message to send
 */
static void sync_to(Simulation *sim, int queue, Message *message)
{
    if (queue == queue_of_role(self_role, self_idx))
        return; // the own mirror is already up to date

    if (queue == Q_CLOCK)
        send_uncounted(sim, queue, message);
    else
    {
        stamp(sim, message);
        sem_post(sem_ref(SEM_SENT));
        queue_send(queue, message);
    }
}

/**
 * @brief Publishes a call
 * @param sim The simulation
 * @param call_idx The call index
 */
void ipc_publish_call(Simulation *sim, int call_idx)
{
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = MSG_SYNC;
    message.call_idx = call_idx;
    message.team_idx = -1;
    message.call = sim->calls[call_idx];

    // the record of a call is needed by the clock, the dispatcher and the callers
    sync_to(sim, Q_CLOCK, &message);
    sync_to(sim, Q_DISPATCHER, &message);
    sync_to(sim, Q_CALLERS, &message);
}

/**
 * @brief Publishes a team
 * @param sim The simulation
 * @param team_idx The team index
 */
void ipc_publish_team(Simulation *sim, int team_idx)
{
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = MSG_SYNC;
    message.call_idx = -1;
    message.team_idx = team_idx;
    message.team = sim->teams[team_idx];

    // the record of a team is needed by the clock, the dispatcher and the team itself
    sync_to(sim, Q_CLOCK, &message);
    sync_to(sim, Q_DISPATCHER, &message);
    sync_to(sim, Q_TEAM0 + team_idx, &message);
}


/**
 * @brief Holds the quiet state
 * @param sim The simulation
 */
void ipc_quiet_hold(Simulation *sim)
{
    (void)sim; // the clock checks the counters only in ipc_quiet_wait()
}

/**
 * @brief Releases the quiet state
 * @param sim The simulation
 */
void ipc_quiet_release(Simulation *sim)
{
    (void)sim;
}

/**
 * @brief Checks the registered clients with kill(pid, 0)
 * @return 1 if all are alive
 */
static int processes_alive(Simulation *sim)
{
    int alive = 1;

    if (sim->dispatcher_pid > 0 && kill(sim->dispatcher_pid, 0) != 0 && errno == ESRCH)
    {
        log_event(sim, "PROCESS FAILURE: dispatcher (pid %d) has terminated", (int)sim->dispatcher_pid);
        sim->dispatcher_pid = 0;
        alive = 0;
    }

    if (sim->callers_pid > 0 && kill(sim->callers_pid, 0) != 0 && errno == ESRCH)
    {
        log_event(sim, "PROCESS FAILURE: callers (pid %d) has terminated", (int)sim->callers_pid);
        sim->callers_pid = 0;
        alive = 0;
    }

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].pid > 0 && kill(sim->teams[idx].pid, 0) != 0 && errno == ESRCH)
        {
            log_event(sim, "PROCESS FAILURE: team B%d (pid %d) has terminated", sim->teams[idx].id,
                      (int)sim->teams[idx].pid);
            sim->teams[idx].pid = 0;
            sim->teams[idx].started = 0;
            alive = 0;
        }

    return alive;
}

/**
 * @brief The clock waits until every sent message has been handled
 * @param sim The simulation
 * @return 1 when the phase is over, 0 if a process has died
 */
int ipc_quiet_wait(Simulation *sim)
{
    sem_t *sent = sem_ref(SEM_SENT);

    for (;;)
    {
        int value = 0;
        sem_getvalue(sent, &value);

        if (done_total == value)
            return 1;

        Message message;
        struct timespec when = deadline_after(WAIT_STEP_MS);

        if (mq_timedreceive(in_queue, (char *)&message, sizeof(message), NULL, &when) >= 0)
        {
            clock_handle(sim, &message);
            continue;
        }

        if (errno == EINTR)
            continue;

        if (errno == ETIMEDOUT && !processes_alive(sim))
            return 0;
    }
}


/**
 * @brief Creates the queues and the semaphores of the session
 * @param loaded The loaded model
 * @param session The session name
 * @return Pointer to the model of the clock, NULL on failure
 */
Simulation *ipc_server_init(const Simulation *loaded, const char *session)
{
    SOFT_ASSERT(loaded, "model is NULL", NULL);

    snprintf(session_name, sizeof(session_name), "%s", session ? session : "main");
    snprintf(prefix, sizeof(prefix), "/ed35mq_%s_", session_name);
    sems_setup(prefix, NULL);

    model = *loaded;
    model.clock_pid = getpid();
    model.processes_started = 1;

    is_server = 1;
    server_pid = getpid();
    ipc_set_self(ROLE_CLOCK, 0);
    atexit(ipc_server_cleanup);

    int ok = sems_create(SEM_OUTPUT, 1) && sems_create(SEM_SENT, 0) && sems_create(SEM_READY, 0) &&
             sems_create(SEM_EXITED, 0);

    ok = ok && queue_create(Q_CLOCK) && queue_create(Q_DISPATCHER) && queue_create(Q_CALLERS);

    for (int idx = 0; ok && idx < model.team_count; idx++)
        ok = queue_create(Q_TEAM0 + idx);

    if (!ok)
        return NULL;

    char name[96];
    queue_name(Q_CLOCK, name, sizeof(name));
    in_queue = mq_open(name, O_RDONLY);
    SOFT_ASSERT(in_queue != (mqd_t)-1, "cannot open the queue of the clock", NULL);

    // the first message of every client queue is its welcome: the settings
    Message welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.type = MSG_WELCOME;
    welcome.call_idx = -1;
    welcome.team_idx = -1;

    Settings *settings = &welcome.u.settings;
    settings->delay_ms = model.delay_ms;
    settings->shift_limit = model.shift_limit;
    settings->strategy = model.strategy;
    settings->random_calls = model.random_calls;
    settings->seed = model.seed;
    settings->clock_pid = getpid();
    snprintf(settings->config_path, sizeof(settings->config_path), "%s", config_path);
    memcpy(settings->log_path, model.log_path, sizeof(settings->log_path));

    welcome.arg = 0;
    ok = send_uncounted(&model, Q_DISPATCHER, &welcome) && send_uncounted(&model, Q_CALLERS, &welcome);

    for (int idx = 0; ok && idx < model.team_count; idx++)
    {
        welcome.arg = idx;
        welcome.value = model.teams[idx].id; // a client started with -i checks it
        ok = send_uncounted(&model, Q_TEAM0 + idx, &welcome);
    }

    // the claim semaphores are created last: the clients start when they see them
    ok = ok && sems_create(SEM_CLAIM_DISPATCHER, 1) && sems_create(SEM_CLAIM_CALLERS, 1);
    for (int idx = 0; ok && idx < model.team_count; idx++)
        ok = sems_create(SEM_CLAIM_TEAM0 + idx, 1);

    return ok ? &model : NULL;
}

/**
 * @brief Prints which roles are still not played
 * @param sim The simulation
 */
static void print_missing(const Simulation *sim)
{
    char line[OUT_BUF_LEN];
    int len = snprintf(line, sizeof(line), "[clock] waiting for:");

    if (sim->dispatcher_pid == 0)
        len += snprintf(line + len, sizeof(line) - (size_t)len, " dispatcher");

    if (sim->callers_pid == 0)
        len += snprintf(line + len, sizeof(line) - (size_t)len, " callers");

    for (int idx = 0; idx < sim->team_count && len < (int)sizeof(line) - 16; idx++)
        if (!sim->teams[idx].started)
            len += snprintf(line + len, sizeof(line) - (size_t)len, " team(B%d)", sim->teams[idx].id);

    fd_printf(STDOUT_FILENO, "%s\n", line);
}

/**
 * @brief Waits until every client has registered itself
 * @param sim The simulation
 * @return 1 on success, 0 on failure
 */
int ipc_start_processes(Simulation *sim)
{
    int needed = 2 + sim->team_count;
    int ready = 0;
    int seconds = 0;

    fd_printf(STDOUT_FILENO,
              "[clock] pid %d, session '%s'. Start the other programs in any terminals:\n"
              "        ./bin/ed_mq_dispatcher -n %s\n"
              "        ./bin/ed_mq_callers    -n %s\n"
              "        ./bin/ed_mq_team       -n %s    (%d times, or with -i <team id>)\n",
              (int)getpid(), session_name, session_name, session_name, session_name, sim->team_count);

    while (ready < needed)
    {
        if (signals_stop_requested())
        {
            fd_printf(STDOUT_FILENO, "[clock] interrupted while waiting for the processes\n");
            clock_drain(sim); // pids of the registered ones, they get MSG_SHUTDOWN
            return 0;
        }

        // a client sends MSG_HELLO, then posts "ready"
        if (sem_wait_ms(sem_ref(SEM_READY), WAIT_STEP_MS) == 0)
        {
            ready++;
            fd_printf(STDOUT_FILENO, "[clock] registered %d of %d processes\n", ready, needed);
            continue;
        }

        clock_drain(sim);

        if (!processes_alive(sim))
            return 0;

        if (++seconds % 5 == 0)
            print_missing(sim);
    }

    // every MSG_HELLO was sent before the semaphore was posted: they are in the queue
    while (hello_received < needed)
    {
        Message message;
        struct timespec when = deadline_after(WAIT_STEP_MS);

        if (mq_timedreceive(in_queue, (char *)&message, sizeof(message), NULL, &when) < 0)
        {
            if (errno == EINTR)
                continue;
            return 0;
        }

        clock_handle(sim, &message);
    }

    return 1;
}

/**
 * @brief Starts a call
 * @param sim The simulation
 * @param call_idx The call index
 * @return 1 on success, 0 on failure
 */
int ipc_start_call(Simulation *sim, int call_idx)
{
    sim->calls[call_idx].pid = sim->callers_pid;
    sim->calls[call_idx].started = 1;

    // a call made by the random generator is not known to the other processes yet
    ipc_publish_call(sim, call_idx);
    return 1;
}

/**
 * @brief Stops a call
 * @param sim The simulation
 * @param call_idx The call index
 * @return 1 on success, 0 on failure
 */
int ipc_stop_call(Simulation *sim, int call_idx)
{
    sim->calls[call_idx].started = 0; // the callers process goes on, only the slot is freed
    return 1;
}

/**
 * @brief Stops all processes
 * @param sim The simulation
 */
void ipc_stop_all(Simulation *sim)
{
    if (sim == NULL)
        return;

    Message message;
    memset(&message, 0, sizeof(message));
    message.type = MSG_SHUTDOWN;
    message.call_idx = -1;
    message.team_idx = -1;

    int running = 0;

    if (sim->dispatcher_pid > 0)
    {
        send_uncounted(sim, Q_DISPATCHER, &message);
        running++;
    }

    if (sim->callers_pid > 0)
    {
        send_uncounted(sim, Q_CALLERS, &message);
        running++;
    }

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].pid > 0)
        {
            send_uncounted(sim, Q_TEAM0 + idx, &message);
            running++;
        }

    // every client answers with its counters (MSG_STATS) and then posts "exited"
    while (stats_received < running)
    {
        Message answer;
        struct timespec when = deadline_after(EXIT_WAIT_MS);

        if (mq_timedreceive(in_queue, (char *)&answer, sizeof(answer), NULL, &when) < 0)
        {
            if (errno == EINTR)
                continue;

            fd_printf(STDOUT_FILENO, "[clock] %d process(es) did not send their statistics\n", running - stats_received);
            break;
        }

        clock_handle(sim, &answer);
    }

    for (int done = 0; done < stats_received; done++)
        if (sem_wait_ms(sem_ref(SEM_EXITED), EXIT_WAIT_MS) != 0)
            break;
}

/**
 * @brief Removes the queues and the semaphores
 */
void ipc_server_cleanup(void)
{
    if (!is_server || getpid() != server_pid)
        return;

    is_server = 0;

    if (in_queue != (mqd_t)-1)
    {
        mq_close(in_queue);
        in_queue = (mqd_t)-1;
    }

    for (int queue = 0; queue < Q_COUNT; queue++)
    {
        if (out_open[queue])
        {
            mq_close(out_queue[queue]);
            out_open[queue] = 0;
        }

        if (created[queue])
        {
            char name[96];
            queue_name(queue, name, sizeof(name));
            mq_unlink(name);
            created[queue] = 0;
        }
    }

    sems_destroy_all();
}

/**
 * @brief Takes a claim semaphore without waiting
 * @param id The semaphore ID
 * @return 1 on success, 0 on failure
 */
static int try_claim(int id)
{
    sem_t *sem = sem_ref(id);
    return sem != NULL && sem_trywait(sem) == 0;
}

/**
 * @brief Opens the own queue and takes the welcome message from it
 * @param queue The queue number
 * @param welcome The welcome message to fill
 * @return 1 on success
 */
static int read_welcome(int queue, Message *welcome)
{   
    char name[96];
    queue_name(queue, name, sizeof(name));

    in_queue = mq_open(name, O_RDONLY);
    if (in_queue == (mqd_t)-1)
        return 0;

    struct timespec when = deadline_after(WAIT_STEP_MS);

    if (mq_timedreceive(in_queue, (char *)welcome, sizeof(*welcome), NULL, &when) < 0 ||
        welcome->type != MSG_WELCOME)
    {
        mq_close(in_queue);
        in_queue = (mqd_t)-1;
        return 0;
    }

    return 1;
}

/**
 * @brief Gives a role back: the welcome message is returned to the queue
 * @param queue The queue number
 * @param claim The claim semaphore ID
 * @param welcome The welcome message to return
 */
static void release_role(int queue, int claim, Message *welcome)
{
    mq_close(in_queue);
    in_queue = (mqd_t)-1;

    mqd_t fd = writer(queue);
    if (fd != (mqd_t)-1)
        mq_send(fd, (const char *)welcome, sizeof(*welcome), 0);

    sem_post(sem_ref(claim));
}

/**
 * @brief Attaches a client to a role
 * @param role The role to claim
 * @param idx The index of the role (for a team)
 * @param session The session name
 * @param wait_seconds The number of seconds to wait for the clock
 * @return Pointer to the model of the client, NULL on failure
 */
Simulation *ipc_client_attach(Role role, int *idx, const char *session, int wait_seconds)
{
    snprintf(session_name, sizeof(session_name), "%s", session ? session : "main");
    snprintf(prefix, sizeof(prefix), "/ed35mq_%s_", session_name);
    sems_setup(prefix, NULL);

    // 1. wait for the clock: the claim semaphores are created last
    int waited_ms = 0;
    int said = 0;

    while (!sems_exist(SEM_CLAIM_DISPATCHER))
    {
        if (!said)
        {
            fd_printf(STDOUT_FILENO, "waiting for the clock (session '%s')...\n", session_name);
            said = 1;
        }

        if (signals_stop_requested() || waited_ms >= wait_seconds * 1000)
        {
            fprintf(stderr, "The clock of session '%s' is not running\n", session_name);
            return NULL;
        }

        struct timespec pause = {0, 200 * 1000000L};
        nanosleep(&pause, NULL);
        waited_ms += 200;
    }

    // 2. claim the role and take the welcome message from the queue of the role
    Message welcome;
    int claimed = 0;

    if (role == ROLE_DISPATCHER)
        claimed = try_claim(SEM_CLAIM_DISPATCHER) && read_welcome(Q_DISPATCHER, &welcome);
    else if (role == ROLE_CALLERS)
        claimed = try_claim(SEM_CLAIM_CALLERS) && read_welcome(Q_CALLERS, &welcome);
    else if (role == ROLE_TEAM)
    {
        int wanted_id = *idx;

        for (int team = 0; team < MAX_TEAMS && !claimed && sems_exist(SEM_CLAIM_TEAM0 + team); team++)
        {
            if (!try_claim(SEM_CLAIM_TEAM0 + team))
                continue;

            if (!read_welcome(Q_TEAM0 + team, &welcome))
            {
                sem_post(sem_ref(SEM_CLAIM_TEAM0 + team));
                continue;
            }

            if (wanted_id > 0 && welcome.value != wanted_id)
            {
                release_role(Q_TEAM0 + team, SEM_CLAIM_TEAM0 + team, &welcome); // another team
                continue;
            }

            *idx = team;
            claimed = 1;
        }
    }

    if (!claimed)
    {
        fprintf(stderr, "No free role for this process: it is already played by another process\n");
        return NULL;
    }

    ipc_set_self(role, role == ROLE_TEAM ? *idx : 0);

    // 3. the own copy of the model: the same configuration and command-line options
    const Settings *settings = &welcome.u.settings;

    set_defaults(&model);

    if (!load_config(&model, settings->config_path))
        return NULL;

    model.delay_ms = settings->delay_ms;
    model.shift_limit = settings->shift_limit;
    model.strategy = (Strategy)settings->strategy;
    model.random_calls = settings->random_calls;
    model.seed = settings->seed;
    model.clock_pid = settings->clock_pid;
    memcpy(model.log_path, settings->log_path, sizeof(model.log_path));

    if (!validate_simulation(&model))
        return NULL;

    // 4. registration: MSG_HELLO with the pid, then the semaphore "ready"
    Message hello;
    memset(&hello, 0, sizeof(hello));
    hello.type = MSG_HELLO;
    hello.value = getpid();
    hello.call_idx = -1;
    hello.team_idx = -1;

    if (!send_uncounted(&model, Q_CLOCK, &hello))
        return NULL;

    sem_post(sem_ref(SEM_READY));
    return &model;
}

/** @brief Detaches a client from the simulation
 *  @param sim The simulation
 */
void ipc_client_detach(Simulation *sim)
{
    // the final counters of this process go to the clock
    Message stats;
    memset(&stats, 0, sizeof(stats));
    stats.type = MSG_STATS;
    stats.call_idx = -1;
    stats.team_idx = -1;

    Counters *counters = &stats.u.counters;
    counters->total_wait = sim->total_wait;
    counters->total_service = sim->total_service;
    counters->served = sim->served;
    counters->cancelled = sim->cancelled;
    counters->unserved = sim->unserved;
    counters->escalations = sim->escalations;
    counters->dispatches = sim->dispatches;
    counters->received = sim->received;

    send_uncounted(sim, Q_CLOCK, &stats);

    sem_t *exited = sem_ref(SEM_EXITED);
    if (exited)
        sem_post(exited);

    if (in_queue != (mqd_t)-1)
        mq_close(in_queue);

    for (int queue = 0; queue < Q_COUNT; queue++)
        if (out_open[queue])
        {
            mq_close(out_queue[queue]);
            out_open[queue] = 0;
        }

    sems_destroy_all(); // a client only closes the semaphores
}
