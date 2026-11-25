#define _POSIX_C_SOURCE 200809L

#include "net.h"
#include "utils.h"
#include "utils_extra.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CONNECT_STEP_MS 200 // pause between two connection attempts

/**
 * @struct Cursor
 * @brief Position in a byte buffer while packing or unpacking a frame
 */
typedef struct
{
    unsigned char *data; // buffer (NULL while unpacking)
    const unsigned char *read;
    size_t size;  // size of the buffer
    size_t pos;   // next byte
    int overflow; // 1 = the buffer is too small / the frame is too short
} Cursor;

/**
 * @brief Writes one byte
 * @param cursor The cursor
 * @param value The byte
 */
static void put_u8(Cursor *cursor, unsigned value)
{
    if (cursor->pos + 1 > cursor->size)
    {
        cursor->overflow = 1;
        return;
    }

    cursor->data[cursor->pos++] = (unsigned char)(value & 0xFFu);
}

/**
 * @brief Reads one byte
 * @param cursor The cursor
 * @return The byte, 0 if the frame is too short
 */
static unsigned get_u8(Cursor *cursor)
{
    if (cursor->pos + 1 > cursor->size)
    {
        cursor->overflow = 1;
        return 0;
    }

    return cursor->read[cursor->pos++];
}

/**
 * @brief Writes an unsigned 16-bit number, most significant byte first
 */
static void put_u16(Cursor *cursor, unsigned value)
{
    put_u8(cursor, value >> 8);
    put_u8(cursor, value);
}

/**
 * @brief Reads an unsigned 16-bit number
 */
static unsigned get_u16(Cursor *cursor)
{
    unsigned high = get_u8(cursor);
    unsigned low = get_u8(cursor);

    return (high << 8) | low;
}

/**
 * @brief Writes an unsigned 32-bit number, most significant byte first
 */
static void put_u32(Cursor *cursor, uint32_t value)
{
    put_u8(cursor, value >> 24);
    put_u8(cursor, value >> 16);
    put_u8(cursor, value >> 8);
    put_u8(cursor, value);
}

/**
 * @brief Reads an unsigned 32-bit number
 */
static uint32_t get_u32(Cursor *cursor)
{
    uint32_t value = 0;

    for (int byte = 0; byte < 4; byte++)
        value = (value << 8) | get_u8(cursor);

    return value;
}

/**
 * @brief Writes a signed number as 32 bits (two's complement)
 */
static void put_i32(Cursor *cursor, int value)
{
    put_u32(cursor, (uint32_t)value);
}

/**
 * @brief Reads a signed 32-bit number
 */
static int get_i32(Cursor *cursor)
{
    return (int)(int32_t)get_u32(cursor);
}

/**
 * @brief Writes a signed number as 64 bits (long counters)
 */
static void put_i64(Cursor *cursor, long long value)
{
    put_u32(cursor, (uint32_t)((uint64_t)value >> 32));
    put_u32(cursor, (uint32_t)((uint64_t)value & 0xFFFFFFFFu));
}

/**
 * @brief Reads a signed 64-bit number
 */
static long long get_i64(Cursor *cursor)
{
    uint64_t high = get_u32(cursor);
    uint64_t low = get_u32(cursor);

    return (long long)((high << 32) | low);
}

/**
 * @brief Writes a fixed number of bytes (a name, a path, a text line)
 */
static void put_text(Cursor *cursor, const char *text, size_t length)
{
    for (size_t idx = 0; idx < length; idx++)
        put_u8(cursor, (unsigned char)text[idx]);
}

/**
 * @brief Reads a fixed number of bytes and closes the string
 */
static void get_text(Cursor *cursor, char *text, size_t length)
{
    for (size_t idx = 0; idx < length; idx++)
        text[idx] = (char)get_u8(cursor);

    text[length - 1] = '\0';
}


/**
 * @brief Writes one call record
 */
static void put_call(Cursor *cursor, const Call *call)
{
    put_i32(cursor, call->id);
    put_i32(cursor, call->district);
    put_i32(cursor, call->x);
    put_i32(cursor, call->y);
    put_i32(cursor, call->spec);
    put_i32(cursor, call->priority);
    put_i32(cursor, call->arrival);
    put_i32(cursor, call->service_duration);
    put_i32(cursor, call->max_wait);
    put_i32(cursor, call->escalation_after);
    put_i32(cursor, call->cancel_after);
    put_i32(cursor, call->state);
    put_i32(cursor, call->assigned_team);
    put_i32(cursor, call->dispatch_time);
    put_i32(cursor, call->service_start);
    put_i32(cursor, call->completion_time);
    put_i32(cursor, (int)call->pid);
    put_i32(cursor, call->started);
}

