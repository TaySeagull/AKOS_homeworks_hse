# Emergency Dispatch Simulator over the Network

Homework 4 for AKOS, variant 35

A discrete-event simulation of an emergency dispatch service, implemented in C as a client-server application using TCP sockets.

The program simulates call intake, dispatching specialized teams, team movement between districts, call servicing, priority escalation, cancellation, unserved calls, shift limits and return to base.

Each entity with independent behaviour is represented by a separate program: the server (clock), dispatcher, teams and callers. Multiple observer programs can monitor the simulation simultaneously.

The simulation logic is the same as in homework 3. Homework 4 adds a network communication layer. All four implementations from homework 3 are preserved.

## Programs

| Program | Role |
|---|---|
| `ed_net_server` | Clock, event queue, message router and central log |
| `ed_net_dispatcher` | Assigns available teams to waiting calls |
| `ed_net_team` | Handles team movement and servicing |
| `ed_net_callers` | Handles call arrival, escalation, cancellation and timeout |
| `ed_net_monitor` | Observes the entire simulation |

The server routes messages between clients. Programs communicate through TCP sockets and can run on different computers.

## Project structure

```text
hw_4_network/
├── incld/
│   ├── config.h
│   ├── dispatch.h
│   ├── event_queue.h
│   ├── generator.h
│   ├── ipc.h
│   ├── message.h
│   ├── model.h
│   ├── net.h
│   ├── output.h
│   ├── roles.h
│   ├── sems.h
│   ├── signals.h
│   ├── simulation.h
│   ├── utils.h
│   └── utils_extra.h
│
├── src/
│   ├── main_server.c
│   ├── main_client.c
│   ├── main_monitor.c
│   ├── net.c
│   ├── ipc_net.c
│   ├── ipc_shm.c
│   ├── ipc_mq.c
│   ├── config.c
│   ├── dispatch.c
│   ├── event_queue.c
│   ├── generator.c
│   ├── output.c
│   ├── roles.c
│   ├── signals.c
│   ├── simulation.c
│   ├── utils.c
│   ├── sem_common.c
│   ├── sem_named.c
│   └── sem_unnamed.c
│
├── tests/
│   ├── test_*.txt
│   ├── test_*.expect
│   └── bad/
│       └── *.txt
│
├── config.txt
├── run_network.sh
├── run_independent.sh
├── run_tests.sh
├── ipc_cleanup.sh
├── Makefile
└── README.md
```

## Command Line Interface

### Server

`-c <file>`             configuration file

`-p <port>`             TCP port (default: 5035)

`-b <address>`          interface to listen on

`-T <seconds>`          client connection timeout (0 = unlimited)

`-S`                    show status in the server terminal

`-d <milliseconds>`     delay after every model minute with events

`-s <strategy>`         team selection strategy

`-l <file>`             central log file

`-t <minutes>`          shift length (0 = unlimited)

`-r <seed>`             enable random call generation

`-h`                    help

### Role clients

`-H <host>`             server address (default: 127.0.0.1)

`-p <port>`             server port (default: 5035)

`-w <seconds>`          server connection timeout (default: 30)

`-i <id>`               team ID (team program only)

`-l <file>`             local log file

`-h`                    help

### Observer

`-H <host>`             server address

`-p <port>`             server port

`-w <seconds>`          server connection timeout

`-n <lines>`            number of recent events to display

`-P`                    plain output mode

`-h`                    help

### Strategy

- `nearest`
- `fastest`

Command-line options override the values from the configuration file.

Clients receive the configuration and simulation state from the server when they connect.

## Configuration

```text
delay_ms=120
shift_limit=720
escalation_enabled=1
return_to_base=1
strategy=NEAREST
random_calls=0
random_seed=1
random_interval=10

[DISTRICTS]
# name, travel times to all districts (-1 = no road)

[TEAMS]
# id, specialization, speed, district

[CALLS]
# id, arrival, district, x, y, priority, specialization,
# service time, max wait, escalation, cancellation
```

## Processes

Every entity with independent behaviour runs in a separate program:

- **Server** — owns model time and the event queue, routes messages, checks invariants and prints the final summary.
- **Dispatcher** — assigns available teams to waiting calls.
- **Team** — travels to calls, provides service and returns to base.
- **Callers** — handles call arrival, priority escalation, cancellation and timeout.
- **Observer** — displays the current state of the simulation without modifying it.

The server is the only program that communicates directly with all clients.

## Interprocess Communication

The network implementation uses TCP sockets.

- `net.c` — TCP connections, frame serialization and network protocol.
- `ipc_net.c` — message routing, client registration and observer communication.
- `ipc.h` — common interface for all IPC implementations.

Messages are exchanged through the server. Each client has a role and an address consisting of its role and index.

