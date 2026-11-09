#include "config.h"
#include "utils.h"
#include "utils_extra.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>

#define KEY_LEN 64  // Maximum length of a key in the configuration file
#define VAL_LEN 128 // Maximum length of a value in the configuration file

/**
 * @brief Sets the default values for the simulation
 * @param sim Pointer to the simulation structure
 * @return 1 on success, 0 on failure
 */
int set_defaults(Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    // memset allows to initialize the entire structure to zero
    memset(sim, 0, sizeof(*sim));

    sim->delay_ms = 100;         // standard delay between events
    sim->shift_limit = 720;      // 12 hours shift (looked in the internet)
    sim->escalation_enabled = 1; // enable escalation
    sim->return_to_base = 1;     // return to base after service
    sim->strategy = STRATEGY_NEAREST;
    sim->stop_reason = STOP_ALL_PROCESSED;
    sim->random_calls = 0;       // calls are taken from [CALLS] only
    sim->seed = 1;
    sim->random_interval = 10;   // a new random call every ~10 minutes
    sim->next_call_id = 1;

    return 1;
}

/**
 * @brief Converts a string to int and checks that the whole string is a number
 * @param text Text to convert
 * @param value Where to store the result
 * @return 1 on success, 0 if the text is not an integer
 */
int parse_int_strict(const char *text, int *value)
{
    SOFT_ASSERT(text, "text is NULL", 0);
    SOFT_ASSERT(value, "value pointer is NULL", 0);

    char *end = NULL;
    errno = 0;
    long number = strtol(text, &end, 10);

    if (end == text || *end != '\0' || errno == ERANGE || number < INT_MIN || number > INT_MAX)
        return 0;

    *value = (int)number;
    return 1;
}

/**
 * @brief Parses a settings line and updates the simulation accordingly
 * @param sim Pointer to the simulation structure
 * @param line The line to parse
 * @return 1 on success, 0 on failure
 */
static int parse_settings_line(Simulation *sim, const char *line)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(line, "configuration line is NULL", 0);

    char key[KEY_LEN], val[VAL_LEN];
    if (sscanf(line, " %63[^= \t] = %127s", key, val) != 2)
    {
        fprintf(stderr, "Config error: invalid settings line: %s", line);
        return 0;
    }

    if (!strcmp(key, "strategy"))
    {
        if (!strcasecmp(val, "FASTEST"))
            sim->strategy = STRATEGY_FASTEST;
        else if (!strcasecmp(val, "NEAREST"))
            sim->strategy = STRATEGY_NEAREST;
        else
        {
            fprintf(stderr, "Config error: unknown strategy '%s' (use NEAREST or FASTEST)\n", val);
            return 0;
        }
        return 1;
    }

    int number = 0;
    if (!parse_int_strict(val, &number))
    {
        fprintf(stderr, "Config error: value of '%s' must be an integer, got '%s'\n", key, val);
        return 0;
    }

    if (!strcmp(key, "delay_ms"))
        sim->delay_ms = number;

    else if (!strcmp(key, "shift_limit"))
        sim->shift_limit = number;

    else if (!strcmp(key, "escalation_enabled"))
        sim->escalation_enabled = number;

    else if (!strcmp(key, "return_to_base"))
        sim->return_to_base = number;

    else if (!strcmp(key, "random_calls"))
        sim->random_calls = number;

    else if (!strcmp(key, "random_seed"))
        sim->seed = (unsigned int)number;

    else if (!strcmp(key, "random_interval"))
        sim->random_interval = number;

    else
        fprintf(stderr, "Config warning: unknown setting '%s' is ignored\n", key);

    return 1;
}

/**
 * @brief Parses a district line and updates the simulation accordingly
 * @param sim Pointer to the simulation structure
 * @param line The line to parse
 * @return 1 on success, 0 on failure
 */
static int parse_district_line(Simulation *sim, const char *line)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(line, "district line is NULL", 0);
    SOFT_ASSERT(sim->district_count < MAX_DISTRICTS, "too many districts", 0);

    District *distrct = &sim->districts[sim->district_count];
    char name[NAME_LEN];
    int each_distrct_time[MAX_DISTRICTS];

    int number = sscanf(line, "%31s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", name, &each_distrct_time[0], &each_distrct_time[1], &each_distrct_time[2], &each_distrct_time[3], &each_distrct_time[4], &each_distrct_time[5], &each_distrct_time[6], &each_distrct_time[7], &each_distrct_time[8], &each_distrct_time[9], &each_distrct_time[10], &each_distrct_time[11], &each_distrct_time[12], &each_distrct_time[13], &each_distrct_time[14], &each_distrct_time[15]);
    SOFT_ASSERT(number >= 2, "invalid district line", 0);
    SOFT_ASSERT(district_index(sim, name) < 0, "duplicate district name", 0);

    strcpy(distrct->name, name); // copy the district name

    // Initialize all travel times to -1
    // -1 because it indicates that the travel time is not set for that district
    for (int idx = 0; idx < MAX_DISTRICTS; idx++)
        distrct->travel[idx] = -1;

    // Copy travel times into the district's travel array
    for (int idx = 0; idx < number - 1; idx++)
        distrct->travel[idx] = each_distrct_time[idx];

    distrct->travel_count = number - 1;

    sim->district_count++;
    return 1;
}

