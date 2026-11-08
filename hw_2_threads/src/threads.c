#define _POSIX_C_SOURCE 200809L
#include "thread_internal.h"
#include "utils.h"
#include "utils_extra.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREAD_STACK_SIZE (256 * 1024)

/**
 * @brief Sends a message and registers it in the Quiet counter
 */
void send_message(Simulation *sim, Mailbox *box, MsgType type, int event, int arg)
{
    Message message = {type, event, arg};

    quiet_add(&sim->quiet);
    mailbox_put(box, message);
}

/**
 * @brief Sends MSG_SHUTDOWN to a thread
 */
static void send_shutdown(Mailbox *box)
{
    Message message = {MSG_SHUTDOWN, 0, 0};
    mailbox_put(box, message);
}

/**
 * @brief Creates a thread with a small stack
 */
static int create_thread(pthread_t *thread, void *(*body)(void *), void *arg)
{
    pthread_attr_t attr;

    if (pthread_attr_init(&attr) != 0)
        return 0;

    pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);

    int result = pthread_create(thread, &attr, body, arg);

    pthread_attr_destroy(&attr);

    if (result != 0)
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(result));
        return 0;
    }

    return 1;
}

/**
 * @brief Initializes mutexes, the Quiet counter and the mailboxes
 * @return 1 on success, 0 on failure
 */
int init_thread_sync(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    SOFT_ASSERT(pthread_mutex_init(&sim->world, NULL) == 0, "cannot init world mutex", 0);
    SOFT_ASSERT(pthread_mutex_init(&sim->output_lock, NULL) == 0, "cannot init output mutex", 0);
    SOFT_ASSERT(pthread_mutex_init(&sim->control, NULL) == 0, "cannot init control mutex", 0);
    SOFT_ASSERT(quiet_init(&sim->quiet), "cannot init quiet counter", 0);
    SOFT_ASSERT(mailbox_init(&sim->dispatcher_box, DISPATCHER_BOX_SIZE), "cannot init dispatcher mailbox", 0);

    for (int idx = 0; idx < sim->team_count; idx++)
        SOFT_ASSERT(mailbox_init(&sim->teams[idx].mailbox, ENTITY_BOX_SIZE), "cannot init team mailbox", 0);

    return 1;
}

/**
 * @brief Destroys all synchronization objects (threads must be joined)
 */
void destroy_thread_sync(Simulation *sim)
{
    if (sim == NULL)
        return;

    for (int idx = 0; idx < sim->call_count; idx++)
        mailbox_destroy(&sim->calls[idx].mailbox);

    for (int idx = 0; idx < sim->team_count; idx++)
        mailbox_destroy(&sim->teams[idx].mailbox);

    mailbox_destroy(&sim->dispatcher_box);
    quiet_destroy(&sim->quiet);
    pthread_mutex_destroy(&sim->control);
    pthread_mutex_destroy(&sim->output_lock);
    pthread_mutex_destroy(&sim->world);
}

/**
 * @brief Starts the signal thread, the dispatcher and all team threads
 * @return 1 on success, 0 on failure (already started threads are stopped)
 */
int start_service_threads(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    SOFT_ASSERT(create_thread(&sim->signal_thread, signal_thread, sim), "cannot create signal thread", 0);
    sim->threads_started++;

    if (!create_thread(&sim->dispatcher, dispatcher_thread, sim))
    {
        pthread_mutex_lock(&sim->control);
        sim->shutting_down = 1;
        pthread_mutex_unlock(&sim->control);
        pthread_kill(sim->signal_thread, SIGUSR2);
        pthread_join(sim->signal_thread, NULL);
        return 0;
    }
    sim->threads_started++;

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        ThreadArgs *args = malloc(sizeof(*args));
        SOFT_ASSERT(args, "cannot allocate thread arguments", 0);

        args->sim = sim;
        args->index = idx;

        if (!create_thread(&sim->teams[idx].thread, team_thread, args))
        {
            free(args);
            stop_all_threads(sim);
            return 0;
        }

        sim->teams[idx].started = 1;
        sim->threads_started++;
    }

    return 1;
}

/**
 * @brief Creates the mailbox and the thread of a call
 * @param sim Pointer to the simulation structure
 * @param call_idx Index of the call
 * @return 1 on success, 0 on failure
 */
int start_call_thread(Simulation *sim, int call_idx)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    Call *call = &sim->calls[call_idx];

    SOFT_ASSERT(mailbox_init(&call->mailbox, ENTITY_BOX_SIZE), "cannot init call mailbox", 0);

    ThreadArgs *args = malloc(sizeof(*args));
    if (args == NULL)
    {
        mailbox_destroy(&call->mailbox);
        return 0;
    }

    args->sim = sim;
    args->index = call_idx;

    if (!create_thread(&call->thread, call_thread, args))
    {
        free(args);
        mailbox_destroy(&call->mailbox);
        return 0;
    }

    call->started = 1;
    sim->threads_started++;

    return 1;
}

/**
 * @brief Ends the thread of a closed call and frees its mailbox
 * @return 1 on success
 */
int join_call_thread(Simulation *sim, int call_idx)
{
    Call *call = &sim->calls[call_idx];

    if (!call->started)
        return 1;

    send_shutdown(&call->mailbox);
    pthread_join(call->thread, NULL);
    mailbox_destroy(&call->mailbox);
    call->started = 0;

    return 1;
}

/**
 * @brief Stops and joins all threads
 */
void stop_all_threads(Simulation *sim)
{
    if (sim == NULL)
        return;

    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].started)
            send_shutdown(&sim->calls[idx].mailbox);

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].started)
            send_shutdown(&sim->teams[idx].mailbox);

    send_shutdown(&sim->dispatcher_box);

    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].started)
        {
            pthread_join(sim->calls[idx].thread, NULL);
            sim->calls[idx].started = 0;
        }

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].started)
        {
            pthread_join(sim->teams[idx].thread, NULL);
            sim->teams[idx].started = 0;
        }

    pthread_join(sim->dispatcher, NULL);

    pthread_mutex_lock(&sim->control);
    sim->shutting_down = 1;
    pthread_mutex_unlock(&sim->control);

    pthread_kill(sim->signal_thread, SIGUSR2);
    pthread_join(sim->signal_thread, NULL);
}
