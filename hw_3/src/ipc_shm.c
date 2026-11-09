#define _XOPEN_SOURCE 700

#include "ipc.h"
#include "event_queue.h"
#include "roles.h"
#include "sems.h"
#include "signals.h"
#include "simulation.h"
#include "utils.h"
#include "utils_extra.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(IPC_FORK) && !defined(IPC_SHM_INDEPENDENT)
#error "define IPC_FORK or IPC_SHM_INDEPENDENT"
#endif

#define SHM_MAGIC 0x45443335u // "ED35": the server has finished the initialisation
#define WAIT_STEP_MS 1000     // step of the waiting loops, the processes are checked between steps
#define EXIT_WAIT_MS 5000     // how long the clock waits for the end of a client

// all message slots of all mailboxes
#define TOTAL_SLOTS (DISPATCHER_BOX_SIZE + MAX_TEAMS * ENTITY_BOX_SIZE + CALLERS_BOX_SIZE + MAX_CALLS * ENTITY_BOX_SIZE)

/**
 * @struct BoxHeader
 * @brief Bounded FIFO queue (mailbox) in the shared memory, its messages are in SharedArea.slots
 */
typedef struct
{
    int offset;   // index of the first slot of the box in SharedArea.slots
    int capacity; // number of slots
    int head;     // index of the oldest message
    int count;    // number of messages in the box
} BoxHeader;

/**
 * @struct SharedArea
 * @brief Contents of the POSIX shared memory object
 */
typedef struct
{
    unsigned magic;           // SHM_MAGIC when the object is ready
    Simulation sim;           // the model, protected by SEM_WORLD
    int pending;              // quiet counter: sent but not yet handled messages (SEM_QUIET_LOCK)
    int registered;           // number of registered client processes
    BoxHeader boxes[BOX_COUNT];
    Message slots[TOTAL_SLOTS];
#ifdef SEM_UNNAMED
    sem_t sems[SEM_TOTAL];    // unnamed process-shared semaphores
#endif
} SharedArea;

static SharedArea *area = NULL;     // mapping of the shared memory in this process
static char shm_name[96];           // name of the shared memory object
static char prefix[64];             // prefix of all names of this session
static __attribute__((unused)) char session_name[SESSION_LEN] = "main"; // session (independent version)
static int is_server = 0;           // 1 in the clock process (it removes the objects)
static Role self_role = ROLE_CLOCK; // role of this process
static int self_idx = 0;            // team / call index of this process
static pid_t server_pid = 0;        // pid of the process that created the objects

/** @brief Sets the role and index of the calling process
 *  @param role The role of the calling process
 *  @param idx The index of the role (for a team)
 */
void ipc_set_self(Role role, int idx)
{
    self_role = role;
    self_idx = idx;
}

/** @brief Returns the role of the calling process
 *  @return The role of the calling process
 */
Role ipc_self_role(void)
{
    return self_role;
}

/** @brief Returns the index of the calling process
 *  @return The index of the calling process
 */
int ipc_self_idx(void)
{
    return self_idx;
}

/** @brief Remembers the configuration path for the calling process
 *  @param path The path to the configuration file
 */
void ipc_remember_config(const char *path)
{
    (void)path; // the clients read the model from the shared memory
}

/** @brief Returns the flavour of the IPC implementation
 *  @return The flavour of the IPC implementation
 */
const char *ipc_flavour(void)
{
#if defined(IPC_FORK) && defined(SEM_UNNAMED)
    return "fork + unnamed POSIX sems in POSIX shm";
#elif defined(IPC_FORK)
    return "fork + named POSIX sems + POSIX shm";
#else
    return "independent + named POSIX sems + POSIX shm";
#endif
}
/** @brief Returns the layout of the calls
 *  @return The layout of the calls
 */
const char *ipc_calls_layout(void)
{
#ifdef IPC_FORK
    return "1 process per call";
#else
    return "1 callers process for all calls";
#endif
}