/**
 * @brief Parses a team line and updates the simulation accordingly
 * @param sim Pointer to the simulation structure
 * @param line The line to parse
 * @return 1 on success, 0 on failure
 */
static int parse_team_line(Simulation *sim, const char *line)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(line, "team line is NULL", 0);
    SOFT_ASSERT(sim->team_count < MAX_TEAMS, "too many teams", 0);

    int id = 0, speed = 0;
    char spec_name[NAME_LEN], district_name[NAME_LEN];
    SOFT_ASSERT(sscanf(line, "%d %31s %d %31s", &id, spec_name, &speed, district_name) == 4, "invalid team line", 0);

    int distrct = district_index(sim, district_name);
    int spec = parse_spec(spec_name);

    SOFT_ASSERT(distrct >= 0, "unknown team district", 0);
    SOFT_ASSERT(spec >= 0, "unknown team specialization", 0);
    SOFT_ASSERT(speed > 0, "team speed must be positive", 0);

    Team *team = &sim->teams[sim->team_count];
    team->id = id;
    team->spec = (Specialization)spec;
    team->speed = speed;
    team->district = distrct;
    team->base = distrct;
    team->state = TEAM_AVAILABLE;
    team->call_id = NO_CALL;

    sim->team_count++;
    return 1;
}

/**
 * @brief Parses a call line and updates the simulation accordingly
 * @param sim Pointer to the simulation structure
 * @param line The line to parse
 * @return 1 on success, 0 on failure
 */
static int parse_call_line(Simulation *sim, const char *line)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(line, "call line is NULL", 0);
    SOFT_ASSERT(sim->call_count < MAX_CALLS, "too many calls", 0);

    int id = 0;
    int arrival = 0;
    int x = 0;
    int y = 0;
    int priority = 0;
    int service = 0;
    int max_wait = 0;
    int esc = 0;
    int cancel = 0;

    char spec_name[NAME_LEN];
    char district_name[NAME_LEN];

    int number = sscanf(
        line,
        "%d %d %31s %d %d %d %31s %d %d %d %d",
        &id,
        &arrival,
        district_name,
        &x,
        &y,
        &priority,
        spec_name,
        &service,
        &max_wait,
        &esc,
        &cancel);

    SOFT_ASSERT(number >= 10, "invalid call line", 0);

    int distrct = district_index(sim, district_name);
    int spec = parse_spec(spec_name);

    SOFT_ASSERT(distrct >= 0, "unknown call district", 0);
    SOFT_ASSERT(spec >= 0, "unknown call specialization", 0);
    SOFT_ASSERT(arrival >= 0, "call arrival must be non-negative", 0);

    Call *call = &sim->calls[sim->call_count];

    call->id = id;
    call->arrival = arrival;
    call->district = distrct;
    call->x = x;
    call->y = y;
    call->priority = priority;
    call->spec = spec;
    call->service_duration = service;
    call->max_wait = max_wait;
    call->escalation_after = esc;
    call->cancel_after = number >= 11 ? cancel : 0;

    // The call has not arrived yet
    call->state = CALL_PENDING;

    call->assigned_team = NO_TEAM;
    call->dispatch_time = 0;
    call->service_start = 0;
    call->completion_time = 0;

    sim->call_count++;

    return 1;
}

/**
 * @brief Sets the current section
 * @param section Pointer to the section variable
 * @param line The line to parse
 */
static void set_section(int *section, const char *line)
{
    SOFT_ASSERT(section, "section pointer is NULL", (void)0);
    SOFT_ASSERT(line, "line is NULL", (void)0);

    if (!strncmp(line, "[DISTRICTS]", 11))
        *section = 1;
    else if (!strncmp(line, "[TEAMS]", 7))
        *section = 2;
    else if (!strncmp(line, "[CALLS]", 7))
        *section = 3;
    else if (!strncmp(line, "[SETTINGS]", 10))
        *section = 0;
}

/**
 * @brief Loads the configuration from a file
 * @param sim Pointer to the simulation structure
 * @param path Path to the configuration file
 * @return 1 on success, 0 on failure
 */
int load_config(Simulation *sim, const char *path)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(path, "config path is NULL", 0);

    FILE *file = fopen(path, "r");
    SOFT_ASSERT(file, "cannot open configuration file", 0);

    char line[LINE_LEN];
    int section = 0; // 0 = settings, 1 = districts, 2 = teams, 3 = calls

    while (fgets(line, sizeof(line), file))
    {
        char *elem = line;
        while (*elem == ' ' || *elem == '\t')
            elem++;

        if (*elem == '#' || *elem == '\n' || *elem == '\0')
            continue;

        if (!strncmp(elem, "[DISTRICTS]", 11) || !strncmp(elem, "[TEAMS]", 7) || !strncmp(elem, "[CALLS]", 7) || !strncmp(elem, "[SETTINGS]", 10))
        {
            set_section(&section, elem);
            continue;
        }

        int ok = 1; // flag to indicate if the line was parsed successfully
        if (section == 0)
            ok = parse_settings_line(sim, elem);

        else if (section == 1)
            ok = parse_district_line(sim, elem);

        else if (section == 2)
            ok = parse_team_line(sim, elem);

        else if (section == 3)
            ok = parse_call_line(sim, elem);

        if (!ok)
        {
            fclose(file);
            return 0;
        }
    }

    fclose(file);

    if (sim->district_count == 0 || sim->team_count == 0)
    {
        fprintf(stderr, "Config error: %s must contain [DISTRICTS] and [TEAMS] sections\n", path);
        return 0;
    }

    return 1;
}