The server maintains message queues for roles, so messages are preserved when a client disconnects.

The implementations from homework 3 are also available:

- `ed_fork_named` — `fork()`, POSIX shared memory and named semaphores.
- `ed_fork_unnamed` — `fork()`, POSIX shared memory and unnamed semaphores.
- `ed_shm_*` — independent programs using POSIX shared memory and named semaphores.
- `ed_mq_*` — independent programs using POSIX message queues and named semaphores.

All implementations use the same simulation logic. Only the IPC layer differs.

## Network Protocol

Every TCP message is transmitted as a frame containing:

- `magic` — protocol identifier (`ED35`);
- `version` — protocol version;
- `kind` — frame type;
- `flags` — additional frame properties;
- `dest_kind`, `dest_idx` — destination role and index;
- `length` — payload size;
- `payload` — serialized data.

Frame types:

- `NET_MESSAGE` — simulation message;
- `NET_STATE` — complete simulation state;
- `NET_LOG` — log line;
- `NET_CONTROL` — user control request.

All numeric fields are serialized in network byte order. TCP is a byte stream, so connections use buffers to collect and extract complete frames.

## One Step of the Clock

1. The server takes events scheduled for the next model minute and sends `MSG_WAKE` to their owners.
2. Clients process events in parallel, update their records and exchange messages through the server.
3. After all counted messages are processed, the server sends `MSG_ROUND` to the dispatcher.
4. The dispatcher assigns available teams.
5. The server waits for all reactions, checks invariants, advances model time and publishes the new state to observers.

The server considers a message processed after receiving `MSG_DONE` from its recipient.

## Client Disconnection and Reconnection

- The server detects disconnected clients and logs the event.
- Messages addressed to a disconnected role remain in its queue.
- A client reconnecting with the same role receives the current simulation state and its pending messages.
- The server preserves role statistics between connections.
- `-T` specifies how long the server waits for a client to connect or reconnect.
- Observers can connect and disconnect at any time without affecting the simulation.

If the server terminates, all connected clients receive a shutdown message or detect the closed connection.

## Signals

`SIGINT` (Ctrl+C), `SIGTERM` — stop the simulation; active calls are recorded as unserved.

`SIGQUIT` (Ctrl+\\) — close call intake; finish active calls before termination.

`SIGUSR1` — print the current simulation status.

Client signals are converted into `NET_CONTROL` messages and processed by the server. A signal in the observer terminal stops only that observer.

## Termination

- All calls are processed and the event queue is empty.
- The shift limit is reached.
- `SIGINT` or `SIGTERM` is received.
- `SIGQUIT` is received.
- A client does not reconnect within the specified timeout.

The server sends `MSG_SHUTDOWN` to clients, collects their statistics, prints the summary, sends the final state to observers and closes the connections.

Exit codes:

- `0` — normal termination
- `1` — configuration, argument, socket or registration error
- `2` — client lost or connection to the server lost

## Build and Launch

1. Clean

```bash
make clean
```

2. Build

```bash
make
```

3. Launch

**Network version — one terminal:**

```bash
./run_network.sh -c config.txt
./run_network.sh -m -c tests/test_stress.txt -d 50
```

**Network version — separate terminals:**

```bash
./bin/ed_net_server -c config.txt -d 150
./bin/ed_net_dispatcher -H 127.0.0.1
./bin/ed_net_callers -H 127.0.0.1
./bin/ed_net_team -H 127.0.0.1
./bin/ed_net_monitor -H 127.0.0.1
```

Run one team client for each team in the configuration. Use `-i <id>` to specify a team ID.

**Network version — different computers:**

Server:

```bash
./bin/ed_net_server -c config.txt -p 5035
```

Clients:

```bash
./bin/ed_net_dispatcher -H <server_address> -p 5035
./bin/ed_net_team -H <server_address> -p 5035 -i 1
./bin/ed_net_callers -H <server_address> -p 5035
./bin/ed_net_monitor -H <server_address> -p 5035
```

The configuration file is required only on the server.

**Homework 3 implementations:**

```bash
./bin/ed_fork_named -c config.txt
./bin/ed_fork_unnamed -c config.txt
./run_independent.sh shm -c config.txt
./run_independent.sh mq -c config.txt
```

4. Tests

```bash
make test
./run_tests.sh net
```

5. IPC cleanup

```bash
make ipc-clean
```

6. Remove build files

```bash
make clean
```

## The Log

Every program writes its own log lines to its terminal and sends them to the server. The server writes the central log file and forwards log lines to observers.

Example:

```text
[01:12] {B3  } B3 ARRIVED at EAST for CALL #007 (after 4 min of waiting)
```

`[HH:MM]` is the model time. The identifier in braces indicates the program that produced the line.