/**
 * @brief Sleeps for some milliseconds
 * @param ms The number of milliseconds to sleep
 */
static __attribute__((unused)) void sleep_ms(int ms)
{
    struct timespec pause = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

/**
 * @brief Number of the mailbox of an address
 * @param kind The kind of the address
 */
static int box_index(BoxKind kind, int idx)
{
    switch (kind)
    {
    case BOX_DISPATCHER:
        return BOX_IDX_DISPATCHER;
    case BOX_TEAM:
        return BOX_IDX_TEAM0 + idx;
    case BOX_CALL:
#ifdef IPC_FORK
        return BOX_IDX_CALL0 + idx;
#else
        (void)idx;
        return BOX_IDX_CALLERS; // one process plays all calls
#endif
    default:
        return -1;
    }
}

/**
 * @brief Number of the mailbox of the calling process
 * @return The index of the mailbox
 */
static int own_box(void)
{
    switch (self_role)
    {
    case ROLE_DISPATCHER:
        return BOX_IDX_DISPATCHER;
    case ROLE_TEAM:
        return BOX_IDX_TEAM0 + self_idx;
    case ROLE_CALL:
        return BOX_IDX_CALL0 + self_idx;
    case ROLE_CALLERS:
        return BOX_IDX_CALLERS;
    default:
        return -1;
    }
}

/** @brief Locks the world (server)
 *  @param sim The simulation
 */
void world_lock(Simulation *sim)
{
    (void)sim;
    sem_wait_retry(sem_ref(SEM_WORLD));
}

/** @brief Unlocks the world (server)
 *  @param sim The simulation
 */
void world_unlock(Simulation *sim)
{
    (void)sim;
    sem_post(sem_ref(SEM_WORLD));
}

/** @brief Locks the output (server)
 *  @param sim The simulation
 */
void ipc_output_lock(void)
{
    sem_wait_retry(sem_ref(SEM_OUTPUT)); // NULL before the semaphores exist: no lock needed
}

/** @brief Unlocks the output (server)
 *  @param sim The simulation
 */
void ipc_output_unlock(void)
{
    sem_t *sem = sem_ref(SEM_OUTPUT);
    if (sem)
        sem_post(sem);
}

/**
 * @brief Creates the semaphores of a mailbox and empties it
 * @param box The index of the mailbox
 * @return 1 on success, 0 on failure
 */
static int box_create(int box)
{
    BoxHeader *header = &area->boxes[box];
    header->head = 0;
    header->count = 0;

    return sems_create(SEM_BOX_LOCK(box), 1) && sems_create(SEM_BOX_FILLED(box), 0) &&
           sems_create(SEM_BOX_FREE(box), (unsigned)header->capacity);
}

/**
 * @brief Puts a message into a mailbox, waits if the box is full (producer)
 * @param box The index of the mailbox
 * @param message The message to put
 * @return 1 on success, 0 on failure
 */
static int mailbox_put(int box, const Message *message)
{
    SOFT_ASSERT(box >= 0 && box < BOX_COUNT, "mailbox index is out of range", 0);

    sem_t *free_places = sem_ref(SEM_BOX_FREE(box));
    sem_t *lock = sem_ref(SEM_BOX_LOCK(box));
    sem_t *filled = sem_ref(SEM_BOX_FILLED(box));
    SOFT_ASSERT(free_places && lock && filled, "mailbox semaphores do not exist", 0);

    sem_wait_retry(free_places); // wait for a free place

    sem_wait_retry(lock);
    BoxHeader *header = &area->boxes[box];
    area->slots[header->offset + (header->head + header->count) % header->capacity] = *message;
    header->count++;
    sem_post(lock);

    sem_post(filled); // one more message for the consumer
    return 1;
}

/**
 * @brief Takes the oldest message of a mailbox, waits if it is empty
 * @details The message is removed from the mailbox (FIFO)
 * @param box The index of the mailbox
 * @return The message, or MSG_SHUTDOWN if the mailbox does not exist
 */
static Message mailbox_get(int box)
{
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = MSG_SHUTDOWN;

    sem_t *free_places = sem_ref(SEM_BOX_FREE(box));
    sem_t *lock = sem_ref(SEM_BOX_LOCK(box));
    sem_t *filled = sem_ref(SEM_BOX_FILLED(box));

    if (!free_places || !lock || !filled)
        return message;

    sem_wait_retry(filled); // wait for a message

    sem_wait_retry(lock);
    BoxHeader *header = &area->boxes[box];
    message = area->slots[header->offset + header->head];
    header->head = (header->head + 1) % header->capacity;
    header->count--;
    sem_post(lock);

    sem_post(free_places);
    return message;
}

/** @brief Adds a process to the quiet counter
 *  @param sim The simulation
 */
static void quiet_add(void)
{
    sem_wait_retry(sem_ref(SEM_QUIET_LOCK));
    area->pending++;
    sem_post(sem_ref(SEM_QUIET_LOCK));
}

/** @brief Removes a process from the quiet counter
 *  @param sim The simulation
 */
static void quiet_done(void)
{
    sem_wait_retry(sem_ref(SEM_QUIET_LOCK));
    area->pending--;

    if (area->pending == 0)
        sem_post(sem_ref(SEM_QUIET_IDLE)); // exactly once per phase

    sem_post(sem_ref(SEM_QUIET_LOCK));
}

/**
 * @brief Holds the quiet counter (server)
 * @param sim The simulation
 */
void ipc_quiet_hold(Simulation *sim)
{
    (void)sim;
    quiet_add(); // the counter cannot drop to 0 while the clock is still sending
}

/**
 * @brief Releases the quiet counter (server)
 * @param sim The simulation
 */
void ipc_quiet_release(Simulation *sim)
{
    (void)sim;
    quiet_done();
}

/**
 * @brief Checks that all processes of the simulation are alive
 * @param sim The simulation
 * @return 1 if all are alive, 0 if one of them has died (it is logged)
 */
static int processes_alive(Simulation *sim)
{
#ifdef IPC_FORK
    int status = 0;
    pid_t pid = waitpid(-1, &status, WNOHANG); // a child that has ended is a failure here

    if (pid <= 0)
        return 1;

    const char *what = "process";
    int number = 0;

    if (pid == sim->dispatcher_pid)
    {
        what = "dispatcher";
        sim->dispatcher_pid = 0;
    }

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].pid == pid)
        {
            what = "team B";
            number = sim->teams[idx].id;
            sim->teams[idx].started = 0;
            sim->teams[idx].pid = 0;
        }

    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].pid == pid)
        {
            what = "call #";
            number = sim->calls[idx].id;
            sim->calls[idx].started = 0;
            sim->calls[idx].pid = 0;
        }

    const char *how = WIFSIGNALED(status) ? "killed by signal" : "exit code";
    int code = WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status);

    if (number)
        log_event(sim, "PROCESS FAILURE: %s%d (pid %d) has terminated, %s %d", what, number, (int)pid, how, code);
    else
        log_event(sim, "PROCESS FAILURE: %s (pid %d) has terminated, %s %d", what, (int)pid, how, code);
    return 0;
