#define _POSIX_C_SOURCE 200809L
#include "sems.h"
#include "utils.h"

static sem_t *table;                     // array in the shared memory
static unsigned char created[SEM_TOTAL]; // 1 = initialised by this process

/**
 * @brief Remembers where the array of semaphores is
 * @param prefix Not used for unnamed semaphores
 * @param shared_array Array in the shared memory
 */
void sems_setup(const char *prefix, sem_t *shared_array)
{
    (void)prefix;
    table = shared_array;
}

/**
 * @brief Initialises an unnamed process-shared semaphore
 * @param id Number of the semaphore
 * @param value Initial value
 * @return 1 on success, 0 on failure
 */
int sems_create(int id, unsigned value)
{
    SOFT_ASSERT(table, "semaphore array is not set", 0);
    SOFT_ASSERT(id >= 0 && id < SEM_TOTAL, "semaphore id is out of range", 0);

    if (sem_init(&table[id], 1, value) != 0) // pshared = 1: shared between processes
    {
        perror("sem_init");
        return 0;
    }

    created[id] = 1;
    return 1;
}

/**
 * @brief Returns the address of the semaphore in the shared memory
 * @param id Number of the semaphore
 * @return Pointer to the semaphore or NULL
 */
sem_t *sem_ref(int id)
{
    if (table == NULL || id < 0 || id >= SEM_TOTAL)
        return NULL;

    return &table[id];
}

/**
 * @brief Unnamed semaphores exist as long as the memory is mapped
 * @param id Number of the semaphore
 * @return 1 if it exists, 0 otherwise
 */
int sems_exist(int id)
{
    return sem_ref(id) != NULL;
}

/**
 * @brief Destroys the semaphores initialised by this process
 */
void sems_destroy_all(void)
{
    if (table == NULL)
        return;

    for (int id = 0; id < SEM_TOTAL; id++)
        if (created[id])
        {
            sem_destroy(&table[id]);
            created[id] = 0;
        }

    table = NULL;
}

/**
 * @brief Kind of the semaphores
 */
const char *sems_kind(void)
{
    return "unnamed POSIX semaphores in shared memory";
}