/**
 * @brief Reads one call record
 */
static void get_call(Cursor *cursor, Call *call)
{
    call->id = get_i32(cursor);
    call->district = get_i32(cursor);
    call->x = get_i32(cursor);
    call->y = get_i32(cursor);
    call->spec = get_i32(cursor);
    call->priority = get_i32(cursor);
    call->arrival = get_i32(cursor);
    call->service_duration = get_i32(cursor);
    call->max_wait = get_i32(cursor);
    call->escalation_after = get_i32(cursor);
    call->cancel_after = get_i32(cursor);
    call->state = (CallState)get_i32(cursor);
    call->assigned_team = get_i32(cursor);
    call->dispatch_time = get_i32(cursor);
    call->service_start = get_i32(cursor);
    call->completion_time = get_i32(cursor);
    call->pid = (pid_t)get_i32(cursor);
    call->started = get_i32(cursor);
}

/**
 * @brief Writes one team record
 */
static void put_team(Cursor *cursor, const Team *team)
{
    put_i32(cursor, team->id);
    put_i32(cursor, team->spec);
    put_i32(cursor, team->speed);
    put_i32(cursor, team->district);
    put_i32(cursor, team->base);
    put_i32(cursor, team->state);
    put_i32(cursor, team->call_id);
    put_i32(cursor, (int)team->pid);
    put_i32(cursor, team->started);
}

/**
 * @brief Reads one team record
 */
static void get_team(Cursor *cursor, Team *team)
{
    team->id = get_i32(cursor);
    team->spec = (Specialization)get_i32(cursor);
    team->speed = get_i32(cursor);
    team->district = get_i32(cursor);
    team->base = get_i32(cursor);
    team->state = (TeamState)get_i32(cursor);
    team->call_id = get_i32(cursor);
    team->pid = (pid_t)get_i32(cursor);
    team->started = get_i32(cursor);
}

/**
 * @brief Writes the statistics of one program
 */
static void put_counters(Cursor *cursor, const Counters *counters)
{
    put_i64(cursor, counters->total_wait);
    put_i64(cursor, counters->total_service);
    put_i32(cursor, counters->served);
    put_i32(cursor, counters->cancelled);
    put_i32(cursor, counters->unserved);
    put_i32(cursor, counters->escalations);
    put_i32(cursor, counters->dispatches);
    put_i32(cursor, counters->received);
}

/**
 * @brief Reads the statistics of one program
 */
static void get_counters(Cursor *cursor, Counters *counters)
{
    counters->total_wait = (long)get_i64(cursor);
    counters->total_service = (long)get_i64(cursor);
    counters->served = get_i32(cursor);
    counters->cancelled = get_i32(cursor);
    counters->unserved = get_i32(cursor);
    counters->escalations = get_i32(cursor);
    counters->dispatches = get_i32(cursor);
    counters->received = get_i32(cursor);
}

/**
 * @brief Packs a message into bytes
 * @param buf Buffer
 * @param size Size of the buffer
 * @param message The message
 * @return Number of bytes, 0 if the buffer is too small
 */
size_t net_pack_message(unsigned char *buf, size_t size, const Message *message)
{
    SOFT_ASSERT(buf && message, "buffer or message is NULL", 0);

    Cursor cursor = {buf, NULL, size, 0, 0};

    put_i32(&cursor, message->type);
    put_i32(&cursor, message->event);
    put_i32(&cursor, message->arg);
    put_i32(&cursor, message->value);
    put_i32(&cursor, message->time);
    put_i32(&cursor, message->sender_role);
    put_i32(&cursor, message->sender_idx);
    put_i32(&cursor, message->call_idx);
    put_i32(&cursor, message->team_idx);
    put_i32(&cursor, message->call_count);

    put_call(&cursor, &message->call);
    put_team(&cursor, &message->team);

    // only the half of the union that belongs to this kind of message is sent:
    // the settings for MSG_WELCOME, the counters for everything else (every update
    // of a record carries the statistics of its sender)
    if (message->type == MSG_WELCOME)
    {
        const Settings *settings = &message->u.settings;

        put_i32(&cursor, settings->delay_ms);
        put_i32(&cursor, settings->shift_limit);
        put_i32(&cursor, settings->strategy);
        put_i32(&cursor, settings->random_calls);
        put_u32(&cursor, settings->seed);
        put_i32(&cursor, (int)settings->clock_pid);
        put_text(&cursor, settings->config_path, LINE_LEN);
        put_text(&cursor, settings->log_path, LINE_LEN);
    }
    else
        put_counters(&cursor, &message->u.counters);

    return cursor.overflow ? 0 : cursor.pos;
}