#else
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
#endif
}

/**
 * @brief The clock waits until all sent messages are handled
 * @param sim The simulation
 * @return 1 when the phase is over, 0 if a process has died
 */
int ipc_quiet_wait(Simulation *sim)
{
    sem_t *idle = sem_ref(SEM_QUIET_IDLE);

    for (;;)
    {
        if (sem_wait_ms(idle, WAIT_STEP_MS) == 0)
            return 1;

        if (errno == EINTR) // a signal: the request is read at the next minute
            continue;

        if (!processes_alive(sim))
            return 0;
    }
}

/**
 * @brief Sends a message to a mailbox
 * @param sim The simulation
 * @param kind The kind of the mailbox
 * @param idx The index of the mailbox
 * @param message The message to send
 * @return 1 on success, 0 on failure
 */
int ipc_send(Simulation *sim, BoxKind kind, int idx, Message *message)
{
    SOFT_ASSERT(message, "message is NULL", 0);

    message->time = sim ? sim->now : 0;
    message->sender_role = self_role;
    message->sender_idx = self_idx;

    quiet_add(); // the message is registered before it can be handled
    return mailbox_put(box_index(kind, idx), message);
}

/**
 * @brief Sends a simple message to a mailbox
 * @param sim The simulation
 * @param kind The kind of the mailbox
 * @param idx The index of the mailbox
 * @param type The type of the message
 * @param event The event type (for MSG_WAKE / MSG_SCHEDULE)
 * @param arg The index of a call or a team
 * @param value An extra value
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
 * @brief Sends MSG_SHUTDOWN
 * @param box The index of the mailbox
 */
