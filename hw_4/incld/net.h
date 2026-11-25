#ifndef NET_H
#define NET_H

#include "message.h"
#include "model.h"

#include <stddef.h>
#include <stdint.h>


#define NET_MAGIC 0x45443335u   // "ED35": variant 35, homework 4
#define NET_VERSION 1           // version of the protocol
#define NET_HEADER_LEN 18       // bytes of the header above
#define NET_FRAME_MAX 16384     // the longest frame (a whole state fits in it)
#define NET_DEFAULT_PORT 5035   // default TCP port of the server
#define NET_HOST_LEN 64         // max length of a host name / address
#define NET_ADDR_LEN 80         // max length of the text "address:port"
#define NET_CTRL_STOP 1     // SIGINT / SIGTERM
#define NET_CTRL_CLOSE 2    // SIGQUIT
#define NET_CTRL_SNAPSHOT 3 // SIGUSR1
#define NET_FLAG_COUNTED 0x0001 // the receiver answers with MSG_DONE when it is ready

/**
 * @enum NetKind
 * @brief Kinds of the frames
 */
typedef enum
{
    NET_MESSAGE = 1, // one Message of the simulation
    NET_STATE,       // the whole state
    NET_LOG,         // one text line of the log
    NET_CONTROL      // a request of the user caught by a client
} NetKind;



/**
 * @struct NetState
 * @brief The whole state of the simulation in one frame
 */
typedef struct
{
    int now;           // model time of the server, minutes from the start of the shift
    int finished;      // 1 = the shift is over, this is the last state
    int stop_reason;   // StopReason, meaningful when finished = 1
    int intake_closed; // 1 = new calls are not accepted any more
    int shift_limit;
    int delay_ms;
    int strategy;
    int random_calls;
    int return_to_base;
    int escalation_enabled;
    unsigned int seed;

    int district_count;
    int team_count;
    int call_count;
    int event_count;      // events in the queue of the server
    int completed_events;

    int invariant_errors;
    int processes_started; // registered programs (server included)
    int clients_online;    // role clients connected right now
    int observers_online;  // observer clients connected right now

    Counters counters; // statistics known to the server

    District districts[MAX_DISTRICTS];
    Team teams[MAX_TEAMS];
    Call calls[MAX_CALLS];
} NetState;

/**
 * @struct NetFrame
 * @brief One complete frame with its payload
 */
typedef struct
{
    int kind;      // NetKind
    int flags;     // NET_FLAG_*
    int dest_kind; // BoxKind of the receiver (NET_MESSAGE) or -1
    int dest_idx;  // index of the receiver
    size_t length; // bytes of the payload
    unsigned char payload[NET_FRAME_MAX];
} NetFrame;

/**
 * @struct NetInput
 * @brief Bytes of one connection that do not form a whole frame yet
 */
typedef struct
{
    unsigned char data[NET_HEADER_LEN + NET_FRAME_MAX];
    size_t length;
} NetInput;


// SOCKETS


// Creates a listening socket
int net_listen(const char *bind_host, int port, int backlog);

// Connects to the server, repeats while the server is not there yet
int net_connect(const char *host, int port, int wait_seconds);

// Accepts a connection on a listening socket
int net_accept(int listen_fd);

// Switches the descriptor to (non)blocking mode
int net_set_nonblocking(int fd, int on);

// Sends small frames at once instead of waiting for more data (TCP_NODELAY)
void net_no_delay(int fd);

// Text "address:port" of the local / remote end of a socket
void net_local_name(int fd, char *buf, size_t size);
void net_peer_name(int fd, char *buf, size_t size);

// FRAMES

// Writes a whole frame (blocking descriptor
int net_write_frame(int fd, int kind, int flags, int dest_kind, int dest_idx,
    const unsigned char *payload, size_t length);

// Builds the bytes of a frame in a buffer
size_t net_build_frame(unsigned char *buf, size_t size, int kind, int flags,
    int dest_kind, int dest_idx, const unsigned char *payload, size_t length);

// Reads one frame from a blocking descriptor
int net_read_frame(int fd, NetInput *input, NetFrame *frame);

// Writes len bytes, repeats after EINTR and after a partial write
int net_write_all(int fd, const unsigned char *buf, size_t length);

// Reads everything that is in the socket now
int net_feed(NetInput *input, int fd);

// Takes the next whole frame out of the buffer
int net_take(NetInput *input, NetFrame *frame);

// SEREALIZATION

// Turning from Message to bytes and vice versa
size_t net_pack_message(unsigned char *buf, size_t size, const Message *message);
int net_unpack_message(const unsigned char *buf, size_t length, Message *message);

// Turning from NetState to bytes and vice versa
size_t net_pack_state(unsigned char *buf, size_t size, const NetState *state);
int net_unpack_state(const unsigned char *buf, size_t length, NetState *state);

// Turning from Control request to bytes and vice versa
size_t net_pack_control(unsigned char *buf, size_t size, int request, int signal_number);
int net_unpack_control(const unsigned char *buf, size_t length, int *request, int *signal_number);

// Fills a NetState from the model of the server
void net_state_from_model(NetState *state, const Simulation *sim, int finished);

// Copies the state into the mirror of a client
void net_state_to_model(const NetState *state, Simulation *sim);

// NETWORK IPC

// Address of the server
void ipc_set_endpoint(const char *host, int port);

// How long the server waits for a client that has disconnected while the model
void ipc_set_lost_timeout(int seconds);

// Verifies the connection loss of the server
int ipc_server_lost(void);

// Connection of the observer client
int ipc_observer_attach(const char *host, int port, int wait_seconds);

// Next frame for an observer
int ipc_observer_next(NetState *state, char *line, size_t size);

// Closes the connection of an observer
void ipc_observer_detach(void);

#endif