/**
 * @brief Unpacks a message
 * @param buf Buffer
 * @param length Number of bytes
 * @param message Out: the message
 * @return 1 on success, 0 if the frame is damaged
 */
int net_unpack_message(const unsigned char *buf, size_t length, Message *message)
{
    SOFT_ASSERT(buf && message, "buffer or message is NULL", 0);

    Cursor cursor = {NULL, buf, length, 0, 0};

    memset(message, 0, sizeof(*message));

    message->type = get_i32(&cursor);
    message->event = get_i32(&cursor);
    message->arg = get_i32(&cursor);
    message->value = get_i32(&cursor);
    message->time = get_i32(&cursor);
    message->sender_role = get_i32(&cursor);
    message->sender_idx = get_i32(&cursor);
    message->call_idx = get_i32(&cursor);
    message->team_idx = get_i32(&cursor);
    message->call_count = get_i32(&cursor);

    get_call(&cursor, &message->call);
    get_team(&cursor, &message->team);

    // the type of the message says which half of the union is in the frame
    if (message->type == MSG_WELCOME)
    {
        Settings *settings = &message->u.settings;

        settings->delay_ms = get_i32(&cursor);
        settings->shift_limit = get_i32(&cursor);
        settings->strategy = get_i32(&cursor);
        settings->random_calls = get_i32(&cursor);
        settings->seed = get_u32(&cursor);
        settings->clock_pid = (pid_t)get_i32(&cursor);
        get_text(&cursor, settings->config_path, LINE_LEN);
        get_text(&cursor, settings->log_path, LINE_LEN);
    }
    else
        get_counters(&cursor, &message->u.counters);

    return !cursor.overflow;
}


/**
 * @brief Packs the whole state of the simulation
 * @param buf Buffer
 * @param size Size of the buffer
 * @param state The state
 * @return Number of bytes, 0 if the buffer is too small
 */
size_t net_pack_state(unsigned char *buf, size_t size, const NetState *state)
{
    SOFT_ASSERT(buf && state, "buffer or state is NULL", 0);

    Cursor cursor = {buf, NULL, size, 0, 0};

    put_i32(&cursor, state->now);
    put_i32(&cursor, state->finished);
    put_i32(&cursor, state->stop_reason);
    put_i32(&cursor, state->intake_closed);
    put_i32(&cursor, state->shift_limit);
    put_i32(&cursor, state->delay_ms);
    put_i32(&cursor, state->strategy);
    put_i32(&cursor, state->random_calls);
    put_i32(&cursor, state->return_to_base);
    put_i32(&cursor, state->escalation_enabled);
    put_u32(&cursor, state->seed);

    put_i32(&cursor, state->district_count);
    put_i32(&cursor, state->team_count);
    put_i32(&cursor, state->call_count);
    put_i32(&cursor, state->event_count);
    put_i32(&cursor, state->completed_events);

    put_i32(&cursor, state->invariant_errors);
    put_i32(&cursor, state->processes_started);
    put_i32(&cursor, state->clients_online);
    put_i32(&cursor, state->observers_online);

    put_counters(&cursor, &state->counters);

    // only the part that is really used is sent, not the whole arrays
    for (int idx = 0; idx < state->district_count; idx++)
    {
        put_text(&cursor, state->districts[idx].name, NAME_LEN);
        put_i32(&cursor, state->districts[idx].travel_count);

        for (int jdx = 0; jdx < state->district_count; jdx++)
            put_i32(&cursor, state->districts[idx].travel[jdx]);
    }

    for (int idx = 0; idx < state->team_count; idx++)
        put_team(&cursor, &state->teams[idx]);

    for (int idx = 0; idx < state->call_count; idx++)
        put_call(&cursor, &state->calls[idx]);

    return cursor.overflow ? 0 : cursor.pos;
}

