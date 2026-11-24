#define _POSIX_C_SOURCE 200809L
#include "sems.h"
#include <errno.h>
#include <time.h>

/**
 * @brief Waits for a semaphore; a signal (EINTR) does not break the wait
 * @param sem Semaphore
 * @return 0 on success, -1 on error
 */
int sem_wait_retry(sem_t *sem)
{
    if (sem == NULL)
        return -1;

    while (sem_wait(sem) != 0)
    {
        if (errno != EINTR)
            return -1;
    }

    return 0;
}

/**
 * @brief Waits for a semaphore not longer than ms milliseconds
 * @param sem Semaphore
 * @param ms Timeout in milliseconds
 * @return 0 if the semaphore was taken, -1 on timeout, signal or error
 */
int sem_wait_ms(sem_t *sem, int ms)
{
    if (sem == NULL)
        return -1;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);

    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;

    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    return sem_timedwait(sem, &deadline) == 0 ? 0 : -1;
}
