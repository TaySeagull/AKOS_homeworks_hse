# Emergency Dispatch Simulator
---

Homework for AKOS, variant 35

An event-driven simulation of an emergency dispatch service, implemented in C.

The program simulates call intake, the dispatch of specialized teams, team movement between districts, call servicing, priority escalation, cancellation, and the return of teams to base.

---

## Project structure

```
emergency_dispatch/
├── incld/
│ ├── config.h
│ ├── dispatch.h
│ ├── event_handlers.h
│ ├── event_queue.h
│ ├── model.h
│ ├── output.h
│ ├── simulation.h
│ ├── utils_extra.h
│ └── utils.h
|
├── src/
│ ├── main.c
│ ├── config.c
│ ├── dispatch.c
│ ├── event_handlers.c
│ ├── event_queue.c
│ ├── output.c
│ ├── simulation.c
│ └── utils.c
|
├── tests/
│ ├── test_cancel.txt
│ ├── test_escalation.txt
│ ├── test_fastest.txt
│ ├── test_nearest.txt
│ ├── test_no_team.txt
│ ├── test_shift_limit.txt
│ └── test_single_call.txt
│
├── config.txt
├── test_config.txt
├── run_tests.sh
├── Makefile
└── README.md
```

## Command Line Interface

#### Arguments



`-c <file>`             configuration file

`-d <milliseconds>`     virtual delay

`-s <strategy>`        strategy of choosing a brigade

`-l <file>`             log journal

`-h`                    help

#### Strategy

`nearest`

`fastest`

## Build and Launch

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

4. Tests

        make test
