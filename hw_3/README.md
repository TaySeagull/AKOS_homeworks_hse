**# Emergency Dispatch Simulator with processes**

Homework 3 for AKOS, variant 35

A discrete-event simulation of an emergency dispatch service, implemented in C using processes and POSIX IPC.

The program simulates call intake, the dispatch of specialized teams, team movement between districts, call servicing, priority escalation, cancellation by the caller, unserved calls due to maximum waiting time, the shift limit and the return of teams to base.

Each entity with independent behaviour is represented by a separate process: the clock, dispatcher, teams and calls.

Four implementations are available:

- `ed_fork_named` — parent process creates child processes using `fork()`, with named POSIX semaphores and shared memory
- `ed_fork_unnamed` — parent process creates child processes using `fork()`, with unnamed POSIX semaphores and shared memory
- `ed_shm_*` — independent programs communicating through POSIX shared memory and named semaphores
- `ed_mq_*` — independent programs communicating through POSIX message queues and named semaphores

All implementations use the same simulation logic. Only the IPC and synchronization layers differ.

Model time is measured in minutes from the start of the shift. Every log line has a timestamp in the `[HH:MM]` format and identifies the process that produced it.

**## Project structure**

```text
hw_3_processes/
├── incld/
│   ├── config.h
│   ├── dispatch.h
│   ├── event_queue.h
│   ├── generator.h
│   ├── model.h
│   ├── output.h
│   ├── simulation.h
│   ├── utils.h
│   ├── utils_extra.h
│   ├── message.h
│   ├── ipc.h
│   ├── sems.h
│   ├── roles.h
│   └── signals.h
│
├── src/
│   ├── main_server.c
│   ├── main_client.c
│   ├── config.c
│   ├── dispatch.c
│   ├── event_queue.c
│   ├── generator.c
│   ├── output.c
│   ├── simulation.c
│   ├── roles.c
│   ├── signals.c
│   ├── utils.c
│   ├── ipc_shm.c
│   ├── ipc_mq.c
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
├── run_tests.sh
├── run_independent.sh
├── ipc_cleanup.sh
├── Makefile
└── README.md
```

**## Command Line Interface**

**#### Clock / Fork programs**

`-c <file>`             configuration file

`-d <milliseconds>`     visual delay after every model minute with events (0 = no delay)

`-s <strategy>`         strategy of choosing a team

`-l <file>`             log file

`-t <minutes>`          shift length (0 = unlimited)

`-r <seed>`             enable random call generation with the specified seed

`-n <name>`             IPC session name

`-h`                    help

**#### Independent clients**

`-n <name>`             IPC session name (must match the clock)

`-i <id>`               team ID (team program only; default: first available team)

`-w <seconds>`          how long to wait for the clock (default: 30)

**#### Strategy**

`nearest`

`fastest`

Command-line options override the values from the configuration file.

**## Configuration**

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

**## Processes**

Every entity with independent behaviour runs in its own process:

- Clock — owns model time and the event queue, wakes processes when their events occur, checks simulation invariants and prints the final summary
- Dispatcher — receives reports and assigns available teams to waiting calls
- Team — travels to calls, provides service, returns to base and schedules its next events
- Call — waits for a team, escalates priority, cancels or gives up after the maximum waiting time

In the fork versions, the clock is the parent process and creates child processes for the dispatcher, teams and calls.

In the independent versions, each role is implemented as a separate executable. The dispatcher, team and callers programs register with the clock when they start.

The signal thread from homework 2 is replaced by signal handlers using `sigaction()` in the clock process.

**## Interprocess Communication**

Four implementations use the same simulation logic but different IPC mechanisms.

`ed_fork_named`

- POSIX shared memory (`shm_open()` and `mmap()`)
- Named POSIX semaphores (`sem_open()`)
- Parent process creates child processes using `fork()`

`ed_fork_unnamed`

