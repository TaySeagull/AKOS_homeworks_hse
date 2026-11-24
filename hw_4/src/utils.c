#define _POSIX_C_SOURCE 200809L
#include "utils_extra.h"
#include "utils.h"
#include "ipc.h"
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

/**
 * @brief Returns the name of a specialization
 * @param specialization The specialization
 * @return The name of the specialization
 */
const char *spec_name(Specialization specialization)
{
    static const char *name[] = {"MEDIC", "REANIM", "FIRE", "POLICE"};

    SOFT_ASSERT(specialization >= 0 && specialization < SPEC_COUNT, "invalid specialization", "UNKNOWN");

    return name[specialization];
}

/**
 * @brief Returns the name of a call state
 * @param call_state The call state
 * @return The name of the call state
 */
const char *call_state_name(CallState call_state)
{
    static const char *name[] =
    {
        "PENDING",
        "WAITING",
        "DISPATCHED",
        "EN_ROUTE",
        "IN_SERVICE",
        "COMPLETED",
        "CANCELLED",
        "UNSERVED"
    };

    SOFT_ASSERT(
        call_state >= CALL_PENDING && call_state <= CALL_UNSERVED,
        "invalid call state",
        "UNKNOWN"
    );

    return name[call_state];
}

/**
 * @brief Returns the name of a team state
 * @param team_state The team state
 * @return The name of the team state
 */
const char *team_state_name(TeamState team_state)
{
    static const char *name[] = {"AVAILABLE", "EN_ROUTE", "AT_SCENE", "RETURNING"};

    SOFT_ASSERT(team_state >= TEAM_AVAILABLE && team_state <= TEAM_RETURNING, "invalid team state", "UNKNOWN");

    return name[team_state];
}

/**
 * @brief Returns the name of a strategy
 * @param strategy The strategy
 * @return The name of the strategy
 */
const char *strategy_name(Strategy strategy)
{
    SOFT_ASSERT(strategy == STRATEGY_NEAREST || strategy == STRATEGY_FASTEST, "invalid strategy", "UNKNOWN");

    return strategy == STRATEGY_FASTEST ? "FASTEST" : "NEAREST";
}

/**
 * @brief Parses a specialization name and returns its enum value
 * @param spec The specialization name
 * @return The specialization enum value, or -1 if not found
 */
int parse_spec(const char *spec)
{
    SOFT_ASSERT(spec, "specialization text is NULL", -1);

    if (!strcasecmp(spec, "MEDIC"))
        return SPEC_MEDIC;

    if (!strcasecmp(spec, "REANIM"))
        return SPEC_REANIM;

    if (!strcasecmp(spec, "FIRE"))
        return SPEC_FIRE;

    if (!strcasecmp(spec, "POLICE"))
        return SPEC_POLICE;

    return -1;
}

/**
 * @brief Finds the index of a district by its name
 * @param sim Pointer to the simulation structure
 * @param name The name of the district to find
 * @return The index of the district, or -1 if not found
 */
int district_index(const Simulation *sim, const char *name)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);
    SOFT_ASSERT(name, "district name is NULL", -1);

    for (int idx = 0; idx < sim->district_count; idx++)
        if (!strcmp(sim->districts[idx].name, name))
            return idx;

    return -1;
}

/**
 * @brief Finds the index of a call by its ID
 * @param sim Pointer to the simulation structure
 * @param id The ID of the call to find
 * @return The index of the call, or -1 if not found
 */
int find_call_index_by_id(const Simulation *sim, int id)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);

    for (int idx = 0; idx < sim->call_count; idx++)
        if (sim->calls[idx].id == id)
            return idx;

    return -1;
}

/**
 * @brief Finds the index of a team by its ID
 * @param sim Pointer to the simulation structure
 * @param id The ID of the team to find
 * @return The index of the team, or -1 if not found
 */
int find_team_index_by_id(const Simulation *sim, int id)
{
    SOFT_ASSERT(sim, "simulation is NULL", -1);

    for (int idx = 0; idx < sim->team_count; idx++)
        if (sim->teams[idx].id == id)
            return idx;

    return -1;
}

