#include "sync.h"
#include "utils.h"
#include <stdlib.h>

/**
 * @brief Returns the name of the synchronization flavour
 * @return "mutex + condition variables"
 */
const char *sync_flavour(void)
{
    return "mutex + condition variables";
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

    if (pthread_cond_init(&box->not_empty, NULL) != 0)
    {
        pthread_mutex_destroy(&box->lock);
        free(box->items);
        return 0;
    }

    if (pthread_cond_init(&box->not_full, NULL) != 0)
    {
        pthread_cond_destroy(&box->not_empty);
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

    pthread_cond_destroy(&box->not_full);
    pthread_cond_destroy(&box->not_empty);
    pthread_mutex_destroy(&box->lock);
    free(box->items);
    box->items = NULL;
}

/**
 * @brief Puts a message into a mailbox
 * @param box Pointer to the mailbox
 * @param message Message to put
 * @return 1 on success, 0 on failure
 */
int mailbox_put(Mailbox *box, Message message)
{
    SOFT_ASSERT(box, "mailbox is NULL", 0);

    pthread_mutex_lock(&box->lock);

    while (box->count == box->capacity) // the loop protects from spurious wake-ups
        pthread_cond_wait(&box->not_full, &box->lock);

    box->items[(box->head + box->count) % box->capacity] = message;
    box->count++;

    pthread_cond_signal(&box->not_empty);
    pthread_mutex_unlock(&box->lock);

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

    pthread_mutex_lock(&box->lock);

    while (box->count == 0)
        pthread_cond_wait(&box->not_empty, &box->lock);

    message = box->items[box->head];
    box->head = (box->head + 1) % box->capacity;
    box->count--;

    pthread_cond_signal(&box->not_full);
    pthread_mutex_unlock(&box->lock);

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

    if (pthread_cond_init(&quiet->zero, NULL) != 0)
    {
        pthread_mutex_destroy(&quiet->lock);
        return 0;
    }

    return 1;
}

/**
 * @brief Quietly destroys the Quiet counter
 * @param quiet Pointer to the Quiet counter
 */
void quiet_destroy(Quiet *quiet)
{
    if (quiet == NULL)
        return;

    pthread_cond_destroy(&quiet->zero);
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
        pthread_cond_broadcast(&quiet->zero);

    pthread_mutex_unlock(&quiet->lock);
}

/**
 * @brief Waits until the pending counter drops to zero
 * @param quiet Pointer to the Quiet counter
 */
void quiet_wait(Quiet *quiet)
{
    pthread_mutex_lock(&quiet->lock);

    while (quiet->pending > 0)
        pthread_cond_wait(&quiet->zero, &quiet->lock);

    pthread_mutex_unlock(&quiet->lock);
}
