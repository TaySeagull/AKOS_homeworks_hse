#define _XOPEN_SOURCE 700

#include "ipc.h"
#include "config.h"
#include "event_queue.h"
#include "net.h"
#include "signals.h"
#include "simulation.h"
#include "utils.h"
#include "utils_extra.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef IPC_NET
#error "ipc_net.c is only for the network version"
#endif

#define SLOT_DISPATCHER 0
#define SLOT_CALLERS 1
#define SLOT_TEAM0 2
#define MAX_OBSERVERS 16
#define SLOT_OBSERVER0 (SLOT_TEAM0 + MAX_TEAMS)
#define SLOT_COUNT (SLOT_OBSERVER0 + MAX_OBSERVERS)
#define CONN_COUNT (SLOT_COUNT + 8) // + connections that have not chosen a role yet

#define PUMP_STEP_MS 100   // step of the waiting loops of the server
#define EXIT_WAIT_MS 5000  // how long the server waits for the statistics of the clients
#define NOTE_EVERY_MS 3000 // how often the server says which client it is waiting for
#define OBS_QUEUE_LIMIT 512 // frames kept for an observer that does not read fast enough
#define LISTEN_BACKLOG 16

/**
 * @struct Frame
 * @brief One frame in the queue of a role: the bytes are ready to be written
 */
typedef struct Frame
{
    struct Frame *next;
    size_t length;      // bytes of the whole frame
    size_t sent;        // bytes already written into the socket
    int counted;        // 1 = the client answers with MSG_DONE, until then it stays here
    unsigned char data[];
} Frame;

/**
 * @struct Queue
 * @brief Queue of frames of one role (FIFO)
 */
typedef struct
{
    Frame *head;
    Frame *tail;
    int count;
} Queue;

/**
 * @struct Conn
 * @brief One TCP connection of the server
 */
typedef struct
{
    int fd;                  // -1 = free
    int slot;                // role slot, -1 = the client has not registered yet
    int role;                // Role, -1 = unknown
    NetInput input;          // bytes of an unfinished frame
    char name[NET_ADDR_LEN]; // address of the client, for the log
} Conn;

/**
 * @struct Slot
 * @brief One role of the application as the server sees it
 */
typedef struct
{
    int online;        // 1 = a client plays this role right now
    int ever;          // 1 = the role has been claimed at least once
    int conn;          // index of the connection or -1
    Queue out;         // messages for this role
    Counters counters; // last known statistics of the client of this role
} Slot;

static Simulation model;    // server: the model; client: the mirror
static Role self_role = ROLE_CLOCK;
static int self_idx = 0;
static int is_server = 0;
static pid_t server_pid = 0;

static char config_path[LINE_LEN];               // absolute path of the configuration
static char endpoint_host[NET_HOST_LEN] = "";    // -b (server) / -H (client)
static int endpoint_port = NET_DEFAULT_PORT;     // -p
static int lost_timeout = 0;                     // -T, seconds, 0 = wait without a limit
static char flavour_text[128] = "client-server over TCP";

// SERVER
static int listen_fd = -1;
static Conn conns[CONN_COUNT];
static Slot slots[SLOT_COUNT];
static NetFrame incoming;       // frame that is being read (16 KiB, not on the stack)
static unsigned char payload[NET_FRAME_MAX];            // payload being packed
static unsigned char build[NET_HEADER_LEN + NET_FRAME_MAX]; // whole frame being built
static int stats_received = 0;
static int shutting_down = 0;
static int counters_merged = 0; // 1 = the statistics of the clients are already in the model

// CLIENT
static int sock = -1;
static NetInput sock_input;
static int server_lost = 0; // 1 = the connection broke before the end of the shift
static int stop_sent = 0;   // the request of the user has already been sent
static int close_sent = 0;

/**
 * @brief Sets the role and index of this program
 */
void ipc_set_self(Role role, int idx)
{
    self_role = role;
    self_idx = idx;
}

/**
 * @brief Role of this program
 */
Role ipc_self_role(void)
{
    return self_role;
}

/**
 * @brief Index of this program (team index)
 */
int ipc_self_idx(void)
{
    return self_idx;
}

/**
 * @brief Name of the IPC scheme for the header and the summary
 */
const char *ipc_flavour(void)
{
    return flavour_text;
}

/**
 * @brief How the calls are played
 */
const char *ipc_calls_layout(void)
{
    return "1 callers client for all calls";
}

/**
 * @brief Address of the server (command line)
 * @param host Address to bind (server) or to connect to (client), NULL = default
 * @param port TCP port
 */
void ipc_set_endpoint(const char *host, int port)
{
    if (host != NULL)
        snprintf(endpoint_host, sizeof(endpoint_host), "%s", host);

    if (port > 0)
        endpoint_port = port;
}

/**
 * @brief How long the server waits for a client that is gone
 * @param seconds 0 = without a limit
 */
void ipc_set_lost_timeout(int seconds)
{
    lost_timeout = seconds > 0 ? seconds : 0;
}

/**
 * @brief 1 if the connection to the server was lost
 */
int ipc_server_lost(void)
{
    return server_lost;
}

/**
 * @brief Remembers the path of the configuration file (sent to the clients)
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
 * @brief The model of every program is its own: nothing to lock
 */
void world_lock(Simulation *sim)
{
    (void)sim;
}

/**
 * @brief The model of every program is its own: nothing to unlock
 */
void world_unlock(Simulation *sim)
{
    (void)sim;
}

/**
 * @brief Every program writes into its own terminal: no common lock is needed
 */
void ipc_output_lock(void)
{
}

/**
 * @brief Counterpart of ipc_output_lock()
 */
void ipc_output_unlock(void)
{
}

/**
 * @brief Copies the attachments of a message into the mirror of this program
 * @param sim The model
 * @param message The received message
 */