static void send_shutdown(int box)
{
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = MSG_SHUTDOWN;
    mailbox_put(box, &message);
}

/**
 * @brief Receives a message from the own mailbox
 * @param sim The simulation
 * @return The received message
 */
Message ipc_receive(Simulation *sim)
{
    (void)sim;
    return mailbox_get(own_box());
}

/**
 * @brief Cleans up the IPC resources
 * @param sim The simulation
 */
void ipc_done(Simulation *sim)
{
    (void)sim;
    quiet_done();
}

/**
 * @brief Schedules an event
 * @param sim The simulation
 * @param time The time of the event
 * @param type The type of the event
 * @param id The ID of the event
 * @return 1 on success, 0 on failure
 */
int ipc_schedule(Simulation *sim, int time, EventType type, int id)
{
    return schedule_event_checked(sim, time, type, id); // the world lock is held by the caller
}

/**
 * @brief Removes the deadlines for a call
 * @param sim The simulation
 * @param call_id The ID of the call
 * @return 1 on success, 0 on failure
 */
int ipc_remove_deadlines(Simulation *sim, int call_id)
{
    return remove_call_deadlines(sim, call_id);
}

/**
 * @brief Publishes a call in the shared memory
 * @param sim The simulation
 * @param call_idx The index of the call
 */
void ipc_publish_call(Simulation *sim, int call_idx)
{
    (void)sim;
    (void)call_idx; // the record is in the shared memory, everybody sees it
}

/**
 * @brief Publishes a team in the shared memory
 * @param sim The simulation
 * @param team_idx The index of the team
 */
void ipc_publish_team(Simulation *sim, int team_idx)
{
    (void)sim;
    (void)team_idx;
}

/**
 * @brief Applies a message to the simulation
 * @param sim The simulation
 * @param message The message to apply
 */
void ipc_apply(Simulation *sim, const Message *message)
{
    (void)sim;
    (void)message;
}


/**
 * @brief Places the mailboxes in the array of slots
 */
static void layout_boxes(void)
{
    int offset = 0;

    for (int box = 0; box < BOX_COUNT; box++)
    {
        int capacity = ENTITY_BOX_SIZE;

        if (box == BOX_IDX_DISPATCHER)
            capacity = DISPATCHER_BOX_SIZE;
        else if (box == BOX_IDX_CALLERS)
            capacity = CALLERS_BOX_SIZE;

        area->boxes[box].offset = offset;
        area->boxes[box].capacity = capacity;
        offset += capacity;
    }
}

/**
 * @brief Creates the shared memory, the semaphores and copies the model
 * @param loaded The model loaded from the configuration file
 * @param session Session name (independent version)
 * @return Pointer to the model in the shared memory, NULL on failure
 */
