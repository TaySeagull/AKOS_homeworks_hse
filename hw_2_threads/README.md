**# Emergency Dispatch Simulator with threads**

Homework 2 for AKOS, variant 35

A multithreaded discrete-event simulation of an emergency dispatch service, implemented in C.

The program simulates call intake, the dispatch of specialized teams, team movement between districts, call servicing, priority escalation, cancellation by the caller, unserved calls due to maximum waiting time, the shift limit and the return of teams to base.

Each entity with independent behaviour is represented by a separate thread: the clock, dispatcher, teams, calls and signal handler.

Threads communicate through messages using bounded mailboxes. Access to shared simulation data is protected by a mutex.

Two synchronization implementations are available:
- `emergency_dispatch` — mutexes and POSIX semaphores
- `emergency_dispatch_cv` — mutexes and condition variables

Model time is measured in minutes from the start of the shift. Every log line has a timestamp in the `[HH:MM]` format and identifies the thread that produced it.


**## Project structure**

```text
hw_2_threads/
├── incld/
│   ├── config.h
│   ├── dispatch.h
│   ├── event_queue.h
│   ├── generator.h
│   ├── model.h
│   ├── output.h
│   ├── simulation.h
│   ├── sync.h
│   ├── threads.h
│   ├── threads_internal.h
│   ├── utils.h
│   └── utils_extra.h
│
├── src/
│   ├── threads/
│   │    ├── call_thread.c
│   │    ├── dispatcher_thread.c
│   │    ├── signal_thread.c
│   │    └── team_thread.c
│   ├── main.c
│   ├── config.c
│   ├── dispatch.c
│   ├── event_queue.c
│   ├── generator.c
│   ├── output.c
│   ├── simulation.c
│   ├── threads.c
│   ├── sync_sem.c
│   ├── sync_cv.c
│   └── utils.c
│
├── tests/
│   ├── test_*.txt
│   ├── test_*.expect
│   └── bad/
│       └── *.txt
│
├── config.txt
├── run_tests.sh
├── Makefile
└── README.md
```

**## Command Line Interface**

**#### Arguments**

`-c <file>`             configuration file

`-d <milliseconds>`     visual delay after every model minute with events (0 = no delay)

`-s <strategy>`         strategy of choosing a brigade

`-l <file>`             log journal

`-t <minutes>`          shift length (0 = unlimited)

`-r <seed>`             enable random call generation with the specified seed

`-h`                    help

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

**## Multithreading**

Every entity with independent behaviour runs in its own thread:

- Clock — owns model time and the event queue, wakes threads when their events occur
- Dispatcher — receives reports and assigns available teams to calls
- Team — travels to calls, provides service, returns to base and schedules its next events
- Call — waits for a team, escalates priority, cancels or gives up after the maximum waiting time
- Signal thread — handles incoming signals using `sigwait()`

Threads communicate only through messages. Each thread has a bounded mailbox, implemented as a producer-consumer buffer.

The `world` mutex protects shared simulation data.

**## Synchronization**

Two versions of the program use the same simulation logic but different synchronization mechanisms.

`emergency_dispatch`

- Mutexes
- POSIX semaphores
- Semaphore-based mailbox and quiet counter (`sync_sem.c`)

`emergency_dispatch_cv`

- Mutexes
- Condition variables
- Condition-variable-based mailbox and quiet counter (`sync_cv.c`)

The quiet counter tracks unhandled messages. The clock waits until all messages have been processed before starting the next dispatching round.

**## One Step of the Clock**

1. The clock takes all events scheduled for the next model minute and sends `MSG_WAKE` messages to their owners.
2. Calls and teams process their events in parallel. They can exchange messages and schedule new events.
3. When all messages have been processed, the dispatcher receives `MSG_ROUND` and assigns available teams.
4. The clock waits for the dispatching round to finish, checks simulation invariants and advances model time.

Inside a minute, threads work asynchronously. The model time remains consistent, and the simulation results are independent of the order in which the operating system schedules threads.

**## Signals**


`SIGINT` (Ctrl+C), `SIGTERM` - Stop the simulation; active calls are recorded as unserved;
`SIGQUIT` (Ctrl+\\) - Close call intake; finish active calls and then stop;
`SIGUSR1` - Print the current simulation status.

Signals are blocked in the main thread and handled by a separate signal thread using `sigwait()`.

**## Termination**

- All calls are processed and the event queue is empty.
- The shift limit is reached. Active calls are recorded as unserved.
- `SIGINT` or `SIGTERM` is received. Active calls are recorded as unserved and the summary is printed.
- `SIGQUIT` is received. No new calls are accepted; active calls are finished before termination.

With `-t 0` and random call generation enabled, the simulation runs without a shift limit and can be stopped using Ctrl+C or Ctrl+\\.

**## Build and Launch**

1. Clean

       make clean

2. Build

       make

3. Launch

       ./emergency_dispatch
       ./emergency_dispatch -c config.txt
       ./emergency_dispatch -c config.txt -d 0
       ./emergency_dispatch -c config.txt -s fastest
       ./emergency_dispatch -l dispatch.log
       ./emergency_dispatch -c tests/test_stress.txt -d 100
       ./emergency_dispatch -c config.txt -t 0 -r 42
       ./emergency_dispatch_cv -c config.txt -d 0

4. Tests

       make test