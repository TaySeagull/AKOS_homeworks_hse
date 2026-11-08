#define _POSIX_C_SOURCE 200809L
#include "thread_internal.h"
#include "utils.h"
#include "utils_extra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

/**
 * @brief Fills the set of signals handled by the signal thread
 */
static void fill_signal_set(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGINT);  // Ctrl+C
    sigaddset(set, SIGTERM); // kill
    sigaddset(set, SIGQUIT); // Ctrl+\ key
    sigaddset(set, SIGUSR1); // live status
    sigaddset(set, SIGUSR2); // internal: end of the signal thread
}

/**
 * @brief Blocks the handled signals in the calling thread
 * @return 1 on success
 */
int block_signals(void)
{
    sigset_t set;
    fill_signal_set(&set);

    int result = pthread_sigmask(SIG_BLOCK, &set, NULL);
    SOFT_ASSERT(result == 0, "pthread_sigmask failed", 0);

    return 1;
}

/**
 * @brief Thread that receives signals synchronously with sigwait()
 */
void *signal_thread(void *raw)
{
    Simulation *sim = raw;
    set_thread_tag("SIG");

    sigset_t set;
    fill_signal_set(&set);

    for (;;)
    {
        int number = 0;

        if (sigwait(&set, &number) != 0)
            continue;

        pthread_mutex_lock(&sim->control);

        if (number == SIGUSR2 && sim->shutting_down)
        {
            pthread_mutex_unlock(&sim->control);
            break;
        }

        const char *text = NULL;

        if (number == SIGINT || number == SIGTERM)
        {
            sim->stop_requested = 1;
            text = number == SIGINT ? "SIGINT (Ctrl+C): stop the simulation" : "SIGTERM: stop the simulation";
        }
        else if (number == SIGQUIT)
        {
            sim->close_requested = 1;
            text = "SIGQUIT (Ctrl+\\): close the intake of calls";
        }
        else if (number == SIGUSR1)
        {
            sim->snapshot_requested = 1;
            text = "SIGUSR1: live status requested";
        }

        pthread_mutex_unlock(&sim->control);

        if (text)
        {
            pthread_mutex_lock(&sim->world); // log_event reads the model time
            log_event(sim, "SIGNAL %s", text);
            pthread_mutex_unlock(&sim->world);
        }
    }

    return NULL;
}