Simulation *ipc_server_init(const Simulation *loaded, const char *session)
{
    SOFT_ASSERT(loaded, "model is NULL", NULL);

#ifdef IPC_FORK
    (void)session;
    // the name contains the pid: several fork versions can run at the same time
    snprintf(prefix, sizeof(prefix), "/ed35_f%d_", (int)getpid());
#else
    snprintf(session_name, sizeof(session_name), "%s", session ? session : "main");
    snprintf(prefix, sizeof(prefix), "/ed35_%s_", session_name);
#endif
    snprintf(shm_name, sizeof(shm_name), "%sshm", prefix);

    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);

    if (fd < 0 && errno == EEXIST)
    {
        // an object of a crashed run (SIGKILL) or of a running server
        fprintf(stderr, "Warning: %s already exists, it is replaced (is another server running?)\n", shm_name);
        shm_unlink(shm_name);
        fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    }

    if (fd < 0)
    {
        perror("shm_open");
        return NULL;
    }

    is_server = 1;
    server_pid = getpid();
    atexit(ipc_server_cleanup);

    if (ftruncate(fd, sizeof(SharedArea)) != 0)
    {
        perror("ftruncate");
        close(fd);
        return NULL;
    }

    area = mmap(NULL, sizeof(SharedArea), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); // the mapping stays valid after close()

    if (area == MAP_FAILED)
    {
        area = NULL;
        perror("mmap");
        return NULL;
    }

    memset(area, 0, sizeof(*area));
    area->sim = *loaded;
    area->sim.clock_pid = getpid();
    area->sim.processes_started = 1; // the clock itself
    layout_boxes();

#ifdef SEM_UNNAMED
    sems_setup(prefix, area->sems);
#else
    sems_setup(prefix, NULL);
#endif

    int ok = sems_create(SEM_WORLD, 1) && sems_create(SEM_OUTPUT, 1) && sems_create(SEM_QUIET_LOCK, 1) &&
             sems_create(SEM_QUIET_IDLE, 0) && box_create(BOX_IDX_DISPATCHER);

    for (int idx = 0; ok && idx < loaded->team_count; idx++)
        ok = box_create(BOX_IDX_TEAM0 + idx);

#ifdef IPC_SHM_INDEPENDENT
    ok = ok && sems_create(SEM_READY, 0) && sems_create(SEM_EXITED, 0) && box_create(BOX_IDX_CALLERS);
#endif
    // in the fork versions the mailboxes of the calls are created when the call process starts

    if (!ok)
        return NULL;

    area->magic = SHM_MAGIC; // everything in the shared memory is ready

#ifdef IPC_SHM_INDEPENDENT
    // the claim semaphores are created last: the clients start when they see them
    // (if they were created before the magic number, a fast client could find
    // the memory "not ready" and give up)
    ok = sems_create(SEM_CLAIM_DISPATCHER, 1) && sems_create(SEM_CLAIM_CALLERS, 1);
    for (int idx = 0; ok && idx < loaded->team_count; idx++)
        ok = sems_create(SEM_CLAIM_TEAM0 + idx, 1);
#endif

    return ok ? &area->sim : NULL;
}

/**
 * @brief Removes the shared memory and the semaphores
 */
void ipc_server_cleanup(void)
{
    if (!is_server || getpid() != server_pid)
        return; // a forked child must not remove the objects of the parent

    is_server = 0;

    sems_destroy_all();

    if (area)
    {
        munmap(area, sizeof(SharedArea));
        area = NULL;
    }

    shm_unlink(shm_name);
}

#ifdef IPC_FORK

/**
 * @brief Body of a forked child: plays one role and ends with _exit()
 * @param sim The simulation
 * @param role The role of the child
 * @param idx The index of the role (for a team)
 */
static void child_main(Simulation *sim, Role role, int idx)
{
    is_server = 0; // the child must not remove the objects of the parent
    ipc_set_self(role, idx);
    signals_install_child(sim->clock_pid);

    role_loop(sim, role, idx);

    _exit(0); // no atexit() handlers of the parent, no flushing of its buffers
}

/**
 * @brief Forks a process for a role
 * @param sim The simulation
 * @param role The role of the child
 * @param idx The index of the role (for a team)
 * @return Pid of the child, -1 on failure
 */
