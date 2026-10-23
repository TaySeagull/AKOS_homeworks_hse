/**
 * @file threads.c- 2d hw
 */
#include "utils.h"
#include "threads.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Waits for assignments of the emergency and terminates when shutdown is requested
 * @return NULL when the thread terminates
 */
static void *team_thread(void *arg)
{
    if (arg == NULL)
    {
        fprintf(stderr, "team_thread: arg is NULL\n");
        return NULL;
    }

    TeamThreadArgs *args = arg;
    Simulation *sim = args->sim;
    Team *team = args->team;
    free(args);

    for (;;)
    {
        while (sem_wait(&team->assignment_sem) != 0)
        {
            if (errno != EINTR)
                return NULL;
        }

        if (pthread_mutex_lock(&sim->mutex) != 0)
            return NULL;

        int shutdown = sim->shutdown_requested;
        TeamCommand command = team->pending_command;
        int call_id = team->call_id;
        int district = team->command_district;

        if (!shutdown)
        {
            switch (command)
            {
            case TEAM_CMD_ASSIGN:
                team->state = TEAM_EN_ROUTE;
                break;

            case TEAM_CMD_ARRIVE:
                team->district = district;
                team->state = TEAM_AT_SCENE;
                break;

            case TEAM_CMD_SERVICE_DONE:
                team->call_id = -1;
                team->state = sim->return_to_base && team->district != team->base
                                  ? TEAM_RETURNING
                                  : TEAM_AVAILABLE;
                break;

            case TEAM_CMD_RETURN_DONE:
                team->district = team->base;
                team->state = TEAM_AVAILABLE;
                break;

            case TEAM_CMD_NONE:

            default:
                break;
            }

            team->pending_command = TEAM_CMD_NONE;
        }

        pthread_mutex_unlock(&sim->mutex);

        if (shutdown)
            break;

        if (command == TEAM_CMD_ASSIGN)
            printf("TEAM %d: accepted call %d and started travel\n", team->id, call_id);

        else if (command == TEAM_CMD_ARRIVE)
            printf("TEAM %d: arrived in district %d\n", team->id, district);

        else if (command == TEAM_CMD_SERVICE_DONE)
            printf("TEAM %d: service finished\n", team->id);

        else if (command == TEAM_CMD_RETURN_DONE)
            printf("TEAM %d: returned to base\n", team->id);

        if (sem_post(&team->assignment_done_sem) != 0)
            return NULL;
    }

    return NULL;
}

/**
 * @brief Sends a command to a team worker and waits for its state transition
 * @return 1 when the worker has processed the command, otherwise 0
 */
int notify_team_thread(Simulation *sim, Team *team, TeamCommand command, int district)
{
    if (sim == NULL || team == NULL)
    {
        fprintf(stderr, "notify_team_thread: invalid argument\n");
        return 0;
    }

    if (pthread_mutex_lock(&sim->mutex) != 0)
        return 0;

    if (sim->shutdown_requested)
    {
        pthread_mutex_unlock(&sim->mutex);
        return 0;
    }

    team->pending_command = command;
    team->command_district = district;

    pthread_mutex_unlock(&sim->mutex);

    if (sem_post(&team->assignment_sem) != 0)
    {
        perror("sem_post assignment_sem");
        return 0;
    }

    while (sem_wait(&team->assignment_done_sem) != 0)
    {
        if (errno != EINTR)
        {
            perror("sem_wait assignment_done_sem");
            return 0;
        }
    }

    return 1;
}

/**
 * @brief Initializes the simulation mutex and team semaphores
 * @return 1 on success, 0 on failure
 */
int init_thread_sync(Simulation *sim)
{
    SOFT_ASSERT(sim != NULL, "simulation is NULL", 0);

    SOFT_ASSERT(
        pthread_mutex_init(&sim->mutex, NULL) == 0,
        "Failed to initialize simulation mutex",
        0);

    sim->shutdown_requested = 0;

    int initialized = 0;

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        if (sem_init(&sim->teams[idx].assignment_sem, 0, 0) != 0)
        {
            perror("sem_init");

            for (int jdx = 0; jdx < initialized; jdx++)
            {
                sem_destroy(&sim->teams[jdx].assignment_sem);
            }

            pthread_mutex_destroy(&sim->mutex);
            return 0;
        }

        if (sem_init(&sim->teams[idx].assignment_done_sem, 0, 0) != 0)
        {
            perror("sem_init");

            sem_destroy(&sim->teams[idx].assignment_sem);

            for (int jdx = 0; jdx < initialized; jdx++)
            {
                sem_destroy(&sim->teams[jdx].assignment_sem);
                sem_destroy(&sim->teams[jdx].assignment_done_sem);
            }

            pthread_mutex_destroy(&sim->mutex);
            return 0;
        }

        initialized++;
    }

    return 1;
}

/**
 * @brief Creates one thread for each team
 * @return 1 on success, 0 on failure
 */
int start_team_threads(Simulation *sim)
{
    SOFT_ASSERT(sim != NULL, "simulation is NULL", 0);

    int created = 0;

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        TeamThreadArgs *args = malloc(sizeof(*args));

        if (args == NULL)
        {
            fprintf(stderr, "Failed to allocate team thread arguments\n");
            break;
        }

        args->sim = sim;
        args->team = &sim->teams[idx];

        int result = pthread_create(
            &sim->teams[idx].thread,
            NULL,
            team_thread,
            args);

        if (result != 0)
        {
            fprintf(stderr, "Failed to create thread for team %d: %d\n",
                    sim->teams[idx].id, result);
            free(args);
            break;
        }

        created++;
    }

    if (created == sim->team_count)
        return 1;

    pthread_mutex_lock(&sim->mutex);
    sim->shutdown_requested = 1;
    pthread_mutex_unlock(&sim->mutex);

    for (int idx = 0; idx < created; idx++)
        sem_post(&sim->teams[idx].assignment_sem);

    for (int idx = 0; idx < created; idx++)
        pthread_join(sim->teams[idx].thread, NULL);

    return 0;
}

/**
 * @brief Requests team threads to stop and waits for their termination
 * @param sim Pointer to the simulation structure
 */
void stop_team_threads(Simulation *sim)
{
    if (sim == NULL)
    {
        fprintf(stderr, "stop_team_threads: simulation is NULL\n");
        return;
    }

    if (pthread_mutex_lock(&sim->mutex) != 0)
    {
        fprintf(stderr, "stop_team_threads: failed to lock mutex\n");
        return;
    }

    sim->shutdown_requested = 1;

    pthread_mutex_unlock(&sim->mutex);

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        if (sem_post(&sim->teams[idx].assignment_sem) != 0)
            perror("sem_post");
    }

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        if (pthread_join(sim->teams[idx].thread, NULL) != 0)
            fprintf(stderr, "Failed to join team thread %d\n", sim->teams[idx].id);
    }
}

/**
 * @brief Destroys the mutex and team semaphores
 */
void destroy_thread_sync(Simulation *sim)
{
    if (sim == NULL)
    {
        fprintf(stderr, "destroy_thread_sync: simulation is NULL\n");
        return;
    }

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        if (sem_destroy(&sim->teams[idx].assignment_sem) != 0)
            perror("sem_destroy assignment_sem");

        if (sem_destroy(&sim->teams[idx].assignment_done_sem) != 0)
            perror("sem_destroy assignment_done_sem");
    }

    if (pthread_mutex_destroy(&sim->mutex) != 0)
        perror("pthread_mutex_destroy");
}