/**
 * @brief Unpacks the whole state of the simulation
 * @param buf Buffer
 * @param length Number of bytes
 * @param state Out: the state
 * @return 1 on success, 0 if the frame is damaged
 */
int net_unpack_state(const unsigned char *buf, size_t length, NetState *state)
{
    SOFT_ASSERT(buf && state, "buffer or state is NULL", 0);

    Cursor cursor = {NULL, buf, length, 0, 0};

    memset(state, 0, sizeof(*state));

    state->now = get_i32(&cursor);
    state->finished = get_i32(&cursor);
    state->stop_reason = get_i32(&cursor);
    state->intake_closed = get_i32(&cursor);
    state->shift_limit = get_i32(&cursor);
    state->delay_ms = get_i32(&cursor);
    state->strategy = get_i32(&cursor);
    state->random_calls = get_i32(&cursor);
    state->return_to_base = get_i32(&cursor);
    state->escalation_enabled = get_i32(&cursor);
    state->seed = get_u32(&cursor);

    state->district_count = get_i32(&cursor);
    state->team_count = get_i32(&cursor);
    state->call_count = get_i32(&cursor);
    state->event_count = get_i32(&cursor);
    state->completed_events = get_i32(&cursor);

    state->invariant_errors = get_i32(&cursor);
    state->processes_started = get_i32(&cursor);
    state->clients_online = get_i32(&cursor);
    state->observers_online = get_i32(&cursor);

    get_counters(&cursor, &state->counters);

    if (state->district_count < 0 || state->district_count > MAX_DISTRICTS || state->team_count < 0 ||
        state->team_count > MAX_TEAMS || state->call_count < 0 || state->call_count > MAX_CALLS)
        return 0;

    for (int idx = 0; idx < state->district_count; idx++)
    {
        get_text(&cursor, state->districts[idx].name, NAME_LEN);
        state->districts[idx].travel_count = get_i32(&cursor);

        for (int jdx = 0; jdx < state->district_count; jdx++)
            state->districts[idx].travel[jdx] = get_i32(&cursor);
    }

    for (int idx = 0; idx < state->team_count; idx++)
        get_team(&cursor, &state->teams[idx]);

    for (int idx = 0; idx < state->call_count; idx++)
        get_call(&cursor, &state->calls[idx]);

    return !cursor.overflow;
}

/**
 * @brief Packs a request of the user
 * @param buf Buffer
 * @param size Size of the buffer
 * @param request NET_CTRL_*
 * @param signal_number Number of the signal that the client has caught
 * @return Number of bytes, 0 on overflow
 */
size_t net_pack_control(unsigned char *buf, size_t size, int request, int signal_number)
{
    SOFT_ASSERT(buf, "buffer is NULL", 0);

    Cursor cursor = {buf, NULL, size, 0, 0};

    put_i32(&cursor, request);
    put_i32(&cursor, signal_number);

    return cursor.overflow ? 0 : cursor.pos;
}

/**
 * @brief Unpacks a request of the user
 * @return 1 on success, 0 if the frame is damaged
 */
int net_unpack_control(const unsigned char *buf, size_t length, int *request, int *signal_number)
{
    SOFT_ASSERT(buf && request && signal_number, "argument is NULL", 0);

    Cursor cursor = {NULL, buf, length, 0, 0};

    *request = get_i32(&cursor);
    *signal_number = get_i32(&cursor);

    return !cursor.overflow;
}

/**
 * @brief Copies the model of the server into a state frame
 * @param state Out: the state
 * @param sim The model of the server
 * @param finished 1 = the shift is over
 */
void net_state_from_model(NetState *state, const Simulation *sim, int finished)
{
    if (state == NULL || sim == NULL)
        return;

    memset(state, 0, sizeof(*state));

    state->now = sim->now;
    state->finished = finished;
    state->stop_reason = sim->stop_reason;
    state->intake_closed = sim->intake_closed;
    state->shift_limit = sim->shift_limit;
    state->delay_ms = sim->delay_ms;
    state->strategy = sim->strategy;
    state->random_calls = sim->random_calls;
    state->return_to_base = sim->return_to_base;
    state->escalation_enabled = sim->escalation_enabled;
    state->seed = sim->seed;

    state->district_count = sim->district_count;
    state->team_count = sim->team_count;
    state->call_count = sim->call_count;
    state->event_count = sim->event_count;
    state->completed_events = sim->completed_events;

    state->invariant_errors = sim->invariant_errors;
    state->processes_started = sim->processes_started;

    state->counters.total_wait = sim->total_wait;
    state->counters.total_service = sim->total_service;
    state->counters.served = sim->served;
    state->counters.cancelled = sim->cancelled;
    state->counters.unserved = sim->unserved;
    state->counters.escalations = sim->escalations;
    state->counters.dispatches = sim->dispatches;
    state->counters.received = sim->received;

    memcpy(state->districts, sim->districts, sizeof(state->districts));
    memcpy(state->teams, sim->teams, sizeof(state->teams));
    memcpy(state->calls, sim->calls, sizeof(state->calls));
}