/**
 * @brief Checks whether IDs are unique
 * @param sim Pointer to the simulation structure
 * @return 1 if IDs are unique, 0 otherwise
 */
static int ids_are_unique(const Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    // Check for duplicate call IDs
    for (int idx = 0; idx < sim->call_count; idx++)
        for (int j = idx + 1; j < sim->call_count; j++)
            if (sim->calls[idx].id == sim->calls[j].id)
                return 0;

    // Check for duplicate team IDs
    for (int idx = 0; idx < sim->team_count; idx++)
        for (int j = idx + 1; j < sim->team_count; j++)
            if (sim->teams[idx].id == sim->teams[j].id)
                return 0;

    return 1;
}

/**
 * @brief Validates the simulation configuration
 * @param sim Pointer to the simulation structure
 * @return 1 if the configuration is valid, 0 otherwise
 */
int validate_simulation(const Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);
    SOFT_ASSERT(sim->district_count > 0, "no districts configured", 0);
    SOFT_ASSERT(sim->team_count > 0, "no teams configured", 0);
    SOFT_ASSERT(sim->shift_limit >= 0, "shift limit must be non-negative (0 = unlimited)", 0);
    SOFT_ASSERT(sim->random_calls == 0 || sim->random_calls == 1, "random_calls must be 0 or 1", 0);
    SOFT_ASSERT(sim->random_interval > 0, "random_interval must be positive", 0);
    SOFT_ASSERT(sim->call_count > 0 || sim->random_calls, "no calls configured and the random generator is off", 0);
    SOFT_ASSERT(sim->delay_ms >= 0, "delay must be non-negative", 0);
    SOFT_ASSERT(sim->escalation_enabled == 0 || sim->escalation_enabled == 1, "escalation_enabled must be 0 or 1", 0);
    SOFT_ASSERT(sim->return_to_base == 0 || sim->return_to_base == 1, "return_to_base must be 0 or 1", 0);
    SOFT_ASSERT(sim->strategy == STRATEGY_NEAREST || sim->strategy == STRATEGY_FASTEST, "invalid strategy", 0);
    SOFT_ASSERT(ids_are_unique(sim), "IDs of calls and teams must be unique", 0);

    // Validate districts
    for (int idx = 0; idx < sim->district_count; idx++)
    {
        SOFT_ASSERT(sim->districts[idx].name[0] != '\0', "district name is empty", 0);

        if (sim->districts[idx].travel_count != sim->district_count)
        {
            fprintf(stderr, "Config error: district %s has %d travel times, expected %d\n",
                    sim->districts[idx].name, sim->districts[idx].travel_count, sim->district_count);
            return 0;
        }

        for (int jdx = 0; jdx < sim->district_count; jdx++)
            SOFT_ASSERT(sim->districts[idx].travel[jdx] >= -1, "invalid travel time", 0);

        SOFT_ASSERT(sim->districts[idx].travel[idx] == 0, "travel time from a district to itself must be 0", 0);
    }

    // Validate teams
    for (int idx = 0; idx < sim->team_count; idx++)
    {
        const Team *team = &sim->teams[idx];
        SOFT_ASSERT(team->spec >= 0 && team->spec < SPEC_COUNT, "invalid team specialization", 0);
        SOFT_ASSERT(team->speed > 0, "team speed must be positive", 0);
        SOFT_ASSERT(team->district >= 0 && team->district < sim->district_count, "team district is out of range", 0);
        SOFT_ASSERT(team->id > 0, "team id must be positive", 0);
    }

    // Validate calls
    for (int idx = 0; idx < sim->call_count; idx++)
    {
        const Call *call = &sim->calls[idx];
        SOFT_ASSERT(call->priority >= 1 && call->priority <= 3, "priority must be 1..3", 0);
        SOFT_ASSERT(call->max_wait > 0, "max_wait must be positive", 0);
        SOFT_ASSERT(call->service_duration > 0, "service duration must be positive", 0);
        SOFT_ASSERT(call->district >= 0 && call->district < sim->district_count, "call district is out of range", 0);
        SOFT_ASSERT(call->escalation_after >= 0, "escalation_after must be non-negative", 0);
        SOFT_ASSERT(call->cancel_after >= 0, "cancel_after must be non-negative", 0);
        SOFT_ASSERT(call->id > 0, "call id must be positive", 0);
    }

    return 1;
}