static pid_t spawn(Simulation *sim, Role role, int idx)
{
    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return -1;
    }

    if (pid == 0)
        child_main(sim, role, idx); // does not return

    sim->processes_started++;
    return pid;
}

/**
 * @brief Starts the processes for each role
 * @param sim The simulation
 * @return 1 on success, 0 on failure
 */
int ipc_start_processes(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    sim->dispatcher_pid = spawn(sim, ROLE_DISPATCHER, 0);
    SOFT_ASSERT(sim->dispatcher_pid > 0, "cannot start the dispatcher", 0);

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        pid_t pid = spawn(sim, ROLE_TEAM, idx);
        SOFT_ASSERT(pid > 0, "cannot start a team process", 0);

        sim->teams[idx].pid = pid;
        sim->teams[idx].started = 1;
    }

    return 1;
}

/**
 * @brief Starts a call process
 * @param sim The simulation
 * @param call_idx The index of the call
 * @return 1 on success, 0 on failure
 */
int ipc_start_call(Simulation *sim, int call_idx)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(call_idx >= 0 && call_idx < MAX_CALLS, "call index is out of range", 0);

    static unsigned char box_exists[MAX_CALLS]; // the semaphores of the box are created once

    int box = BOX_IDX_CALL0 + call_idx;

    if (!box_exists[call_idx])
    {
        SOFT_ASSERT(box_create(box), "cannot create the mailbox of a call", 0);
        box_exists[call_idx] = 1;
    }

    pid_t pid = spawn(sim, ROLE_CALL, call_idx);
    if (pid < 0)
        return 0;

    sim->calls[call_idx].pid = pid;
    sim->calls[call_idx].started = 1;
    return 1;
}

/**
 * @brief Stops a call process
 * @param sim The simulation
 * @param call_idx The index of the call
 * @return 1 on success, 0 on failure
 */
int ipc_stop_call(Simulation *sim, int call_idx)
{
    Call *call = &sim->calls[call_idx];

    if (!call->started)
        return 1;

    send_shutdown(BOX_IDX_CALL0 + call_idx);
    waitpid(call->pid, NULL, 0);

    call->started = 0;
    call->pid = 0;
    return 1;
}

/**
 * @brief Stops all processes
 * @param sim The simulation
 */
void ipc_stop_all(Simulation *sim)
{
    if (sim == NULL || area == NULL)
        return;

    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].started)
            send_shutdown(BOX_IDX_CALL0 + idx);

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].started)
            send_shutdown(BOX_IDX_TEAM0 + idx);

    if (sim->dispatcher_pid > 0)
        send_shutdown(BOX_IDX_DISPATCHER);

    // wait for every child: no zombie processes stay
    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].started)
        {
            waitpid(sim->calls[idx].pid, NULL, 0);
            sim->calls[idx].started = 0;
        }

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].started)
        {
            waitpid(sim->teams[idx].pid, NULL, 0);
            sim->teams[idx].started = 0;
        }

    if (sim->dispatcher_pid > 0)
        waitpid(sim->dispatcher_pid, NULL, 0);

    while (waitpid(-1, NULL, WNOHANG) > 0) // children that ended in another way
        ;
}

/**
 * @brief Attaches to the simulation as a client
 * @param role The role of the client
 * @param idx The index of the role (for a team)
 * @param session The session name
 * @param wait_seconds The number of seconds to wait for the processes to start
 * @return A pointer to the simulation, or NULL on failure
 */
Simulation *ipc_client_attach(Role role, int *idx, const char *session, int wait_seconds)
{
    (void)role;
    (void)idx;
    (void)session;
    (void)wait_seconds;
    return NULL; // the fork versions have no independent clients
}

/**
 * @brief Detaches from the simulation as a client
 * @param sim The simulation
 */
void ipc_client_detach(Simulation *sim)
{
    (void)sim;
}

#endif /* IPC_FORK */

