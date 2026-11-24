#define _POSIX_C_SOURCE 200809L

#include "signals.h"
#include "utils.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop_flag = 0;     // SIGINT / SIGTERM
static volatile sig_atomic_t stop_signal = 0;   // number of the first stop signal
static volatile sig_atomic_t close_flag = 0;    // SIGQUIT
static volatile sig_atomic_t snapshot_flag = 0; // SIGUSR1
static volatile pid_t forward_to = 0;           // clock pid for the forwarding handler

/**
 * @brief Handler of the clock process: only remembers the request
 * @param number Number of the signal
 */
static void server_handler(int number)
{
    if (number == SIGINT || number == SIGTERM)
    {
        if (!stop_flag)
            stop_signal = number;
        stop_flag = 1;
    }
    else if (number == SIGQUIT)
        close_flag = 1;
    else if (number == SIGUSR1)
        snapshot_flag = 1;
}

/**
 * @brief Handler of the other processes: passes the signal to the clock
 * @param number Number of the signal
 */
static void forward_handler(int number)
{
    if (forward_to > 0)
        kill(forward_to, number); // kill() is async-signal-safe
}

/**
 * @brief Installs a handler without SA_RESTART: blocking calls are interrupted
 * @param number Number of the signal
 * @param handler Pointer to the handler function
 * @return 1 on success
 */
static int set_handler(int number, void (*handler)(int))
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    return sigaction(number, &action, NULL) == 0;
}

/**
 * @brief Installs the handlers of the clock process
 * @return 1 on success
 */
int signals_install_server(void)
{
    SOFT_ASSERT(set_handler(SIGINT, server_handler), "sigaction(SIGINT) failed", 0);
    SOFT_ASSERT(set_handler(SIGTERM, server_handler), "sigaction(SIGTERM) failed", 0);
    SOFT_ASSERT(set_handler(SIGQUIT, server_handler), "sigaction(SIGQUIT) failed", 0);
    SOFT_ASSERT(set_handler(SIGUSR1, server_handler), "sigaction(SIGUSR1) failed", 0);
    signal(SIGPIPE, SIG_IGN);
    return 1;
}

/**
 * @brief Reads the requests received with signals
 * @param requests Out: the requests (snapshot flag is reset)
 */
void signals_take(ControlRequests *requests)
{
    if (requests == NULL)
        return;

    requests->stop = stop_flag;
    requests->stop_signal = stop_signal;
    requests->close = close_flag;
    requests->snapshot = snapshot_flag;
    snapshot_flag = 0;
}

/**
 * @brief 1 if a stop signal has been received
 */
int signals_stop_requested(void)
{
    return stop_flag != 0;
}

/**
 * @brief Signals of a forked child process
 * @param clock_pid Pid of the parent (clock)
 * @return 1 on success
 */
int signals_install_child(pid_t clock_pid)
{
    forward_to = clock_pid;

    // Ctrl+C and Ctrl+\ come to the whole foreground group: the parent gets them too
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    // "kill -TERM <child>" or "kill -USR1 <child>" works as if it was sent to the clock
    SOFT_ASSERT(set_handler(SIGTERM, forward_handler), "sigaction(SIGTERM) failed", 0);
    SOFT_ASSERT(set_handler(SIGUSR1, forward_handler), "sigaction(SIGUSR1) failed", 0);
    return 1;
}

/**
 * @brief Signals of an independent client process
 * @param clock_pid Pid of the clock (taken from the shared memory / welcome message)
 * @return 1 on success
 */
int signals_install_client(pid_t clock_pid)
{
    forward_to = clock_pid;
    signal(SIGPIPE, SIG_IGN);

    SOFT_ASSERT(set_handler(SIGINT, forward_handler), "sigaction(SIGINT) failed", 0);
    SOFT_ASSERT(set_handler(SIGTERM, forward_handler), "sigaction(SIGTERM) failed", 0);
    SOFT_ASSERT(set_handler(SIGQUIT, forward_handler), "sigaction(SIGQUIT) failed", 0);
    SOFT_ASSERT(set_handler(SIGUSR1, forward_handler), "sigaction(SIGUSR1) failed", 0);
    return 1;
}

/**
 * @brief Text for the log
 * @param number Number of the signal
 * @return Text of the signal
 */
const char *signal_text(int number)
{
    switch (number)
    {
    case SIGINT:
        return "SIGINT (Ctrl+C): stop the simulation";
    case SIGTERM:
        return "SIGTERM: stop the simulation";
    case SIGQUIT:
        return "SIGQUIT (Ctrl+\\): close the intake of calls";
    case SIGUSR1:
        return "SIGUSR1: live status requested";
    default:
        return "unknown signal";
    }
}