void ipc_apply(Simulation *sim, const Message *message)
{
    if (sim == NULL || message == NULL)
        return;

    // the server owns the model time, the clients take it from every message
    if (self_role != ROLE_CLOCK)
        sim->now = message->time;

    if (message->call_count > sim->call_count && message->call_count <= MAX_CALLS)
        sim->call_count = message->call_count;

    if (message->call_idx >= 0 && message->call_idx < MAX_CALLS)
    {
        Call *call = &sim->calls[message->call_idx];
        pid_t pid = call->pid;
        int started = call->started; // information about the process is local

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
 * @brief 1 if the slot belongs to an observer
 */
static int observer_slot(int slot)
{
    return slot >= SLOT_OBSERVER0 && slot < SLOT_COUNT;
}

/**
 * @brief Name of a kind of message, for the log and for the console
 * @param type MsgType
 * @return Constant string
 */
static const char *message_name(int type)
{
    static const char *name[] = {"MSG_WAKE",    "MSG_NEW_CALL", "MSG_CALL_CHANGED",     "MSG_TEAM_FREE",
                                 "MSG_ROUND",   "MSG_ASSIGN",   "MSG_CALL_FINISHED",    "MSG_SHUTDOWN",
                                 "MSG_SYNC",    "MSG_SCHEDULE", "MSG_REMOVE_DEADLINES", "MSG_DONE",
                                 "MSG_HELLO",   "MSG_WELCOME",  "MSG_STATS",            "MSG_RESYNC"};

    return (type >= 0 && type < (int)(sizeof(name) / sizeof(name[0]))) ? name[type] : "?";
}

/**
 * @brief Kind of message that is in a frame of a queue
 * @param frame The frame (its header is followed by the packed message)
 * @return Constant string
 */
static const char *frame_message_name(const Frame *frame)
{
    if (frame == NULL || frame->length < NET_HEADER_LEN + 4)
        return "?";

    const unsigned char *body = frame->data + NET_HEADER_LEN;
    int type = (int)((uint32_t)body[0] << 24 | (uint32_t)body[1] << 16 | (uint32_t)body[2] << 8 | body[3]);

    return message_name(type);
}

/**
 * @brief Name of a role for the log and for the console
 * @param slot Number of the slot
 * @return Constant string
 */
static const char *slot_label(int slot)
{
    static char text[32];

    if (slot == SLOT_DISPATCHER)
        return "dispatcher";

    if (slot == SLOT_CALLERS)
        return "callers";

    if (observer_slot(slot))
    {
        snprintf(text, sizeof(text), "observer %d", slot - SLOT_OBSERVER0 + 1);
        return text;
    }

    int idx = slot - SLOT_TEAM0;
    snprintf(text, sizeof(text), "team B%d", idx >= 0 && idx < MAX_TEAMS ? model.teams[idx].id : 0);

    return text;
}

/**
 * @brief Puts a frame at the end of a queue
 */
static void queue_push(Queue *queue, Frame *frame)
{
    frame->next = NULL;

    if (queue->tail != NULL)
        queue->tail->next = frame;
    else
        queue->head = frame;

    queue->tail = frame;
    queue->count++;
}

/**
 * @brief Puts a frame at the head of a queue
 * @details The answer to a registration has to overtake the messages that are
 * waiting for the role: a client expects the welcome and the state first.
 */
static void queue_push_front(Queue *queue, Frame *frame)
{
    frame->next = queue->head;
    queue->head = frame;

    if (queue->tail == NULL)
        queue->tail = frame;

    queue->count++;
}

/**
 * @brief Takes the first frame out of a queue
 * @return The frame or NULL
 */
static Frame *queue_pop(Queue *queue)
{
    Frame *frame = queue->head;

    if (frame == NULL)
        return NULL;

    queue->head = frame->next;

    if (queue->head == NULL)
        queue->tail = NULL;

    queue->count--;
    frame->next = NULL;

    return frame;
}

/**
 * @brief Removes every frame of a queue
 */
static void queue_clear(Queue *queue)
{
    Frame *frame = NULL;

    while ((frame = queue_pop(queue)) != NULL)
        free(frame);
}

/**
 * @brief 1 if the queue of a slot has bytes that are not written yet
 */
static int slot_has_unsent(int slot)
{
    if (slot < 0 || slot >= SLOT_COUNT)
        return 0;

    for (Frame *frame = slots[slot].out.head; frame != NULL; frame = frame->next)
        if (frame->sent < frame->length)
            return 1;

    return 0;
}

/**
 * @brief Removes the frames at the head of a queue that are written and need no answer
 */
static void prune_sent(Queue *queue)
{
    while (queue->head != NULL && queue->head->sent == queue->head->length && !queue->head->counted)
        free(queue_pop(queue));
}

/**
 * @brief Writes as much of the queue of a slot into the socket as the socket takes
 * @param slot Number of the slot
 */
static void drop_conn(int idx, const char *why);

static void flush_slot(int slot)
{
    if (slot < 0 || slot >= SLOT_COUNT || !slots[slot].online)
        return;

    int conn = slots[slot].conn;

    if (conn < 0 || conns[conn].fd < 0)
        return;

    for (Frame *frame = slots[slot].out.head; frame != NULL; frame = frame->next)
    {
        while (frame->sent < frame->length)
        {
            ssize_t written = write(conns[conn].fd, frame->data + frame->sent, frame->length - frame->sent);

            if (written > 0)
            {
                frame->sent += (size_t)written;
                continue;
            }

            if (written < 0 && errno == EINTR)
                continue;

            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return; // the socket is full, the rest goes out on the next POLLOUT

            drop_conn(conn, "the connection cannot be written any more");
            return;
        }
    }

    prune_sent(&slots[slot].out);
}

/**
 * @brief Puts a frame into the queue of a slot and tries to write it at once
 * @param slot Number of the slot
 * @param kind NetKind
 * @param counted 1 = the client must answer with MSG_DONE
 * @param front 1 = before everything that is already waiting (registration)
 * @param dest_kind Address of the receiver (BoxKind) or -1
 * @param dest_idx Index of the receiver
 * @param body The payload
 * @param length Bytes of the payload
 * @return 1 on success, 0 if the frame could not be built
 */
static int enqueue(int slot, int kind, int counted, int front, int dest_kind, int dest_idx,
                   const unsigned char *body, size_t length)
{
    if (slot < 0 || slot >= SLOT_COUNT)
        return 0;

    // an observer that does not read is not allowed to eat the memory of the server
    if (observer_slot(slot) && slots[slot].out.count >= OBS_QUEUE_LIMIT)
    {
        while (slots[slot].out.count >= OBS_QUEUE_LIMIT && slots[slot].out.head != NULL &&
               slots[slot].out.head->sent == 0)
            free(queue_pop(&slots[slot].out));
    }

    size_t bytes = net_build_frame(build, sizeof(build), kind, counted ? NET_FLAG_COUNTED : 0, dest_kind, dest_idx,
                                   body, length);

    if (bytes == 0)
    {
        fprintf(stderr, "a frame of %zu bytes does not fit into the protocol\n", length);
        return 0;
    }

    Frame *frame = malloc(sizeof(Frame) + bytes);

    if (frame == NULL)
    {
        fprintf(stderr, "out of memory for a frame\n");
        return 0;
    }

    frame->next = NULL;
    frame->length = bytes;
    frame->sent = 0;
    frame->counted = counted;
    memcpy(frame->data, build, bytes);

    if (front)
    {
        // the frames of a registration are inserted in the reverse order and are
        // written only when all of them are in the queue (see handle_hello)
        queue_push_front(&slots[slot].out, frame);
        return 1;
    }

    queue_push(&slots[slot].out, frame);
    flush_slot(slot);

    return 1;
}

/**
 * @brief Sends a message to a role (server side)
 * @param slot Number of the slot
 * @param dest_kind Address of the receiver
 * @param dest_idx Index of the receiver
 * @param message The message
 * @param front 1 = before everything that is already waiting
 * @return 1 on success
 */
static int enqueue_message(int slot, int dest_kind, int dest_idx, const Message *message, int front)
{
    size_t length = net_pack_message(payload, sizeof(payload), message);

    if (length == 0)
        return 0;

    // every message except the shutdown and the welcome is answered with MSG_DONE
    int counted = message->type != MSG_SHUTDOWN && message->type != MSG_WELCOME;

    return enqueue(slot, NET_MESSAGE, counted, front, dest_kind, dest_idx, payload, length);
}

/**
 * @brief Number of the slot of an address
 * @param kind BoxKind
 * @param idx Index inside the kind
 * @return Number of the slot, -1 = the server itself
 */
static int slot_of_dest(int kind, int idx)
{
    switch (kind)
    {
    case BOX_DISPATCHER:
        return SLOT_DISPATCHER;

    case BOX_TEAM:
        return (idx >= 0 && idx < MAX_TEAMS) ? SLOT_TEAM0 + idx : -1;

    case BOX_CALL:
        return SLOT_CALLERS; // the callers client plays every call

    default:
        return -1; // BOX_CLOCK: the server itself
    }
}

/**
 * @brief Keeps the best known statistics of a role
 * @param slot Number of the slot
 * @param counters The counters that came with a message
 */
static void remember_counters(int slot, const Counters *counters)
{
    if (slot < 0 || slot >= SLOT_COUNT || observer_slot(slot))
        return;

    Counters *known = &slots[slot].counters;

    if (counters->total_wait > known->total_wait)
        known->total_wait = counters->total_wait;

    if (counters->total_service > known->total_service)
        known->total_service = counters->total_service;

    if (counters->served > known->served)
        known->served = counters->served;

    if (counters->cancelled > known->cancelled)
        known->cancelled = counters->cancelled;

    if (counters->unserved > known->unserved)
        known->unserved = counters->unserved;

    if (counters->escalations > known->escalations)
        known->escalations = counters->escalations;

    if (counters->dispatches > known->dispatches)
        known->dispatches = counters->dispatches;

    if (counters->received > known->received)
        known->received = counters->received;
}

/**
 * @brief Statistics of the whole application: the counters of the server plus the
 * last known counters of every client
 * @param out Out: the sum
 */
static void total_counters(Counters *out)
{
    out->total_wait = model.total_wait;
    out->total_service = model.total_service;
    out->served = model.served;
    out->cancelled = model.cancelled;
    out->unserved = model.unserved;
    out->escalations = model.escalations;
    out->dispatches = model.dispatches;
    out->received = model.received;

    // after the end of the shift the counters of the clients are already added
    // to the model (see ipc_stop_all), adding them twice would be wrong
    if (counters_merged)
        return;

    for (int slot = 0; slot < SLOT_OBSERVER0; slot++)
    {
        if (!slots[slot].ever)
            continue;

        const Counters *counters = &slots[slot].counters;

        out->total_wait += counters->total_wait;
        out->total_service += counters->total_service;
        out->served += counters->served;
        out->cancelled += counters->cancelled;
        out->unserved += counters->unserved;
        out->escalations += counters->escalations;
        out->dispatches += counters->dispatches;
        out->received += counters->received;
    }
}

/**
 * @brief Sends the whole state to one slot
 * @param slot Number of the slot
 * @param finished 1 = the shift is over
 * @param front 1 = before everything that is already waiting (registration)
 */
static void send_state(int slot, int finished, int front)
{
    static NetState state;

    net_state_from_model(&state, &model, finished);

    state.clients_online = 0;

    for (int idx = 0; idx < SLOT_OBSERVER0; idx++)
        if (slots[idx].online)
            state.clients_online++;

    state.observers_online = 0;

    for (int idx = SLOT_OBSERVER0; idx < SLOT_COUNT; idx++)
        if (slots[idx].online)
            state.observers_online++;

    if (observer_slot(slot))
        total_counters(&state.counters);
    else
        state.counters = slots[slot].counters;

    size_t length = net_pack_state(payload, sizeof(payload), &state);

    if (length > 0)
        enqueue(slot, NET_STATE, 0, front, -1, 0, payload, length);
}

/**
 * @brief Sends one line of the log to every observer
 * @param line The line (with the time and the tag of the program)
 */
static void fan_out_log(const char *line)
{
    size_t length = strlen(line);

    if (length > NET_FRAME_MAX)
        length = NET_FRAME_MAX;

    for (int slot = SLOT_OBSERVER0; slot < SLOT_COUNT; slot++)
        if (slots[slot].online)
            enqueue(slot, NET_LOG, 0, 0, -1, 0, (const unsigned char *)line, length);
}


/**
 * @brief Number of a free connection
 * @return Index or -1
 */
static int free_conn(void)
{
    for (int idx = 0; idx < CONN_COUNT; idx++)
        if (conns[idx].fd < 0)
            return idx;

    return -1;
}

/**
 * @brief Tells a client why it is not accepted and closes the socket
 * @param fd The socket
 * @param reason Text for the user of the client
 */
static void refuse(int fd, const char *reason)
{
    net_write_frame(fd, NET_LOG, 0, -1, 0, (const unsigned char *)reason, strlen(reason));
    close(fd);
}

/**
 * @brief Accepts every waiting connection
 */
static void accept_new(void)
{
    for (;;)
    {
        int fd = net_accept(listen_fd);

        if (fd < 0)
            return; // nothing more to accept

        int idx = free_conn();

        if (idx < 0)
        {
            refuse(fd, "the server has no free connection left");
            continue;
        }

        memset(&conns[idx], 0, sizeof(conns[idx]));

        conns[idx].fd = fd;
        conns[idx].slot = -1;
        conns[idx].role = -1;

        net_peer_name(fd, conns[idx].name, sizeof(conns[idx].name));
    }
}

/**
 * @brief Closes a connection; the queue of its role waits for the next client
 * @param idx Index of the connection
 * @param why Text for the log
 */
static void drop_conn(int idx, const char *why)
{
    if (idx < 0 || idx >= CONN_COUNT || conns[idx].fd < 0)
        return;

    int slot = conns[idx].slot;

    close(conns[idx].fd);

    memset(&conns[idx], 0, sizeof(conns[idx]));
    conns[idx].fd = -1;
    conns[idx].slot = -1;
    conns[idx].role = -1;

    if (slot < 0)
        return;

    slots[slot].online = 0;
    slots[slot].conn = -1;

    if (observer_slot(slot))
    {
        queue_clear(&slots[slot].out); // an observer that left needs nothing any more
        log_event(&model, "OBSERVER LEFT: %s", slot_label(slot));
        return;
    }

    // the role keeps its queue: every frame is written again for the next client
    for (Frame *frame = slots[slot].out.head; frame != NULL; frame = frame->next)
        frame->sent = 0;

    if (slot >= SLOT_TEAM0 && slot < SLOT_OBSERVER0)
        model.teams[slot - SLOT_TEAM0].started = 0;

    if (shutting_down)
        log_event(&model, "CLIENT FINISHED: %s", slot_label(slot));
    else
        log_event(&model, "CLIENT DISCONNECTED: %s (%s); %d message(s) wait for the next client", slot_label(slot),
                  why, slots[slot].out.count);
}

/**
 * @brief Sends the welcome message with the settings to a role client
 * @param slot Number of the slot
 */
static void send_welcome(int slot)
{
    Message welcome;
    memset(&welcome, 0, sizeof(welcome));

    welcome.type = MSG_WELCOME;
    welcome.call_idx = -1;
    welcome.team_idx = -1;
    welcome.time = model.now;
    welcome.sender_role = ROLE_CLOCK;

    int team = slot - SLOT_TEAM0;
    int is_team = slot >= SLOT_TEAM0 && slot < SLOT_OBSERVER0;

    welcome.arg = is_team ? team : 0;                  // index of the team of this client
    welcome.value = is_team ? model.teams[team].id : 0; // its id from the configuration

    Settings *settings = &welcome.u.settings;

    settings->delay_ms = model.delay_ms;
    settings->shift_limit = model.shift_limit;
    settings->strategy = model.strategy;
    settings->random_calls = model.random_calls;
    settings->seed = model.seed;
    settings->clock_pid = model.clock_pid;
    snprintf(settings->config_path, sizeof(settings->config_path), "%s", config_path);
    memcpy(settings->log_path, model.log_path, sizeof(settings->log_path));

    // the welcome goes before the messages that are waiting for this role
    enqueue_message(slot, -1, 0, &welcome, 1);
}

/**
 * @brief A client has sent MSG_HELLO: the role is given to it, if it is free
 * @param idx Index of the connection
 * @param message The message
 */
static void handle_hello(int idx, const Message *message)
{
    Role role = (Role)message->sender_role;
    int wanted = message->arg; // id of the team that the client asks for, 0 = any
    int slot = -1;

    if (role == ROLE_OBSERVER)
    {
        for (int obs = SLOT_OBSERVER0; obs < SLOT_COUNT && slot < 0; obs++)
            if (!slots[obs].online)
                slot = obs;

        if (slot < 0)
        {
            refuse(conns[idx].fd, "too many observers are connected to this server");
            conns[idx].fd = -1;
            return;
        }
    }
    else if (role == ROLE_DISPATCHER)
        slot = SLOT_DISPATCHER;
    else if (role == ROLE_CALLERS)
        slot = SLOT_CALLERS;
    else if (role == ROLE_TEAM)
    {
        if (wanted > 0)
        {
            for (int team = 0; team < model.team_count; team++)
                if (model.teams[team].id == wanted)
                    slot = SLOT_TEAM0 + team;

            if (slot < 0)
            {
                refuse(conns[idx].fd, "this configuration has no team with the id you asked for");
                conns[idx].fd = -1;
                return;
            }
        }
        else
            for (int team = 0; team < model.team_count && slot < 0; team++)
                if (!slots[SLOT_TEAM0 + team].online)
                    slot = SLOT_TEAM0 + team;
    }

    if (slot < 0 || slots[slot].online)
    {
        refuse(conns[idx].fd, "this role is already played by another program");
        conns[idx].fd = -1;
        return;
    }

    conns[idx].slot = slot;
    conns[idx].role = (int)role;

    slots[slot].online = 1;
    slots[slot].conn = idx;

    int again = slots[slot].ever;
    slots[slot].ever = 1;

    if (role == ROLE_OBSERVER)
    {
        send_state(slot, 0, 0); // the observer sees the whole application at once
        flush_slot(slot);
        log_event(&model, "OBSERVER CONNECTED: %s from %s", slot_label(slot), conns[idx].name);
        return;
    }

    if (!again)
        model.processes_started++;

    if (role == ROLE_DISPATCHER)
        model.dispatcher_pid = message->value;
    else if (role == ROLE_CALLERS)
    {
        model.callers_pid = message->value;

        // every call that is already active is played by the new callers client
        for (int call = 0; call < model.call_count; call++)
            if (model.calls[call].started)
                model.calls[call].pid = message->value;
    }
    else
    {
        model.teams[slot - SLOT_TEAM0].pid = message->value;
        model.teams[slot - SLOT_TEAM0].started = 1;
    }

    send_state(slot, 0, 1);
    send_welcome(slot); // ends up before the state
    flush_slot(slot);

    log_event(&model, "CLIENT %s: %s from %s (pid %d)%s", again ? "RECONNECTED" : "CONNECTED", slot_label(slot),
              conns[idx].name, message->value,
              again && slots[slot].out.count > 0 ? ", its messages are delivered again" : "");
}

/**
 * @brief Sends the new state of a record to the clients that need it
 * @param message The MSG_SYNC message
 * @param from_slot Slot of the sender (-1 = the server itself)
 */
static void distribute_record(const Message *message, int from_slot)
{
    if (message->call_idx >= 0)
    {
        if (from_slot != SLOT_DISPATCHER)
            enqueue_message(SLOT_DISPATCHER, BOX_DISPATCHER, 0, message, 0);

        if (from_slot != SLOT_CALLERS)
            enqueue_message(SLOT_CALLERS, BOX_CALL, message->call_idx, message, 0);
    }

    if (message->team_idx >= 0 && message->team_idx < MAX_TEAMS)
    {
        int team_slot = SLOT_TEAM0 + message->team_idx;

        if (from_slot != SLOT_DISPATCHER)
            enqueue_message(SLOT_DISPATCHER, BOX_DISPATCHER, 0, message, 0);

        if (from_slot != team_slot)
            enqueue_message(team_slot, BOX_TEAM, message->team_idx, message, 0);
    }
}

/**
 * @brief Handles a message that came from a client
 * @param sim The model of the server
 * @param slot Slot of the sender
 * @param message The message
 * @param frame The frame it came in (it holds the address of the receiver)
 */
static void handle_message(Simulation *sim, int slot, const Message *message, const NetFrame *frame)
{
    // a message for another client: the server is the router
    if (frame->dest_kind != BOX_CLOCK)
    {
        int target = slot_of_dest(frame->dest_kind, frame->dest_idx);

        if (target >= 0)
            enqueue(target, NET_MESSAGE, message->type != MSG_SHUTDOWN && message->type != MSG_WELCOME, 0,
                    frame->dest_kind, frame->dest_idx, frame->payload, frame->length);

        return;
    }

    switch (message->type)
    {
    case MSG_DONE:
        // one counted message has been handled: it leaves the queue of the role
        prune_sent(&slots[slot].out);

        if (slots[slot].out.head != NULL && slots[slot].out.head->sent == slots[slot].out.head->length)
            free(queue_pop(&slots[slot].out));

        prune_sent(&slots[slot].out);
        break;

    case MSG_SCHEDULE:
        schedule_event_checked(sim, message->value, (EventType)message->event, message->arg);
        break;

    case MSG_REMOVE_DEADLINES:
        remove_call_deadlines(sim, message->arg);
        break;

    case MSG_SYNC:
        ipc_apply(sim, message);                      // the server keeps the whole model
        remember_counters(slot, &message->u.counters); // and the statistics of every client
        distribute_record(message, slot);              // the other clients get the new record
        break;

    case MSG_STATS:
        remember_counters(slot, &message->u.counters);
        stats_received++;
        break;

    default:
        break;
    }
}

/**
 * @brief Handles one frame of a connection
 * @param sim The model of the server
 * @param idx Index of the connection
 * @param frame The frame
 */
static void handle_frame(Simulation *sim, int idx, const NetFrame *frame)
{
    switch (frame->kind)
    {
    case NET_MESSAGE:
    {
        Message message;

        if (!net_unpack_message(frame->payload, frame->length, &message))
        {
            drop_conn(idx, "a damaged message");
            return;
        }

        if (conns[idx].slot < 0)
        {
            if (message.type == MSG_HELLO)
                handle_hello(idx, &message);
            else
                drop_conn(idx, "the first message must be MSG_HELLO");

            return;
        }

        handle_message(sim, conns[idx].slot, &message, frame);
        break;
    }

    case NET_LOG:
    {
        // a line of a client: into the central log of the server and to the observers
        if (conns[idx].slot < 0 || frame->length == 0)
            return;

        char line[OUT_BUF_LEN + 32];
        size_t length = frame->length < sizeof(line) - 1 ? frame->length : sizeof(line) - 1;

        memcpy(line, frame->payload, length);
        line[length] = '\0';

        if (get_log_fd() >= 0)
            write_all(get_log_fd(), line, length);

        fan_out_log(line);
        break;
    }

    case NET_CONTROL:
    {
        int request = 0;
        int number = 0;

        if (!net_unpack_control(frame->payload, frame->length, &request, &number))
            return;

        // a signal cannot be sent to another computer: the client sends the request
        signals_remote_request(request, number);
        break;
    }

    default:
        break;
    }
}

/**
 * @brief Reads and handles everything that one connection has sent
 * @param sim The model of the server
 * @param idx Index of the connection
 */
static void read_conn(Simulation *sim, int idx)
{
    int result = net_feed(&conns[idx].input, conns[idx].fd);

    for (;;)
    {
        int taken = net_take(&conns[idx].input, &incoming);

        if (taken == 0)
            break; // the rest of the frame comes later

        if (taken < 0)
        {
            drop_conn(idx, "the bytes are not a frame of this protocol");
            return;
        }

        handle_frame(sim, idx, &incoming);

        if (conns[idx].fd < 0)
            return; // the frame has closed this connection
    }

    if (result <= 0)
        drop_conn(idx, result == 0 ? "the program has closed the connection" : "a read error");
}

/**
 * @brief 1 when every message of the model has been handled
 */
static int quiet(void)
{
    for (int slot = 0; slot < SLOT_OBSERVER0; slot++)
        if (slots[slot].out.head != NULL)
            return 0;

    return 1;
}

/**
 * @brief Slot that the model waits for because its client is not connected
 * @return Number of the slot or -1
 */
static int blocked_slot(void)
{
    for (int slot = 0; slot < SLOT_OBSERVER0; slot++)
        if (!slots[slot].online && slots[slot].out.head != NULL)
            return slot;

    return -1;
}

/**
 * @brief Serves the sockets of the server
 * @param sim The model of the server
 * @param timeout_ms How long to work (-1 = until the model is quiet)
 * @param until_quiet 1 = return as soon as every message has been handled
 * @return 1 = quiet, 0 = the time is over, -1 = a client did not come back
 */
static int pump(Simulation *sim, int timeout_ms, int until_quiet)
{
    struct pollfd set[CONN_COUNT + 1];
    int owner[CONN_COUNT + 1];

    int waited = 0;
    int offline_ms = 0;
    int noted = 0;

    for (;;)
    {
        if (until_quiet && quiet())
            return 1;

        int count = 0;

        if (listen_fd >= 0)
        {
            set[0].fd = listen_fd;
            set[0].events = POLLIN;
            set[0].revents = 0;
            owner[0] = -1;
            count = 1;
        }

        for (int idx = 0; idx < CONN_COUNT; idx++)
        {
            if (conns[idx].fd < 0)
                continue;

            set[count].fd = conns[idx].fd;
            set[count].events = (short)(POLLIN | (slot_has_unsent(conns[idx].slot) ? POLLOUT : 0));
            set[count].revents = 0;
            owner[count] = idx;
            count++;
        }

        int step = PUMP_STEP_MS;

        if (timeout_ms >= 0 && timeout_ms - waited < step)
            step = timeout_ms - waited;

        if (step < 0)
            step = 0;

        int ready = poll(set, (nfds_t)count, step);

        if (ready < 0 && errno != EINTR)
        {
            perror("poll");
            return -1;
        }

        for (int item = 0; ready > 0 && item < count; item++)
        {
            if (set[item].revents == 0)
                continue;

            if (owner[item] < 0)
            {
                accept_new();
                continue;
            }

            int idx = owner[item];

            if ((set[item].revents & POLLOUT) && conns[idx].fd >= 0)
                flush_slot(conns[idx].slot);

            if ((set[item].revents & (POLLIN | POLLHUP | POLLERR)) && conns[idx].fd >= 0)
                read_conn(sim, idx);
        }

        waited += step;

        // the model needs a client that is not there: say so and keep the work
        int blocked = blocked_slot();

        if (blocked >= 0)
        {
            offline_ms += step;

            if (offline_ms - noted >= NOTE_EVERY_MS)
            {
                noted = offline_ms;
                fd_printf(STDOUT_FILENO, "[server] waiting for the client of the %s (%d s), start it again\n",
                          slot_label(blocked), offline_ms / 1000);
            }

            if (lost_timeout > 0 && offline_ms >= lost_timeout * 1000)
            {
                log_event(sim, "CLIENT LOST: the %s did not come back within %d s", slot_label(blocked), lost_timeout);
                return -1;
            }

            if (signals_stop_requested())
            {
                log_event(sim, "STOPPED BY THE USER while the %s was missing", slot_label(blocked));
                return -1;
            }
        }
        else
        {
            offline_ms = 0;
            noted = 0;
        }

        // every client is there but the minute does not end: say what is missing
        if (until_quiet && blocked < 0 && waited - noted >= NOTE_EVERY_MS * 3)
        {
            noted = waited;

            for (int slot = 0; slot < SLOT_OBSERVER0; slot++)
                if (slots[slot].out.head != NULL)
                    fd_printf(STDOUT_FILENO,
                              "[server] the %s still owes an answer for %d message(s) (first %s, %zu of %zu bytes "
                              "written)\n",
                              slot_label(slot), slots[slot].out.count, frame_message_name(slots[slot].out.head),
                              slots[slot].out.head->sent, slots[slot].out.head->length);
        }

        if (!until_quiet && timeout_ms >= 0 && waited >= timeout_ms)
            return 0;
    }
}


/**
 * @brief Creates the listening socket and takes over the loaded model
 * @param loaded The model from the configuration file
 * @param session Not used by the network version
 * @return The model of the server, NULL on failure
 */
Simulation *ipc_server_init(const Simulation *loaded, const char *session)
{
    SOFT_ASSERT(loaded, "model is NULL", NULL);
    (void)session;

    model = *loaded;
    model.clock_pid = getpid();
    model.processes_started = 1; // the server itself

    is_server = 1;
    server_pid = getpid();
    ipc_set_self(ROLE_CLOCK, 0);
    set_process_tag("CLK"); // the registration of the clients is logged as well

    for (int idx = 0; idx < CONN_COUNT; idx++)
    {
        memset(&conns[idx], 0, sizeof(conns[idx]));
        conns[idx].fd = -1;
        conns[idx].slot = -1;
        conns[idx].role = -1;
    }

    for (int slot = 0; slot < SLOT_COUNT; slot++)
    {
        memset(&slots[slot], 0, sizeof(slots[slot]));
        slots[slot].conn = -1;
    }

    listen_fd = net_listen(endpoint_host, endpoint_port, LISTEN_BACKLOG);

    if (listen_fd < 0)
    {
        fprintf(stderr, "cannot listen on %s:%d\n", endpoint_host[0] ? endpoint_host : "*", endpoint_port);
        return NULL;
    }

    char local[NET_ADDR_LEN];
    net_local_name(listen_fd, local, sizeof(local));
    snprintf(flavour_text, sizeof(flavour_text), "TCP client-server, server listens on %s", local);

    atexit(ipc_server_cleanup);

    return &model;
}

/**
 * @brief Prints the roles that are still not played
 * @param sim The model
 */
static void print_missing(const Simulation *sim)
{
    char line[OUT_BUF_LEN];
    int len = snprintf(line, sizeof(line), "[server] waiting for:");

    if (!slots[SLOT_DISPATCHER].online)
        len += snprintf(line + len, sizeof(line) - (size_t)len, " dispatcher");

    if (!slots[SLOT_CALLERS].online)
        len += snprintf(line + len, sizeof(line) - (size_t)len, " callers");

    for (int idx = 0; idx < sim->team_count && len < (int)sizeof(line) - 16; idx++)
        if (!slots[SLOT_TEAM0 + idx].online)
            len += snprintf(line + len, sizeof(line) - (size_t)len, " team(B%d)", sim->teams[idx].id);

    fd_printf(STDOUT_FILENO, "%s\n", line);
}

/**
 * @brief Waits until every role of the model is played by a client
 * @param sim The model
 * @return 1 when everybody is there, 0 if the user interrupted the waiting
 */
int ipc_start_processes(Simulation *sim)
{
    char local[NET_ADDR_LEN];
    net_local_name(listen_fd, local, sizeof(local));

    fd_printf(STDOUT_FILENO,
              "[server] pid %d listens on %s. Start the other programs on this or on any\n"
              "         other computer of the network (-H = address of this server):\n"
              "         ./bin/ed_net_dispatcher -H <server> -p %d\n"
              "         ./bin/ed_net_callers    -H <server> -p %d\n"
              "         ./bin/ed_net_team       -H <server> -p %d   (%d times, or -i <team id>)\n"
              "         ./bin/ed_net_monitor    -H <server> -p %d   (any number, at any time)\n",
              (int)getpid(), local, endpoint_port, endpoint_port, endpoint_port, sim->team_count, endpoint_port);

    int needed = 2 + sim->team_count;
    int ready = 0;
    int waited = 0;

    while (ready < needed)
    {
        if (signals_stop_requested())
        {
            fd_printf(STDOUT_FILENO, "[server] interrupted while waiting for the clients\n");
            return 0;
        }

        if (pump(sim, PUMP_STEP_MS * 2, 0) < 0)
            return 0;

        waited += PUMP_STEP_MS * 2;

        int now_ready = (slots[SLOT_DISPATCHER].online ? 1 : 0) + (slots[SLOT_CALLERS].online ? 1 : 0);

        for (int idx = 0; idx < sim->team_count; idx++)
            if (slots[SLOT_TEAM0 + idx].online)
                now_ready++;

        if (now_ready != ready)
        {
            ready = now_ready;
            fd_printf(STDOUT_FILENO, "[server] %d of %d client(s) registered\n", ready, needed);
            waited = 0;
        }
        else if (lost_timeout > 0 && waited >= lost_timeout * 1000)
        {
            // -T also limits the waiting for the first clients, so that a server
            // that nobody joins does not wait for ever
            fd_printf(STDOUT_FILENO, "[server] no client registered within %d s, giving up\n", lost_timeout);
            print_missing(sim);
            return 0;
        }
        else if (waited >= 5000)
        {
            waited = 0;
            print_missing(sim);
        }
    }

    return 1;
}

/**
 * @brief The call is played by the callers client
 * @param sim The model
 * @param call_idx Index of the call
 * @return 1 on success
 */
int ipc_start_call(Simulation *sim, int call_idx)
{
    sim->calls[call_idx].pid = sim->callers_pid;
    sim->calls[call_idx].started = 1;

    // a call of the random generator is not known to the clients yet
    ipc_publish_call(sim, call_idx);

    return 1;
}

/**
 * @brief The slot of a closed call is free again
 * @param sim The model
 * @param call_idx Index of the call
 * @return 1 on success
 */
int ipc_stop_call(Simulation *sim, int call_idx)
{
    sim->calls[call_idx].started = 0; // the callers client goes on working

    return 1;
}

/**
 * @brief Tells every client that the shift is over and collects their statistics
 * @param sim The model
 */
void ipc_stop_all(Simulation *sim)
{
    if (sim == NULL || !is_server)
        return;

    shutting_down = 1;

    Message message;
    memset(&message, 0, sizeof(message));

    message.type = MSG_SHUTDOWN;
    message.call_idx = -1;
    message.team_idx = -1;
    message.time = sim->now;
    message.sender_role = ROLE_CLOCK;

    int running = 0;

    for (int slot = 0; slot < SLOT_OBSERVER0; slot++)
        if (slots[slot].online)
        {
            enqueue_message(slot, -1, 0, &message, 0);
            running++;
        }

    // every client answers with its counters and closes the connection
    stats_received = 0;

    for (int waited = 0; stats_received < running && waited < EXIT_WAIT_MS; waited += PUMP_STEP_MS)
        pump(sim, PUMP_STEP_MS, 0);

    if (stats_received < running)
        fd_printf(STDOUT_FILENO, "[server] %d client(s) did not send their statistics\n", running - stats_received);

    // the statistics of the clients are added to the counters of the server
    counters_merged = 1;

    for (int slot = 0; slot < SLOT_OBSERVER0; slot++)
    {
        if (!slots[slot].ever)
            continue;

        const Counters *counters = &slots[slot].counters;

        sim->total_wait += counters->total_wait;
        sim->total_service += counters->total_service;
        sim->served += counters->served;
        sim->cancelled += counters->cancelled;
        sim->unserved += counters->unserved;
        sim->escalations += counters->escalations;
        sim->dispatches += counters->dispatches;
        sim->received += counters->received;
    }

    // the last state of the shift for the observers, with the final statistics
    ipc_publish_state(sim, 1);

    for (int waited = 0; waited < 1000; waited += PUMP_STEP_MS)
        pump(sim, PUMP_STEP_MS, 0);
}

/**
 * @brief Closes the sockets of the server
 */
void ipc_server_cleanup(void)
{
    if (!is_server || getpid() != server_pid)
        return;

    is_server = 0;

    for (int idx = 0; idx < CONN_COUNT; idx++)
        if (conns[idx].fd >= 0)
        {
            close(conns[idx].fd);
            conns[idx].fd = -1;
        }

    for (int slot = 0; slot < SLOT_COUNT; slot++)
        queue_clear(&slots[slot].out);

    if (listen_fd >= 0)
    {
        close(listen_fd);
        listen_fd = -1;
    }
}

/**
 * @brief The quiet counter of the network version lives in the queues of the roles
 */
void ipc_quiet_hold(Simulation *sim)
{
    (void)sim;
}

/**
 * @brief Counterpart of ipc_quiet_hold()
 */
void ipc_quiet_release(Simulation *sim)
{
    (void)sim;
}

/**
 * @brief The server works until every message of the minute has been handled
 * @param sim The model
 * @return 1 when the phase is over, 0 if a client did not come back
 */
int ipc_quiet_wait(Simulation *sim)
{
    return pump(sim, -1, 1) == 1;
}

/**
 * @brief Sends the whole state to every observer
 * @param sim The model
 * @param finished 1 = the last state of the shift
 */
void ipc_publish_state(Simulation *sim, int finished)
{
    if (!is_server || sim == NULL)
        return;

    for (int slot = SLOT_OBSERVER0; slot < SLOT_COUNT; slot++)
        if (slots[slot].online)
            send_state(slot, finished, 0);
}

/**
 * @brief Pause between two model minutes
 * @param sim The model
 * @param ms Length of the pause
 */
void ipc_idle(Simulation *sim, int ms)
{
    if (ms <= 0)
    {
        if (is_server)
            pump(sim, 0, 0); // a new observer may be waiting at the door
        return;
    }

    if (is_server)
    {
        pump(sim, ms, 0); // the server serves its clients while it waits
        return;
    }

    struct timespec pause = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

/**
 * @brief Fills the fields that every message carries
 * @param sim The model
 * @param message The message
 */
static void stamp(Simulation *sim, Message *message)
{
    message->time = sim ? sim->now : 0;
    message->sender_role = self_role;
    message->sender_idx = self_idx;
    message->call_count = sim ? sim->call_count : 0;
}

/**
 * @brief Writes a message into the connection to the server
 * @param message The message
 * @param dest_kind Address of the receiver (BoxKind), -1 = the server itself
 * @param dest_idx Index of the receiver
 * @return 1 on success, 0 if the connection is gone
 */
static int client_send(const Message *message, int dest_kind, int dest_idx)
{
    if (sock < 0)
        return 0;

    size_t length = net_pack_message(payload, sizeof(payload), message);

    if (length == 0)
        return 0;

    if (!net_write_frame(sock, NET_MESSAGE, 0, dest_kind, dest_idx, payload, length))
    {
        server_lost = 1;
        return 0;
    }

    return 1;
}

/**
 * @brief Sends the statistics of this client with every update of a record
 * @param sim The model
 * @param message The message
 */
static void attach_counters(const Simulation *sim, Message *message)
{
    Counters *counters = &message->u.counters;

    counters->total_wait = sim->total_wait;
    counters->total_service = sim->total_service;
    counters->served = sim->served;
    counters->cancelled = sim->cancelled;
    counters->unserved = sim->unserved;
    counters->escalations = sim->escalations;
    counters->dispatches = sim->dispatches;
    counters->received = sim->received;
}

/**
 * @brief Sends a message that is counted by the quiet counter
 * @param sim The model
 * @param kind Address of the receiver
 * @param idx Index of the receiver
 * @param message The message
 * @return 1 on success
 */
int ipc_send(Simulation *sim, BoxKind kind, int idx, Message *message)
{
    SOFT_ASSERT(message, "message is NULL", 0);

    stamp(sim, message);

    if (is_server)
    {
        int slot = slot_of_dest(kind, idx);

        return slot >= 0 ? enqueue_message(slot, kind, idx, message, 0) : 0;
    }

    return client_send(message, kind, idx);
}

/**
 * @brief Short form of ipc_send() without attachments
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
 * @brief Sends the requests of the user that this client has caught as signals
 */
static void forward_requests(void)
{
    ControlRequests requests;
    signals_take(&requests);

    unsigned char body[16];

    if (requests.stop && !stop_sent)
    {
        stop_sent = 1;
        size_t length = net_pack_control(body, sizeof(body), NET_CTRL_STOP, requests.stop_signal);

        if (length > 0)
            net_write_frame(sock, NET_CONTROL, 0, -1, 0, body, length);

        fd_printf(STDOUT_FILENO, "[client] the request to stop was sent to the server\n");
    }

    if (requests.close && !close_sent)
    {
        close_sent = 1;
        size_t length = net_pack_control(body, sizeof(body), NET_CTRL_CLOSE, SIGQUIT);

        if (length > 0)
            net_write_frame(sock, NET_CONTROL, 0, -1, 0, body, length);

        fd_printf(STDOUT_FILENO, "[client] the request to close the intake was sent to the server\n");
    }

    if (requests.snapshot)
    {
        size_t length = net_pack_control(body, sizeof(body), NET_CTRL_SNAPSHOT, SIGUSR1);

        if (length > 0)
            net_write_frame(sock, NET_CONTROL, 0, -1, 0, body, length);
    }
}

/**
 * @brief Receives the next message of this client
 * @param sim The model (mirror)
 * @return The message; MSG_SHUTDOWN when the server is gone or the shift is over
 */
Message ipc_receive(Simulation *sim)
{
    Message message;
    memset(&message, 0, sizeof(message));

    message.type = MSG_SHUTDOWN;
    message.call_idx = -1;
    message.team_idx = -1;

    if (sock < 0)
        return message;

    for (;;)
    {
        int result = net_read_frame(sock, &sock_input, &incoming);

        if (result == -2)
        {
            forward_requests(); // a signal came: the server decides what to do
            continue;
        }

        if (result <= 0)
        {
            server_lost = 1; // the shift ends with MSG_SHUTDOWN, not with a closed socket
            return message;
        }

        if (incoming.kind == NET_MESSAGE)
        {
            if (!net_unpack_message(incoming.payload, incoming.length, &message))
                continue;

            return message;
        }

        if (incoming.kind == NET_STATE)
        {
            static NetState state;

            if (!net_unpack_state(incoming.payload, incoming.length, &state))
                continue;

            net_state_to_model(&state, sim); // the mirror is the state of the server again

            message.type = MSG_RESYNC;
            return message;
        }

        // NET_LOG and NET_CONTROL are not for a role client
    }
}

/**
 * @brief Tells the server that one counted message has been handled completely
 * @param sim The model
 */
void ipc_done(Simulation *sim)
{
    if (is_server)
        return;

    Message message;
    memset(&message, 0, sizeof(message));

    message.type = MSG_DONE;
    message.call_idx = -1;
    message.team_idx = -1;

    stamp(sim, &message);
    client_send(&message, BOX_CLOCK, 0);
}

/**
 * @brief Puts an event into the queue of the server
 * @param sim The model
 * @param time Model time of the event
 * @param type Type of the event
 * @param id Id of the call or of the team
 * @return 1 on success
 */
int ipc_schedule(Simulation *sim, int time, EventType type, int id)
{
    if (is_server)
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

    stamp(sim, &message);

    return client_send(&message, BOX_CLOCK, 0);
}

/**
 * @brief Asks the server to remove the deadlines of a call
 * @param sim The model
 * @param call_id Id of the call
 * @return 1 on success
 */
int ipc_remove_deadlines(Simulation *sim, int call_id)
{
    if (is_server)
        return remove_call_deadlines(sim, call_id);

    Message message;
    memset(&message, 0, sizeof(message));

    message.type = MSG_REMOVE_DEADLINES;
    message.arg = call_id;
    message.call_idx = -1;
    message.team_idx = -1;

    stamp(sim, &message);

    return client_send(&message, BOX_CLOCK, 0);
}

/**
 * @brief Sends the new state of a call to the programs that need it
 * @param sim The model
 * @param call_idx Index of the call
 */
void ipc_publish_call(Simulation *sim, int call_idx)
{
    if (call_idx < 0 || call_idx >= MAX_CALLS)
        return;

    Message message;
    memset(&message, 0, sizeof(message));

    message.type = MSG_SYNC;
    message.call_idx = call_idx;
    message.team_idx = -1;
    message.call = sim->calls[call_idx];

    attach_counters(sim, &message);
    stamp(sim, &message);

    if (is_server)
        distribute_record(&message, -1);
    else
        client_send(&message, BOX_CLOCK, 0); // the server keeps the model and forwards it
}

/**
 * @brief Sends the new state of a team to the programs that need it
 * @param sim The model
 * @param team_idx Index of the team
 */
void ipc_publish_team(Simulation *sim, int team_idx)
{
    if (team_idx < 0 || team_idx >= MAX_TEAMS)
        return;

    Message message;
    memset(&message, 0, sizeof(message));

    message.type = MSG_SYNC;
    message.call_idx = -1;
    message.team_idx = team_idx;
    message.team = sim->teams[team_idx];

    attach_counters(sim, &message);
    stamp(sim, &message);

    if (is_server)
        distribute_record(&message, -1);
    else
        client_send(&message, BOX_CLOCK, 0);
}

/**
 * @brief One finished line of the log
 * @details The server fans it out to the observers (its own log file has it
 * already), a client sends it to the server, which writes it into the central
 * log and gives it to the observers.
 * @param line The line, with the time and the tag of the program
 */
void ipc_log_line(const char *line)
{
    if (line == NULL)
        return;

    if (is_server)
    {
        fan_out_log(line);
        return;
    }

    if (sock >= 0)
        net_write_frame(sock, NET_LOG, 0, -1, 0, (const unsigned char *)line, strlen(line));
}

/**
 * @brief Connects a client to the server and takes a free role
 * @param role The role this program wants to play
 * @param idx In: id of the wanted team (0 = any); out: index of the team
 * @param session Not used by the network version
 * @param wait_seconds How long to wait for a server that is not started yet
 * @return The mirror of the model, NULL on failure
 */
Simulation *ipc_client_attach(Role role, int *idx, const char *session, int wait_seconds)
{
    (void)session;

    const char *host = endpoint_host[0] ? endpoint_host : "127.0.0.1";

    sock = net_connect(host, endpoint_port, wait_seconds);

    if (sock < 0)
    {
        fprintf(stderr, "no server answers at %s:%d\n", host, endpoint_port);
        return NULL;
    }

    sock_input.length = 0;
    snprintf(flavour_text, sizeof(flavour_text), "TCP client of the server %s:%d", host, endpoint_port);

    ipc_set_self(role, 0);

    // 1. the registration: role, wanted team and pid
    Message hello;
    memset(&hello, 0, sizeof(hello));

    hello.type = MSG_HELLO;
    hello.arg = idx != NULL ? *idx : 0; // id of the wanted team, 0 = any free one
    hello.value = (int)getpid();
    hello.call_idx = -1;
    hello.team_idx = -1;
    hello.sender_role = role;

    if (!client_send(&hello, BOX_CLOCK, 0))
    {
        fprintf(stderr, "cannot send the registration to the server\n");
        close(sock);
        sock = -1;
        return NULL;
    }

    // 2. the answer: the settings and the whole state of the model
    static NetState state;

    Message welcome;
    memset(&welcome, 0, sizeof(welcome));

    char refusal[256] = "";
    int have_welcome = 0;
    int have_state = 0;

    while (!have_welcome || !have_state)
    {
        int result = net_read_frame(sock, &sock_input, &incoming);

        if (result == -2)
        {
            if (signals_stop_requested())
            {
                fprintf(stderr, "interrupted while registering\n");
                close(sock);
                sock = -1;
                return NULL;
            }

            continue;
        }

        if (result <= 0)
        {
            if (refusal[0] != '\0')
                fprintf(stderr, "the server refused this program: %s\n", refusal);
            else
                fprintf(stderr, "the server closed the connection during the registration\n");

            close(sock);
            sock = -1;
            return NULL;
        }

        if (incoming.kind == NET_LOG)
        {
            size_t length = incoming.length < sizeof(refusal) - 1 ? incoming.length : sizeof(refusal) - 1;
            memcpy(refusal, incoming.payload, length);
            refusal[length] = '\0';
            continue;
        }

        if (incoming.kind == NET_STATE)
        {
            have_state = net_unpack_state(incoming.payload, incoming.length, &state);
            continue;
        }

        if (incoming.kind == NET_MESSAGE)
        {
            Message message;

            if (!net_unpack_message(incoming.payload, incoming.length, &message))
                continue;

            if (message.type == MSG_WELCOME)
            {
                welcome = message;
                have_welcome = 1;
                continue;
            }

            fprintf(stderr, "protocol error: the server sent %d before the welcome\n", message.type);

            close(sock);
            sock = -1;
            return NULL;
        }
    }

    set_defaults(&model);

    const Settings *settings = &welcome.u.settings;

    model.delay_ms = settings->delay_ms;
    model.shift_limit = settings->shift_limit;
    model.strategy = (Strategy)settings->strategy;
    model.random_calls = settings->random_calls;
    model.seed = settings->seed;
    model.clock_pid = settings->clock_pid;
    memcpy(model.log_path, settings->log_path, sizeof(model.log_path));

    net_state_to_model(&state, &model);

    if (idx != NULL)
        *idx = welcome.arg;

    ipc_set_self(role, welcome.arg);

    if (!validate_simulation(&model))
    {
        fprintf(stderr, "the model of the server does not pass the validation\n");
        close(sock);
        sock = -1;
        return NULL;
    }

    return &model;
}

/**
 * @brief Sends the final statistics and closes the connection
 * @param sim The model
 */
void ipc_client_detach(Simulation *sim)
{
    if (is_server || sock < 0)
        return;

    if (!server_lost)
    {
        Message stats;
        memset(&stats, 0, sizeof(stats));

        stats.type = MSG_STATS;
        stats.call_idx = -1;
        stats.team_idx = -1;

        attach_counters(sim, &stats);
        stamp(sim, &stats);

        client_send(&stats, BOX_CLOCK, 0);
    }

    close(sock);
    sock = -1;
}


/**
 * @brief Connects an observer to the server
 * @param host Address of the server
 * @param port TCP port of the server
 * @param wait_seconds How long to wait for a server that is not started yet
 * @return 1 on success, 0 on failure
 */
int ipc_observer_attach(const char *host, int port, int wait_seconds)
{
    ipc_set_endpoint(host, port);

    sock = net_connect(host && *host ? host : "127.0.0.1", endpoint_port, wait_seconds);

    if (sock < 0)
    {
        fprintf(stderr, "no server answers at %s:%d\n", host ? host : "127.0.0.1", endpoint_port);
        return 0;
    }

    sock_input.length = 0;
    ipc_set_self(ROLE_OBSERVER, 0);

    Message hello;
    memset(&hello, 0, sizeof(hello));

    hello.type = MSG_HELLO;
    hello.sender_role = ROLE_OBSERVER;
    hello.value = (int)getpid();
    hello.call_idx = -1;
    hello.team_idx = -1;

    if (!client_send(&hello, BOX_CLOCK, 0))
    {
        fprintf(stderr, "cannot send the registration to the server\n");
        close(sock);
        sock = -1;
        return 0;
    }

    return 1;
}

/**
 * @brief Waits for the next frame for an observer
 * @param state Out: filled when the answer is NET_STATE
 * @param line Out: filled when the answer is NET_LOG
 * @param size Size of the line buffer
 * @return NET_STATE, NET_LOG, 0 = the server is gone, -1 = error, -2 = a signal came
 */
int ipc_observer_next(NetState *state, char *line, size_t size)
{
    if (sock < 0)
        return 0;

    for (;;)
    {
        int result = net_read_frame(sock, &sock_input, &incoming);

        if (result == -2)
            return -2;

        if (result <= 0)
            return result == 0 ? 0 : -1;

        if (incoming.kind == NET_STATE && state != NULL)
        {
            if (!net_unpack_state(incoming.payload, incoming.length, state))
                continue;

            return NET_STATE;
        }

        if (incoming.kind == NET_LOG && line != NULL && size > 0)
        {
            size_t length = incoming.length < size - 1 ? incoming.length : size - 1;

            memcpy(line, incoming.payload, length);
            line[length] = '\0';

            // the line of the log already ends with a line feed
            while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
                line[--length] = '\0';

            return NET_LOG;
        }
    }
}

/**
 * @brief Closes the connection of an observer
 */
void ipc_observer_detach(void)
{
    if (sock >= 0)
    {
        close(sock);
        sock = -1;
    }
}
