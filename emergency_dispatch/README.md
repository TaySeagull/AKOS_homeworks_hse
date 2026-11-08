**# Emergency Dispatch Simulator**
**---**

Homework for AKOS, variant 35

An event-driven (discrete-event) simulation of an emergency dispatch service, implemented in C.

The program simulates call intake, the dispatch of specialized teams, team movement between districts, call servicing, priority escalation, cancellation by the caller, unserved calls due to maximum waiting time, the shift limit and the return of teams to base.

Model time is measured in minutes from the start of the shift. Every log line has a timestamp in the `[HH:MM]` format.

**---**

**## Project structure**

```text
emergency_dispatch/
├── incld/
│   ├── config.h
│   ├── dispatch.h
│   ├── event_handlers.h
│   ├── event_queue.h
│   ├── generator.h
│   ├── model.h
│   ├── output.h
│   ├── simulation.h
│   ├── utils.h
│   └── utils_extra.h
│
├── src/
│   ├── main.c
│   ├── config.c
│   ├── dispatch.c
│   ├── event_handlers.c
│   ├── event_queue.c
│   ├── generator.c
│   ├── output.c
│   ├── simulation.c
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

`-d <milliseconds>`     visual delay after every event (0 = no delay)

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

**## Termination**

- All calls are processed and the event queue is empty.
- The shift limit is reached. Active calls are recorded as unserved.
- Ctrl+C or SIGTERM is received. Active calls are recorded as unserved and the summary is printed.

With `-t 0` and random call generation enabled, the simulation runs without a shift limit and can be stopped using Ctrl+C or SIGTERM.

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
       ./emergency_dispatch -c config.txt -l dispatch.log
       ./emergency_dispatch -c tests/test_stress.txt -d 50
       ./emergency_dispatch -c config.txt -t 0 -r 42

4. Tests

       make test