#ifdef IPC_SHM_INDEPENDENT

/**
 * @brief Prints which roles are still not played by a process
 * @param sim The simulation
 */
static void print_missing(const Simulation *sim)
{
    char line[OUT_BUF_LEN];
    int len = snprintf(line, sizeof(line), "[clock] waiting for:");

    world_lock((Simulation *)sim);

    if (sim->dispatcher_pid == 0)
        len += snprintf(line + len, sizeof(line) - (size_t)len, " dispatcher");

    if (sim->callers_pid == 0)
        len += snprintf(line + len, sizeof(line) - (size_t)len, " callers");

    for (int idx = 0; idx < sim->team_count && len < (int)sizeof(line) - 16; idx++)
        if (!sim->teams[idx].started)
            len += snprintf(line + len, sizeof(line) - (size_t)len, " team(B%d)", sim->teams[idx].id);

    world_unlock((Simulation *)sim);

    fd_printf(STDOUT_FILENO, "%s\n", line);
}

/**
 * @brief Waits until all processes have started
 * @param sim The simulation
 * @return 1 on success, 0 on failure
 */
int ipc_start_processes(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    int needed = 2 + sim->team_count; // dispatcher + callers + teams
    int ready = 0;

    fd_printf(STDOUT_FILENO,
              "[clock] pid %d, session '%s'. Start the other programs in any terminals:\n"
              "        ./bin/ed_shm_dispatcher -n %s\n"
              "        ./bin/ed_shm_callers    -n %s\n"
              "        ./bin/ed_shm_team       -n %s    (%d times, or with -i <team id>)\n",
              (int)getpid(), session_name, session_name, session_name, session_name, sim->team_count);

    int seconds = 0;

    while (ready < needed)
    {
        if (signals_stop_requested())
        {
            fd_printf(STDOUT_FILENO, "[clock] interrupted while waiting for the processes\n");
            return 0;
        }

        if (sem_wait_ms(sem_ref(SEM_READY), WAIT_STEP_MS) == 0)
        {
            ready++;
            fd_printf(STDOUT_FILENO, "[clock] registered %d of %d processes\n", ready, needed);
            continue;
        }

        if (!processes_alive(sim))
            return 0;

        if (++seconds % 5 == 0)
            print_missing(sim);
    }

    return 1;
}

/**
 * @brief Starts a call (the caller's family plays the call)
 * @param sim The simulation
 * @param call_idx The index of the call
 * @return 1 on success, 0 on failure
 */
int ipc_start_call(Simulation *sim, int call_idx)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    // the callers family plays the call: it reads the record from the shared memory
    sim->calls[call_idx].pid = sim->callers_pid;
    sim->calls[call_idx].started = 1;
    return 1;
}

/**
 * @brief Stops a call process
 * @param sim The simulation
 * @param call_idx The index of the call
 * @return 1 on success, 0 on failure
 */
int ipc_stop_call(Simulation *sim, int call_idx)
{
    sim->calls[call_idx].started = 0; // the family process goes on, only the slot is freed
    return 1;
}

/**
 * @brief Stops all processes
 * @param sim The simulation
 */
void ipc_stop_all(Simulation *sim)
{
    if (sim == NULL || area == NULL)
        return;

    int running = 0;

    if (sim->dispatcher_pid > 0)
    {
        send_shutdown(BOX_IDX_DISPATCHER);
        running++;
    }

    if (sim->callers_pid > 0)
    {
        send_shutdown(BOX_IDX_CALLERS);
        running++;
    }

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].pid > 0)
        {
            send_shutdown(BOX_IDX_TEAM0 + idx);
            running++;
        }

    // every client posts SEM_EXITED before it detaches
    for (int done = 0; done < running; done++)
        if (sem_wait_ms(sem_ref(SEM_EXITED), EXIT_WAIT_MS) != 0)
        {
            fd_printf(STDOUT_FILENO, "[clock] %d process(es) did not finish in time\n", running - done);
            break;
        }
}