/**
 * @brief Copies a received state into the mirror of a client
 * @param state The state of the server
 * @param sim Out: the mirror
 */
void net_state_to_model(const NetState *state, Simulation *sim)
{
    if (state == NULL || sim == NULL)
        return;

    sim->now = state->now;
    sim->intake_closed = state->intake_closed;
    sim->shift_limit = state->shift_limit;
    sim->delay_ms = state->delay_ms;
    sim->strategy = (Strategy)state->strategy;
    sim->random_calls = state->random_calls;
    sim->return_to_base = state->return_to_base;
    sim->escalation_enabled = state->escalation_enabled;
    sim->seed = state->seed;

    sim->district_count = state->district_count;
    sim->team_count = state->team_count;
    sim->call_count = state->call_count;

    // the counters of this client as the server knows them: a client that was
    // restarted goes on counting from the values of its predecessor
    sim->total_wait = state->counters.total_wait;
    sim->total_service = state->counters.total_service;
    sim->served = state->counters.served;
    sim->cancelled = state->counters.cancelled;
    sim->unserved = state->counters.unserved;
    sim->escalations = state->counters.escalations;
    sim->dispatches = state->counters.dispatches;
    sim->received = state->counters.received;

    memcpy(sim->districts, state->districts, sizeof(sim->districts));
    memcpy(sim->calls, state->calls, sizeof(sim->calls));

    // the process information of a team is local, the rest comes from the server
    for (int idx = 0; idx < MAX_TEAMS; idx++)
    {
        pid_t pid = sim->teams[idx].pid;
        int started = sim->teams[idx].started;

        sim->teams[idx] = state->teams[idx];
        sim->teams[idx].pid = pid;
        sim->teams[idx].started = started;
    }
}


/**
 * @brief Switches a descriptor to non-blocking or blocking mode
 * @param fd The descriptor
 * @param on 1 = non-blocking
 * @return 1 on success, 0 on failure
 */
int net_set_nonblocking(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return 0;

    if (on)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;

    return fcntl(fd, F_SETFL, flags) == 0;
}

/**
 * @brief Switches off the Nagle algorithm: a short frame is sent at once
 * @param fd The descriptor
 */
