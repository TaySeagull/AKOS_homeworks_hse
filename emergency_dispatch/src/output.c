#include "output.h"
#include "utils.h"
#include "utils_extra.h"
#include <unistd.h>

#define BOX_WIDTH 62

/**
 * @brief Prints one line inside a box (to stdout through write(2)).
 * @param text Text to print.
 */
static void print_box_line(const char *text)
{
    fd_printf(STDOUT_FILENO, "║ %-60.60s ║\n", text);
}

/**
 * @brief Prints a horizontal line for the live status box.
 * @param left Character for the left corner.
 * @param middle Character for the horizontal line.
 * @param right Character for the right corner.
 */
static void print_snapshot_border(const char *left, const char *middle, const char *right)
{
    fd_printf(STDOUT_FILENO, "%s", left);

    for (int idx = 0; idx < BOX_WIDTH; idx++)
        fd_printf(STDOUT_FILENO, "%s", middle);

    fd_printf(STDOUT_FILENO, "%s\n", right);
}

/**
 * @brief Returns a human-readable description of the stop reason.
 * @param reason Reason why the simulation loop stopped.
 * @return Constant string.
 */
static const char *stop_reason_name(StopReason reason)
{
    switch (reason)
    {
    case STOP_ALL_PROCESSED:
        return "all calls processed";
    case STOP_SHIFT_LIMIT:
        return "shift limit reached";
    case STOP_INTERRUPTED:
        return "interrupted by user";
    case STOP_INTAKE_CLOSED:
        return "intake closed by user, active calls finished";
    default:
        return "unknown";
    }
}

/**
 * @brief Prints the header for the simulation.
 * @param sim Pointer to the simulation structure.
 * @return 1 on success, 0 on failure.
 * Made with the help of AI
 */
int print_header(const Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    fd_printf(STDOUT_FILENO, "\n");
    fd_printf(STDOUT_FILENO, "╔══════════════════════════════════════════════════════════════╗\n");

    print_box_line("              EMERGENCY DISPATCH SIMULATOR");

    fd_printf(STDOUT_FILENO, "╚══════════════════════════════════════════════════════════════╝\n");

    char shift[32];

    if (sim->shift_limit > 0)
        snprintf(shift, sizeof(shift), "%d min", sim->shift_limit);
    else
        snprintf(shift, sizeof(shift), "unlimited (Ctrl+C to stop)");

    fd_printf(STDOUT_FILENO, "Strategy: %-9s | Shift: %s | Delay: %d ms | Return to base: %s\n",
              strategy_name(sim->strategy),
              shift,
              sim->delay_ms,
              sim->return_to_base ? "yes" : "no");

    if (sim->random_calls)
        fd_printf(STDOUT_FILENO, "Random calls: on | seed=%u | mean interval=%d min\n",
                  sim->seed, sim->random_interval);

    return 1;
}

/**
 * @brief Shows a snapshot of the current simulation status.
 * @param sim Pointer to the simulation structure.
 * @return 1 on success, 0 on failure.
 * Made with the help of AI
 */
int show_snapshot(const Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    int waiting = 0;

    for (int idx = 0; idx < sim->call_count; idx++)
    {
        if (sim->calls[idx].state == CALL_WAITING)
            waiting++;
    }

    fd_printf(STDOUT_FILENO, "\n");

    print_snapshot_border("┌", "─", "┐");

    char line[128];

    snprintf(line, sizeof(line), "                LIVE STATUS at [%02d:%02d]", sim->now / 60, sim->now % 60);
    print_box_line(line);

    print_snapshot_border("├", "─", "┤");

    snprintf(
        line,
        sizeof(line),
        "Waiting calls: %-3d  |  Events: %-4d",
        waiting,
        sim->event_count
    );

    print_box_line(line);

    print_snapshot_border("├", "─", "┤");

    for (int idx = 0; idx < sim->team_count; idx++)
    {
        const Team *team = &sim->teams[idx];

        char call_text[16];

        if (team->call_id != NO_CALL)
            snprintf(call_text, sizeof(call_text), "#%03d", team->call_id);
        else
            snprintf(call_text, sizeof(call_text), "---");

        snprintf(
            line,
            sizeof(line),
            "B%-2d   %-7.7s  %-12.12s  district=%-8.8s  call=%-4.4s",
            team->id,
            spec_name(team->spec),
            team_state_name(team->state),
            sim->districts[team->district].name,
            call_text
        );

        print_box_line(line);
    }

    print_snapshot_border("└", "─", "┘");

    return 1;
}

/**
 * @brief Writes the summary lines in plain text form to a descriptor.
 * @param sim Pointer to the simulation structure.
 * @param fd Output descriptor.
 * @param average_waiting Average waiting time.
 * @param average_service Average service time.
 */