/**
 * @brief Takes one "claim" semaphore without waiting
 * @param id The index of the claim semaphore
 * @return 1 if the role has been taken by this process
 */
static int try_claim(int id)
{
    sem_t *sem = sem_ref(id);
    return sem != NULL && sem_trywait(sem) == 0;
}

/**
 * @brief Attaches an independent client to the session
 * @param role Role of the client
 * @param idx In: requested team id (0 = any), out: team index
 * @param session Session name
 * @param wait_seconds How long to wait for the server
 * @return Pointer to the model in the shared memory, NULL on failure
 */
Simulation *ipc_client_attach(Role role, int *idx, const char *session, int wait_seconds)
{
    snprintf(prefix, sizeof(prefix), "/ed35_%s_", session ? session : "main");
    snprintf(shm_name, sizeof(shm_name), "%sshm", prefix);
    sems_setup(prefix, NULL);

    // 1. wait until the server has created the objects (the claim semaphores are created last)
    int waited_ms = 0;
    int said = 0;

    while (!sems_exist(SEM_CLAIM_DISPATCHER))
    {
        if (!said)
        {
            fd_printf(STDOUT_FILENO, "waiting for the clock (session '%s')...\n", session);
            said = 1;
        }

        if (signals_stop_requested() || waited_ms >= wait_seconds * 1000)
        {
            fprintf(stderr, "The clock of session '%s' is not running\n", session);
            return NULL;
        }

        sleep_ms(200);
        waited_ms += 200;
    }

    // 2. map the shared memory
    int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0)
    {
        perror("shm_open");
        return NULL;
    }

    area = mmap(NULL, sizeof(SharedArea), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (area == MAP_FAILED || area->magic != SHM_MAGIC)
    {
        area = NULL;
        fprintf(stderr, "The shared memory of session '%s' is not ready\n", session);
        return NULL;
    }

    Simulation *sim = &area->sim;

    // 3. claim the role: a claim semaphore has the value 1 while the role is free
    int claimed = 0;

    if (role == ROLE_DISPATCHER)
        claimed = try_claim(SEM_CLAIM_DISPATCHER);
    else if (role == ROLE_CALLERS)
        claimed = try_claim(SEM_CLAIM_CALLERS);
    else if (role == ROLE_TEAM)
    {
        int wanted_id = *idx;

        for (int team = 0; team < sim->team_count && !claimed; team++)
        {
            if (wanted_id > 0 && sim->teams[team].id != wanted_id)
                continue;

            if (try_claim(SEM_CLAIM_TEAM0 + team))
            {
                *idx = team;
                claimed = 1;
            }
        }
    }

    if (!claimed)
    {
        fprintf(stderr, "No free role for this process: it is already played by another process\n");
        munmap(area, sizeof(SharedArea));
        area = NULL;
        return NULL;
    }

    // 4. registration in the model
    ipc_set_self(role, role == ROLE_TEAM ? *idx : 0);

    world_lock(sim);

    if (role == ROLE_DISPATCHER)
        sim->dispatcher_pid = getpid();
    else if (role == ROLE_CALLERS)
        sim->callers_pid = getpid();
    else
    {
        sim->teams[*idx].pid = getpid();
        sim->teams[*idx].started = 1;
    }

    sim->processes_started++;
    area->registered++;

    world_unlock(sim);

    sem_post(sem_ref(SEM_READY)); // the clock counts the registered processes
    return sim;
}

/**
 * @brief Tells the clock that the client has finished and unmaps the memory
 * @param sim The simulation
 */
void ipc_client_detach(Simulation *sim)
{
    (void)sim;

    sem_t *exited = sem_ref(SEM_EXITED);
    if (exited)
        sem_post(exited);

    sems_destroy_all(); // only closes: a client creates nothing

    if (area)
    {
        munmap(area, sizeof(SharedArea));
        area = NULL;
    }
}

#endif /* IPC_SHM_INDEPENDENT */