- POSIX shared memory
- Unnamed POSIX semaphores (`sem_init()` with `pshared = 1`)
- Parent process creates child processes using `fork()`

`ed_shm_*`

- POSIX shared memory
- Named POSIX semaphores
- Independent programs for the clock, dispatcher, teams and callers

`ed_mq_*`

- POSIX message queues (`mq_open()`)
- Named POSIX semaphores
- Independent programs for the clock, dispatcher, teams and callers
- No shared memory

Processes communicate through messages. In the shared-memory versions, messages are exchanged through bounded mailboxes protected by semaphores.

In the message-queue version, every process maintains its own copy of the simulation model. Changes to calls and teams are sent using `MSG_SYNC` messages. The clock owns the event queue, and other processes request event scheduling through messages.

**## One Step of the Clock**

1. The clock takes all events scheduled for the next model minute and sends `MSG_WAKE` messages to their owners.
2. Calls and teams process their events in parallel. They can exchange messages and schedule new events.
3. When all messages have been processed, the dispatcher receives `MSG_ROUND` and assigns available teams.
4. The clock waits for the dispatching round to finish, checks simulation invariants and advances model time.

The quiet counter tracks unhandled messages. The clock starts the next minute only after all reactions from the current minute have been completed.

In the shared-memory versions, the counter is stored in shared memory and protected by semaphores.

In the message-queue version, the clock counts sent messages and receives `MSG_DONE` responses when they have been handled.

**## Semaphores**

The following semaphores are used for synchronization:

- `world` — protects access to the shared simulation model
- `output` — prevents processes from printing simultaneously
- `quiet_lock`, `quiet_idle` — synchronize the quiet counter
- `box_*_lock`, `box_*_filled`, `box_*_free` — implement producer-consumer mailboxes in shared memory
- `sent` — tracks sent messages in the message-queue version
- `ready`, `exited` — synchronize registration and termination of independent clients
- `claim_dispatcher`, `claim_callers`, `claim_team<k>` — prevent multiple processes from taking the same role

**## Signals**

`SIGINT` (Ctrl+C), `SIGTERM` — stop the simulation; active calls are recorded as unserved;

`SIGQUIT` (Ctrl+\\) — close call intake; finish active calls and then stop;

`SIGUSR1` — print the current simulation status.

Signals are handled by the clock process using `sigaction()`.

Forked child processes ignore `SIGINT` and `SIGQUIT` and forward `SIGTERM` and `SIGUSR1` to the clock.

Independent clients forward signals to the clock, so a signal received by any client can stop or control the whole simulation.

**## Termination**

- All calls are processed and the event queue is empty.
- The shift limit is reached. Active calls are recorded as unserved.
- `SIGINT` or `SIGTERM` is received. Active calls are recorded as unserved and the summary is printed.
- `SIGQUIT` is received. No new calls are accepted; active calls are finished before termination.
- A simulation process terminates unexpectedly. The clock detects the failure and stops the simulation with exit code 2.

In every case, the clock sends `MSG_SHUTDOWN` to the other processes, waits for their termination, prints the summary and removes IPC resources.

Exit codes:

- `0` — normal termination
- `1` — configuration, argument or IPC error
- `2` — unexpected process termination

**## Build and Launch**

1. Clean

       make clean

2. Build

       make

3. Launch

**Fork versions (4–6 points)**

       ./bin/ed_fork_named -c config.txt
       ./bin/ed_fork_unnamed -c config.txt

**Independent programs with shared memory (7–8 points)**

       ./run_independent.sh shm -c config.txt

**Independent programs with message queues (9–10 points)**

       ./run_independent.sh mq -c config.txt
       ./run_independent.sh mq -c tests/test_stress.txt -d 50

4. Tests

       make test

5. IPC cleanup

       make ipc-clean

`make ipc-clean` removes IPC objects left after an abnormal termination, for example after `kill -9`.

6. Remove build files

       make clean