void net_no_delay(int fd)
{
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

/**
 * @brief Text form of the address of a socket
 * @param fd The descriptor
 * @param buf Out: "address:port"
 * @param size Size of the buffer
 * @param peer 1 = the other end, 0 = this end
 */
static void socket_name(int fd, char *buf, size_t size, int peer)
{
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);

    snprintf(buf, size, "?");

    if (peer ? getpeername(fd, (struct sockaddr *)&address, &length) != 0
             : getsockname(fd, (struct sockaddr *)&address, &length) != 0)
        return;

    char host[NET_HOST_LEN];
    char service[16];

    if (getnameinfo((struct sockaddr *)&address, length, host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0)
        snprintf(buf, size, "%s:%s", host, service);
}

/**
 * @brief Text "address:port" of this end of a socket
 */
void net_local_name(int fd, char *buf, size_t size)
{
    socket_name(fd, buf, size, 0);
}

/**
 * @brief Text "address:port" of the other end of a socket
 */
void net_peer_name(int fd, char *buf, size_t size)
{
    socket_name(fd, buf, size, 1);
}

/**
 * @brief Creates a listening socket
 * @param bind_host Address of the interface, NULL or "" = all interfaces
 * @param port TCP port
 * @param backlog Length of the queue of waiting connections
 * @return Descriptor of the non-blocking listening socket, -1 on failure
 */
int net_listen(const char *bind_host, int port, int backlog)
{
    char service[16];
    snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6, whatever the machine offers
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE;     // an address to bind to

    struct addrinfo *list = NULL;
    const char *node = (bind_host && *bind_host) ? bind_host : NULL;

    int code = getaddrinfo(node, service, &hints, &list);

    if (code != 0)
    {
        fprintf(stderr, "cannot use the address '%s': %s\n", node ? node : "*", gai_strerror(code));
        return -1;
    }

    int fd = -1;

    for (struct addrinfo *item = list; item != NULL; item = item->ai_next)
    {
        fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);

        if (fd < 0)
            continue;

        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)); // restart without waiting

        if (bind(fd, item->ai_addr, item->ai_addrlen) == 0 && listen(fd, backlog) == 0)
            break;

        perror("bind");
        close(fd);
        fd = -1;
    }

    freeaddrinfo(list);

    if (fd < 0)
        return -1;

    if (!net_set_nonblocking(fd, 1))
    {
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * @brief Connects to the server
 * @param host Address or name of the server
 * @param port TCP port
 * @param wait_seconds How long to keep trying while the server is not there (0 = one try)
 * @return Descriptor of the blocking socket, -1 on failure
 */
int net_connect(const char *host, int port, int wait_seconds)
{
    SOFT_ASSERT(host, "host is NULL", -1);

    char service[16];
    snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *list = NULL;
    int code = getaddrinfo(host, service, &hints, &list);

    if (code != 0)
    {
        fprintf(stderr, "cannot find the server '%s': %s\n", host, gai_strerror(code));
        return -1;
    }

    int waited_ms = 0;
    int said = 0;

    for (;;)
    {
        for (struct addrinfo *item = list; item != NULL; item = item->ai_next)
        {
            int fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);

            if (fd < 0)
                continue;

            if (connect(fd, item->ai_addr, item->ai_addrlen) == 0)
            {
                net_no_delay(fd);
                freeaddrinfo(list);
                return fd;
            }

            close(fd);
        }

        if (waited_ms >= wait_seconds * 1000)
            break;

        if (!said)
        {
            fd_printf(STDOUT_FILENO, "waiting for the server %s:%d ...\n", host, port);
            said = 1;
        }

        struct timespec pause = {0, CONNECT_STEP_MS * 1000000L};

        if (nanosleep(&pause, NULL) != 0 && errno == EINTR)
            break; // Ctrl+C while waiting

        waited_ms += CONNECT_STEP_MS;
    }

    freeaddrinfo(list);
    return -1;
}

/**
 * @brief Accepts a waiting connection
 * @param listen_fd The listening socket
 * @return Descriptor of the new connection, -1 if there is nothing to accept
 */
int net_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);

    if (fd < 0)
        return -1;

    net_no_delay(fd);
    net_set_nonblocking(fd, 1);

    return fd;
}

/**
 * @brief Writes all the bytes, also after a partial write or a signal
 * @param fd The descriptor
 * @param buf The bytes
 * @param length Number of bytes
 * @return 1 on success, 0 on failure
 */
int net_write_all(int fd, const unsigned char *buf, size_t length)
{
    SOFT_ASSERT(buf, "buffer is NULL", 0);

    while (length > 0)
    {
        ssize_t written = write(fd, buf, length);

        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            return 0;
        }

        buf += written;
        length -= (size_t)written;
    }

    return 1;
}

/**
 * @brief Builds the bytes of a frame
 * @param buf Out: the bytes
 * @param size Size of the buffer
 * @param kind NetKind
 * @param flags NET_FLAG_*
 * @param dest_kind BoxKind of the receiver or -1
 * @param dest_idx Index of the receiver
 * @param payload The payload
 * @param length Number of bytes of the payload
 * @return Number of bytes of the whole frame, 0 on overflow
 */
size_t net_build_frame(unsigned char *buf, size_t size, int kind, int flags, int dest_kind, int dest_idx,
                       const unsigned char *payload, size_t length)
{
    SOFT_ASSERT(buf, "buffer is NULL", 0);

    if (length > NET_FRAME_MAX || size < NET_HEADER_LEN + length)
        return 0;

    Cursor cursor = {buf, NULL, size, 0, 0};

    put_u32(&cursor, NET_MAGIC);
    put_u16(&cursor, NET_VERSION);
    put_u16(&cursor, (unsigned)kind);
    put_u16(&cursor, (unsigned)flags);
    put_u16(&cursor, (unsigned)(uint16_t)(int16_t)dest_kind);
    put_u16(&cursor, (unsigned)(uint16_t)(int16_t)dest_idx);
    put_u32(&cursor, (uint32_t)length);

    for (size_t idx = 0; idx < length && !cursor.overflow; idx++)
        put_u8(&cursor, payload[idx]);

    return cursor.overflow ? 0 : cursor.pos;
}