/**
 * @brief Writes the whole buffer to a file descriptor
 * @param fd File descriptor
 * @param buf Data to write
 * @param len Number of bytes
 * @return 1 on success, 0 on failure
 */
int write_all(int fd, const char *buf, size_t len)
{
    SOFT_ASSERT(buf, "buffer is NULL", 0);

    while (len > 0)
    {
        ssize_t written = write(fd, buf, len);

        if (written < 0)
        {
            if (errno == EINTR) // interrupted by SIGINT before anything was written
                continue;
            return 0;
        }

        buf += written;
        len -= (size_t)written;
    }

    return 1;
}

/**
 * @brief printf-like output to a file descriptor (vsnprintf + write(2))
 * @param fd File descriptor (STDOUT_FILENO, log descriptor, ...)
 * @param fmt Format string
 * @return 1 on success, 0 on failure
 */
int fd_printf(int fd, const char *fmt, ...)
{
    SOFT_ASSERT(fmt, "format is NULL", 0);

    if (fd < 0)
        return 1; // nothing to write to (log is disabled)

    char buffer[OUT_BUF_LEN];

    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    SOFT_ASSERT(len >= 0, "formatting error", 0);

    if ((size_t)len >= sizeof(buffer)) // the line was truncated
        len = (int)sizeof(buffer) - 1;

    return write_all(fd, buffer, (size_t)len);
}

// name of the current process (role), printed in every log line
static char process_tag[16] = "main";

// descriptor of the log file of this process (-1 = no log); every process
// opens the same file with O_APPEND, so the lines of all processes are kept
static int log_descriptor = -1;

/**
 * @brief Sets the name of the current process for the log
 * @param tag Short name
 */
void set_process_tag(const char *tag)
{
    if (tag == NULL)
        return;

    snprintf(process_tag, sizeof(process_tag), "%s", tag);
}

/**
 * @brief Sets the descriptor of the log file of this process
 * @param fd File descriptor
 */
void set_log_fd(int fd)
{
    log_descriptor = fd;
}

/**
 * @brief Returns the descriptor of the log file of this process
 * @return File descriptor, or -1 if no log is set
 */
int get_log_fd(void)
{
    return log_descriptor;
}

/**
 * @brief Locks the console/log output (process-shared semaphore)
 * @param sim Pointer to the simulation structure
 */
void output_begin(Simulation *sim)
{
    (void)sim;
    ipc_output_lock();
}

/**
 * @brief Unlocks the console/log output
 * @param sim Pointer to the simulation structure
 */
void output_end(Simulation *sim)
{
    (void)sim;
    ipc_output_unlock();
}

/**
 * @brief Logs an event to the console and to the log file
 * @param sim Pointer to the simulation structure
 * @param fmt The format string for the log message
 * @param ... The arguments for the format string
 * @return 1 on success, 0 on failure
 */
int log_event(Simulation *sim, const char *fmt, ...)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(fmt, "log format is NULL", 0);

    char message[OUT_BUF_LEN];

    va_list ap;        // Variable argument list
    va_start(ap, fmt); // Initialize the variable argument list
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    // model time is in minutes: hours = now / 60, minutes = now % 60
    char line[OUT_BUF_LEN + 32];
    // {tag} shows which process (role) produced the line
    int len = snprintf(line, sizeof(line), "[%02d:%02d] {%-4s} %s\n", sim->now / 60, sim->now % 60, process_tag, message);

    if (len < 0)
        return 0;

    if ((size_t)len >= sizeof(line))
        len = (int)sizeof(line) - 1;

    // the same order of lines in the console and in the log file
    ipc_output_lock();

    write_all(STDOUT_FILENO, line, (size_t)len);

    if (log_descriptor >= 0)
        write_all(log_descriptor, line, (size_t)len);

    ipc_output_unlock();

    return 1;
}

/**
 * @brief Pauses the simulation for a specified amount of time
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int sleep_visual(const Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    if (sim->delay_ms <= 0)
        return 1;

    struct timespec timesp = {sim->delay_ms / 1000, (long)(sim->delay_ms % 1000) * 1000000L};
    
    nanosleep(&timesp, NULL);

    return 1;
}