static void write_plain_summary(const Simulation *sim, int fd, double average_waiting, double average_service)
{
    fd_printf(fd, "\n=== SHIFT SUMMARY ===\n");
    fd_printf(fd, "Stop reason: %s\n", stop_reason_name(sim->stop_reason));
    fd_printf(fd, "Calls received: %d\n", sim->received);
    fd_printf(fd, "Served: %d\n", sim->served);
    fd_printf(fd, "Cancelled: %d\n", sim->cancelled);
    fd_printf(fd, "Unserved: %d\n", sim->unserved);
    fd_printf(fd, "Dispatches: %d\n", sim->dispatches);
    fd_printf(fd, "Priority escalations: %d\n", sim->escalations);
    fd_printf(fd, "Average waiting: %.2f min/call\n", average_waiting);
    fd_printf(fd, "Average service: %.2f min/call\n", average_service);
    fd_printf(fd, "Simulation end: %d min\n", sim->now);
    fd_printf(fd, "Processed events: %d\n", sim->completed_events);
    fd_printf(fd, "Invariant violations: %d\n", sim->invariant_errors);
}

/**
 * @brief Prints the final simulation summary.
 * @details Calls received = served + cancelled + unserved always holds,
 * because every call that arrived ends in exactly one of these states.
 * @param sim Pointer to the simulation structure.
 * @return 1 on success, 0 on failure.
 * Made with the help of AI
 */
int print_summary(const Simulation *sim)
{
    SOFT_ASSERT(sim, "simulation is NULL", 0);

    double average_waiting = 0.0;
    double average_service = 0.0;

    if (sim->served > 0)
    {
        average_waiting = (double)sim->total_wait / sim->served;
        average_service = (double)sim->total_service / sim->served;
    }

    char line[128];

    fd_printf(STDOUT_FILENO, "\n");
    fd_printf(STDOUT_FILENO, "╔══════════════════════════════════════════════════════════════╗\n");

    print_box_line("                       SHIFT SUMMARY");

    fd_printf(STDOUT_FILENO, "╠══════════════════════════════════════════════════════════════╣\n");

    snprintf(line, sizeof(line), "Stop reason:          %s", stop_reason_name(sim->stop_reason));
    print_box_line(line);

    snprintf(line, sizeof(line), "Calls received:       %d", sim->received);
    print_box_line(line);

    snprintf(line, sizeof(line), "Served:               %d", sim->served);
    print_box_line(line);

    snprintf(line, sizeof(line), "Cancelled:            %d", sim->cancelled);
    print_box_line(line);

    snprintf(line, sizeof(line), "Unserved:             %d", sim->unserved);
    print_box_line(line);

    snprintf(line, sizeof(line), "Dispatches:           %d", sim->dispatches);
    print_box_line(line);

    snprintf(line, sizeof(line), "Priority escalations: %d", sim->escalations);
    print_box_line(line);

    snprintf(line, sizeof(line), "Average waiting:      %.2f min/call", average_waiting);
    print_box_line(line);

    snprintf(line, sizeof(line), "Average service:      %.2f min/call", average_service);
    print_box_line(line);

    snprintf(line, sizeof(line), "Simulation end:       %d min", sim->now);
    print_box_line(line);

    snprintf(line, sizeof(line), "Invariant violations: %d", sim->invariant_errors);
    print_box_line(line);

    fd_printf(STDOUT_FILENO, "╚══════════════════════════════════════════════════════════════╝\n");

    if (sim->log_fd >= 0)
        write_plain_summary(sim, sim->log_fd, average_waiting, average_service);

    return 1;
}

/**
 * @brief Prints the command-line usage information.
 * @param program The name of the program.
 * @return 1 on success, 0 on failure.
 */
int print_usage(const char *program)
{
    SOFT_ASSERT(program, "program name is NULL", 0);

    fd_printf(
        STDOUT_FILENO,
        "Usage: %s [-c config] [-d delay_ms] [-s nearest|fastest] [-l logfile]\n"
        "          [-t shift_minutes] [-r seed] [-h]\n"
        "  -c file    configuration file (default config.txt)\n"
        "  -d ms      visual delay after every event, ms (0 = no delay)\n"
        "  -s name    team selection strategy: nearest | fastest\n"
        "  -l file    log file (default dispatch.log)\n"
        "  -t min     shift length in minutes, 0 = unlimited (stop with Ctrl+C)\n"
        "  -r seed    enable the random call generator with the given seed\n"
        "  -h         show this help\n"
        "Signals: Ctrl+C (SIGINT) or SIGTERM - stop now, active calls become unserved;\n"
        "         Ctrl+\\ (SIGQUIT) - stop accepting calls and finish the active ones;\n"
        "         SIGUSR1 - print the live status.\n"
        "Command-line options override the values from the configuration file.\n",
        program
    );

    return 1;
}
