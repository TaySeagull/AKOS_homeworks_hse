#include "sync.h"
#include "utils.h"
#include <errno.h>
#include <stdlib.h>

/**
 * @brief sem_wait() that is repeated if it was interrupted by a signal
 * @param sem Semaphore
 */
static void sem_wait_retry(sem_t *sem)
{
    while (sem_wait(sem) != 0 && errno == EINTR)
        ;
}

/**
 * @brief Returns the name of the synchronization flavour
 * @return "mutex + semaphores"
 */
const char *sync_flavour(void)
{
    return "mutex + semaphores";
}

/**
 * @brief Initializes a mailbox
 * @param box Pointer to the mailbox
 * @param capacity Maximum number of messages in the mailbox
 * @return 1 on success, 0 on failure
 */
int mailbox_init(Mailbox *box, int capacity)
{
    SOFT_ASSERT(box, "mailbox is NULL", 0);
    SOFT_ASSERT(capacity > 0, "mailbox capacity must be positive", 0);

    box->items = calloc((size_t)capacity, sizeof(Message));
    SOFT_ASSERT(box->items, "cannot allocate mailbox", 0);

    box->capacity = capacity;
    box->head = 0;
    box->count = 0;

    if (pthread_mutex_init(&box->lock, NULL) != 0)
    {
        free(box->items);
        return 0;
    }

    if (sem_init(&box->filled, 0, 0) != 0)
    {
        pthread_mutex_destroy(&box->lock);
        free(box->items);
        return 0;
    }

    if (sem_init(&box->free, 0, (unsigned)capacity) != 0)
    {
        sem_destroy(&box->filled);
        pthread_mutex_destroy(&box->lock);
        free(box->items);
        return 0;
    }

    return 1;
}

/**
 * @brief Destroys a mailbox
 * @param box Pointer to the mailbox
 */
void mailbox_destroy(Mailbox *box)
{
    if (box == NULL || box->items == NULL)
        return;

    sem_destroy(&box->free);
    sem_destroy(&box->filled);
    pthread_mutex_destroy(&box->lock);
    free(box->items);
    box->items = NULL;
}

/**
 * @brief Puts a message into a mailbox
 * @param box Pointer to the mailbox
 * @param message Message to put
 * @return 1 on success
 */
int mailbox_put(Mailbox *box, Message message)
{
    SOFT_ASSERT(box, "mailbox is NULL", 0);

    sem_wait_retry(&box->free); // wait for a free place

    pthread_mutex_lock(&box->lock);
    box->items[(box->head + box->count) % box->capacity] = message;
    box->count++;
    pthread_mutex_unlock(&box->lock);

    sem_post(&box->filled); // one more message for the consumer

    return 1;
}

/**
 * @brief Gets a message from a mailbox
 * @param box Pointer to the mailbox
 * @return The message
 */
Message mailbox_get(Mailbox *box)
{
    Message message = {MSG_SHUTDOWN, 0, 0};

    SOFT_ASSERT(box, "mailbox is NULL", message);

    sem_wait_retry(&box->filled); // wait for a message

    pthread_mutex_lock(&box->lock);
    message = box->items[box->head];
    box->head = (box->head + 1) % box->capacity;
    box->count--;
    pthread_mutex_unlock(&box->lock);

    sem_post(&box->free);

    return message;
}

/**
 * @brief  Quietly initializes the Quiet counter
 * @param quiet Pointer to the Quiet counter
 * @return 1 on success, 0 on failure
 */
int quiet_init(Quiet *quiet)
{
    SOFT_ASSERT(quiet, "quiet is NULL", 0);

    quiet->pending = 0;

    if (pthread_mutex_init(&quiet->lock, NULL) != 0)
        return 0;

    if (sem_init(&quiet->idle, 0, 0) != 0)
    {
        pthread_mutex_destroy(&quiet->lock);
        return 0;
    }

    return 1;
}

/**
 * @brief Quitly destroys the Quiet counter
 * @param quiet Pointer to the Quiet counter
 */
void quiet_destroy(Quiet *quiet)
{
    if (quiet == NULL)
        return;

    sem_destroy(&quiet->idle);
    pthread_mutex_destroy(&quiet->lock);
}

/**
 * @brief Quietly increments the pending counter
 * @param quiet Pointer to the Quiet counter
 */
void quiet_add(Quiet *quiet)
{
    pthread_mutex_lock(&quiet->lock);
    quiet->pending++;
    pthread_mutex_unlock(&quiet->lock);
}

/**
 * @brief Quietly decrements the pending counter and signals if it drops to zero
 * @param quiet Pointer to the Quiet counter
 */
void quiet_done(Quiet *quiet)
{
    pthread_mutex_lock(&quiet->lock);
    quiet->pending--;

    if (quiet->pending == 0)
        sem_post(&quiet->idle);

    pthread_mutex_unlock(&quiet->lock);
}

/**
 * @brief Quietly waits until the pending counter drops to zero
 * @param quiet Pointer to the Quiet counter
 */
void quiet_wait(Quiet *quiet)
{
    sem_wait_retry(&quiet->idle);
}
