#include "output.h"
#include "utils.h"
#include "utils_extra.h"
#include <stdio.h>

#define BOX_WIDTH 62
#define BOX_TEXT_WIDTH 60

/**
 * @brief Prints one line inside a box.
 * @param text Text to print.
 */
static void print_box_line(const char *text)
{
    printf("║ %-60.60s ║\n", text);
}

/**
 * @brief Prints a horizontal line for the live status box.
 * @param left Character for the left corner.
 * @param middle Character for the horizontal line.
 * @param right Character for the right corner.
 */
static void print_snapshot_border(const char *left, const char *middle, const char *right)
{
    printf("%s", left);

    for (int idx = 0; idx < BOX_WIDTH; idx++)
        printf("%s", middle);

    printf("%s\n", right);
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

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");

    print_box_line("              EMERGENCY DISPATCH SIMULATOR");

    printf("╚══════════════════════════════════════════════════════════════╝\n");

    printf("Strategy: %-9s | Shift: %d min | Delay: %d ms\n",
           strategy_name(sim->strategy),
           sim->shift_limit,
           sim->delay_ms);

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

    printf("\n");

    print_snapshot_border("┌", "─", "┐");

    print_box_line("                    LIVE STATUS");

    print_snapshot_border("├", "─", "┤");

    char line[128];

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

        if (team->call_id)
        {
            snprintf(
                call_text,
                sizeof(call_text),
                "#%03d",
                team->call_id
            );
        }
        else
        {
            snprintf(
                call_text,
                sizeof(call_text),
                "---"
            );
        }

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
 * @brief Prints the final simulation summary.
 * @param sim Pointer to the simulation structure.
 * @return 1 on success, 0 on failure.
 * Made with the healp of AI
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

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");

    print_box_line("                       SHIFT SUMMARY");

    printf("╠══════════════════════════════════════════════════════════════╣\n");

    snprintf(line, sizeof(line), "Calls received:       %d", sim->call_count);
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

    snprintf(
        line,
        sizeof(line),
        "Average waiting:      %.2f min/call",
        average_waiting
    );
    print_box_line(line);

    snprintf(
        line,
        sizeof(line),
        "Average service:      %.2f min/call",
        average_service
    );
    print_box_line(line);

    snprintf(
        line,
        sizeof(line),
        "Simulation end:       %d min",
        sim->now
    );
    print_box_line(line);

    printf("╚══════════════════════════════════════════════════════════════╝\n");

    if (sim->log)
    {
        fprintf(sim->log, "\n=== SHIFT SUMMARY ===\n");
        fprintf(sim->log, "Calls received: %d\n", sim->call_count);
        fprintf(sim->log, "Served: %d\n", sim->served);
        fprintf(sim->log, "Cancelled: %d\n", sim->cancelled);
        fprintf(sim->log, "Unserved: %d\n", sim->unserved);
        fprintf(sim->log, "Dispatches: %d\n", sim->dispatches);
        fprintf(sim->log, "Priority escalations: %d\n", sim->escalations);
        fprintf(sim->log, "Average waiting: %.2f min/call\n", average_waiting);
        fprintf(sim->log, "Average service: %.2f min/call\n", average_service);
        fprintf(sim->log, "Simulation end: %d min\n", sim->now);
        fflush(sim->log);
    }

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

    printf(
        "Usage: %s [-c config] [-d delay_ms] "
        "[-s nearest|fastest] [-l logfile]\n",
        program
    );

    return 1;
}
