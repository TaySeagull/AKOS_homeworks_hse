#define _POSIX_C_SOURCE 200809L
#include "sems.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>


static char sem_prefix[64] = "/ed35_";   // prefix of the names of this session
static sem_t *opened[SEM_TOTAL];         // semaphores opened by this process
static unsigned char created[SEM_TOTAL]; // 1 = created by this process, must be unlinked

/**
 * @brief Builds a readable name of the semaphore
 * @param id Number of the semaphore
 * @param buf Buffer for the name
 * @param size Size of the buffer
 */
static void sem_name(int id, char *buf, size_t size)
{
    static const char *const fixed[] = {"world", "output", "quiet_lock", "quiet_idle", "ready",
                                        "exited", "sent", "claim_dispatcher", "claim_callers"};

    if (id < SEM_CLAIM_TEAM0)
    {
        snprintf(buf, size, "%s%s", sem_prefix, fixed[id]);
        return;
    }

    if (id < SEM_BOX0)
    {
        snprintf(buf, size, "%sclaim_team%d", sem_prefix, id - SEM_CLAIM_TEAM0);
        return;
    }

    int box = (id - SEM_BOX0) / 3;
    static const char *const part[] = {"lock", "filled", "free"};
    const char *what = part[(id - SEM_BOX0) % 3];

    if (box == BOX_IDX_DISPATCHER)
        snprintf(buf, size, "%sbox_dispatcher_%s", sem_prefix, what);
    else if (box == BOX_IDX_CALLERS)
        snprintf(buf, size, "%sbox_callers_%s", sem_prefix, what);
    else if (box < BOX_IDX_CALLERS)
        snprintf(buf, size, "%sbox_team%d_%s", sem_prefix, box - BOX_IDX_TEAM0, what);
    else
        snprintf(buf, size, "%sbox_call%d_%s", sem_prefix, box - BOX_IDX_CALL0, what);
}

/**
 * @brief Sets the prefix of the names
 * @param prefix Prefix ("/ed35_<session>_")
 * @param shared_array Not used for named semaphores
 */
void sems_setup(const char *prefix, sem_t *shared_array)
{
    (void)shared_array;

    if (prefix)
        snprintf(sem_prefix, sizeof(sem_prefix), "%s", prefix);
}

/**
 * @brief Creates a named semaphore; a stale one from a crashed run is removed first
 * @param id Number of the semaphore
 * @param value Initial value
 * @return 1 on success, 0 on failure
 */
int sems_create(int id, unsigned value)
{
    SOFT_ASSERT(id >= 0 && id < SEM_TOTAL, "semaphore id is out of range", 0);

    char name[128];
    sem_name(id, name, sizeof(name));

    sem_t *sem = sem_open(name, O_CREAT | O_EXCL, 0600, value);

    if (sem == SEM_FAILED && errno == EEXIST)
    {
        sem_unlink(name); // left by a run that was killed with SIGKILL
        sem = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    }

    if (sem == SEM_FAILED)
    {
        perror(name);
        return 0;
    }

    opened[id] = sem;
    created[id] = 1;
    return 1;
}

/**
 * @brief Returns the semaphore, opens it by name on the first use
 * @param id Number of the semaphore
 * @return Pointer to the semaphore or NULL
 */
sem_t *sem_ref(int id)
{
    if (id < 0 || id >= SEM_TOTAL)
        return NULL;

    if (opened[id] == NULL)
    {
        char name[128];
        sem_name(id, name, sizeof(name));

        sem_t *sem = sem_open(name, 0);
        if (sem == SEM_FAILED)
            return NULL;

        opened[id] = sem;
    }

    return opened[id];
}

/**
 * @brief Checks if a named semaphore exists
 * @param id Number of the semaphore
 * @return 1 if it exists
 */
int sems_exist(int id)
{
    return sem_ref(id) != NULL;
}

/**
 * @brief Closes all opened semaphores and unlinks the ones created by this process
 */
void sems_destroy_all(void)
{
    for (int id = 0; id < SEM_TOTAL; id++)
    {
        if (opened[id])
        {
            sem_close(opened[id]);
            opened[id] = NULL;
        }

        if (created[id])
        {
            char name[128];
            sem_name(id, name, sizeof(name));
            sem_unlink(name);
            created[id] = 0;
        }
    }
}

/**
 * @brief Kind of the semaphores
 */
const char *sems_kind(void)
{
    return "named POSIX semaphores";
}
