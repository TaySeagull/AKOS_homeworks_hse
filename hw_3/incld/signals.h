#ifndef SIGNALS_H
#define SIGNALS_H

#include <sys/types.h>

/**
 * @struct ControlRequests
 * @brief Requests of the user received with signals by the clock process
 */
typedef struct
{
    int stop;        // SIGINT / SIGTERM: stop now
    int stop_signal; // which of them came first
    int close;       // SIGQUIT: close the intake
    int snapshot;    // SIGUSR1: print the live status (reset after reading)
} ControlRequests;

// Clock: installs the handlers of SIGINT, SIGTERM, SIGQUIT, SIGUSR1
int signals_install_server(void);

// Reads the requests
void signals_take(ControlRequests *requests);

// 1 if SIGINT / SIGTERM has been received
int signals_stop_requested(void);

// Forked child
int signals_install_child(pid_t clock_pid);

// Independent client
int signals_install_client(pid_t clock_pid);

// Name of a signal for the log
const char *signal_text(int number);

#endif