/**
 * @brief Writes a whole frame into a blocking socket
 * @return 1 on success, 0 on failure
 */
int net_write_frame(int fd, int kind, int flags, int dest_kind, int dest_idx, const unsigned char *payload,
                    size_t length)
{
    static unsigned char buf[NET_HEADER_LEN + NET_FRAME_MAX];

    size_t bytes = net_build_frame(buf, sizeof(buf), kind, flags, dest_kind, dest_idx, payload, length);

    if (bytes == 0)
        return 0;

    return net_write_all(fd, buf, bytes);
}

/**
 * @brief Reads the header of a frame out of a buffer of bytes
 * @param data The bytes
 * @param frame Out: kind, flags, receiver and length of the payload
 * @return 1 if the header is correct, 0 if the stream is damaged
 */
static int parse_header(const unsigned char *data, NetFrame *frame)
{
    Cursor cursor = {NULL, data, NET_HEADER_LEN, 0, 0};

    uint32_t magic = get_u32(&cursor);
    unsigned version = get_u16(&cursor);

    if (magic != NET_MAGIC || version != NET_VERSION)
        return 0;

    frame->kind = (int)get_u16(&cursor);
    frame->flags = (int)get_u16(&cursor);
    frame->dest_kind = (int)(int16_t)get_u16(&cursor);
    frame->dest_idx = (int)(int16_t)get_u16(&cursor);
    frame->length = (size_t)get_u32(&cursor);

    return frame->length <= NET_FRAME_MAX;
}

/**
 * @brief Reads everything that is in the socket into the buffer of a connection
 * @param input Buffer of the connection
 * @param fd The descriptor
 * @return 1 = something was read or nothing is there, 0 = the other end is gone, -1 = error
 */
int net_feed(NetInput *input, int fd)
{
    SOFT_ASSERT(input, "input is NULL", -1);

    for (;;)
    {
        if (input->length >= sizeof(input->data))
            return 1; // the buffer is full: the frames in it must be taken out first

        ssize_t bytes = read(fd, input->data + input->length, sizeof(input->data) - input->length);

        if (bytes > 0)
        {
            input->length += (size_t)bytes;
            continue;
        }

        if (bytes == 0)
            return 0; // the other end has closed the connection

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 1; // everything that was there has been read

        return -1;
    }
}

/**
 * @brief Takes the next whole frame out of the buffer of a connection
 * @param input Buffer of the connection
 * @param frame Out: the frame
 * @return 1 = frame, 0 = the frame is not complete yet, -1 = damaged stream
 */
int net_take(NetInput *input, NetFrame *frame)
{
    SOFT_ASSERT(input && frame, "argument is NULL", -1);

    if (input->length < NET_HEADER_LEN)
        return 0;

    if (!parse_header(input->data, frame))
        return -1;

    size_t whole = NET_HEADER_LEN + frame->length;

    if (input->length < whole)
        return 0;

    memcpy(frame->payload, input->data + NET_HEADER_LEN, frame->length);

    input->length -= whole;
    memmove(input->data, input->data + whole, input->length);

    return 1;
}

/**
 * @brief Reads one frame from a blocking socket
 * @param fd The descriptor
 * @param input Buffer of this connection (keeps the bytes of an unfinished frame)
 * @param frame Out: the frame
 * @return 1 = frame, 0 = the other end is gone, -1 = error, -2 = interrupted by a signal
 */
int net_read_frame(int fd, NetInput *input, NetFrame *frame)
{
    SOFT_ASSERT(input && frame, "argument is NULL", -1);

    for (;;)
    {
        int taken = net_take(input, frame);

        if (taken == 1)
            return 1;

        if (taken < 0)
        {
            fprintf(stderr, "the connection sends something that is not a frame of this protocol\n");
            return -1;
        }

        if (input->length >= sizeof(input->data))
            return -1;

        ssize_t bytes = read(fd, input->data + input->length, sizeof(input->data) - input->length);

        if (bytes > 0)
        {
            input->length += (size_t)bytes;
            continue;
        }

        if (bytes == 0)
            return 0;

        if (errno == EINTR)
            return -2; // a signal: the caller looks at the requests of the user

        return -1;
    }
}
