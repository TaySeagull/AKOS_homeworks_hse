#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "ipc.h"
#include "net.h"
#include "signals.h"
#include "utils.h"
#include "utils_extra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WAIT 30    // seconds to wait for a server that is not started yet
#define LOG_LINES_MAX 40   // lines of the log that the screen can keep
#define LOG_LINES_DEF 12   // lines of the log that are shown by default
#define LINE_WIDTH 92      // width of one line of the log in the box
#define REDRAW_EVERY_MS 60 // the screen is not redrawn more often than this

static char log_lines[LOG_LINES_MAX][LINE_WIDTH]; // ring buffer of the last lines
static int log_count = 0;                         // lines in the ring buffer
static int log_first = 0;                         // oldest line of the ring buffer
static int log_shown = LOG_LINES_DEF;

/**
 * @brief Prints the usage of the observer
 * @param program The name of the program
 */
static void usage(const char *program)
{
    fd_printf(STDOUT_FILENO,
              "Usage: %s [-H host] [-p port] [-w seconds] [-n lines] [-P] [-h]\n"
              "  -H host    address or name of the server (default 127.0.0.1)\n"
              "  -p port    TCP port of the server (default %d)\n"
              "  -w sec     how long to wait for a server that is not started yet (default %d)\n"
              "  -n lines   how many lines of the log to show (1..%d, default %d)\n"
              "  -P         plain mode: no full screen, one line per event (for logs and pipes)\n"
              "  -h         show this help\n"
              "This program only watches: it shows the behaviour of the whole application\n"
              "(model time, teams, calls, statistics and the events of every program).\n"
              "It can be started and stopped at any moment and it changes nothing in the\n"
              "simulation; several observers may watch the same server at the same time.\n",
              program, NET_DEFAULT_PORT, DEFAULT_WAIT, LOG_LINES_MAX, LOG_LINES_DEF);
}

/**
 * @brief Puts one line of the log into the ring buffer
 * @param line The line
 */
static void remember_line(const char *line)
{
    int slot = (log_first + log_count) % LOG_LINES_MAX;

    if (log_count == LOG_LINES_MAX)
    {
        slot = log_first;
        log_first = (log_first + 1) % LOG_LINES_MAX;
    }
    else
        log_count++;

    snprintf(log_lines[slot], LINE_WIDTH, "%s", line);
}

/**
 * @brief Text of the reason why the shift has stopped
 * @param reason The reason (StopReason)
 * @return Constant string
 */
static const char *reason_text(int reason)
{
    switch (reason)
    {
    case STOP_ALL_PROCESSED:
        return "all calls processed";
    case STOP_SHIFT_LIMIT:
        return "shift limit reached";
    case STOP_INTERRUPTED:
        return "interrupted by the user";
    case STOP_INTAKE_CLOSED:
        return "intake closed, active calls finished";
    case STOP_PROCESS_FAILURE:
        return "a client was lost";
    default:
        return "unknown";
    }
}

/**
 * @brief 1 if the call is interesting for the observer right now
 * @param call The call
 * @return 1 or 0
 */
static int call_is_active(const Call *call)
{
    return call->state == CALL_WAITING || call->state == CALL_EN_ROUTE || call->state == CALL_IN_SERVICE;
}

/**
 * @brief Draws the whole screen of the observer
 * @param state The state of the application
 * @param where Address of the server
 */
static void draw(const NetState *state, const char *where)
{
    fd_printf(STDOUT_FILENO, "\033[H\033[2J"); // cursor home, clear the screen

    fd_printf(STDOUT_FILENO, "+============================================================================+\n");
    fd_printf(STDOUT_FILENO, "| EMERGENCY DISPATCH - OBSERVER  server %-22s   time [%02d:%02d] |\n", where,
              state->now / 60, state->now % 60);
    fd_printf(STDOUT_FILENO, "+============================================================================+\n");

    fd_printf(STDOUT_FILENO, " strategy %-7s | shift %-4d min | intake %-6s | delay %-4d ms\n",
              strategy_name((Strategy)state->strategy), state->shift_limit, state->intake_closed ? "CLOSED" : "OPEN",
              state->delay_ms);

    fd_printf(STDOUT_FILENO, " programs: server + %d client(s) of %d roles | observers: %d | events in queue: %d\n",
              state->clients_online, 2 + state->team_count, state->observers_online, state->event_count);

    const Counters *counters = &state->counters;

    fd_printf(STDOUT_FILENO,
              " received %-3d  served %-3d  cancelled %-3d  unserved %-3d  dispatches %-3d  escalations %-3d\n",
              counters->received, counters->served, counters->cancelled, counters->unserved, counters->dispatches,
              counters->escalations);

    fd_printf(STDOUT_FILENO, "+-------------------------------- TEAMS -------------------------------------+\n");

    for (int idx = 0; idx < state->team_count; idx++)
    {
        const Team *team = &state->teams[idx];
        char call_text[16];

        if (team->call_id != NO_CALL)
            snprintf(call_text, sizeof(call_text), "#%03d", team->call_id);
        else
            snprintf(call_text, sizeof(call_text), "---");

        fd_printf(STDOUT_FILENO, " B%-2d %-7.7s %-10.10s district %-8.8s call %-5.5s %s\n", team->id,
                  spec_name(team->spec), team_state_name(team->state),
                  team->district >= 0 && team->district < state->district_count
                      ? state->districts[team->district].name
                      : "?",
                  call_text, team->started ? "" : "(client off)");
    }

    fd_printf(STDOUT_FILENO, "+---------------------------- ACTIVE CALLS ----------------------------------+\n");

    int shown = 0;

    for (int idx = 0; idx < state->call_count && shown < 10; idx++)
    {
        const Call *call = &state->calls[idx];

        if (!call_is_active(call))
            continue;

        char team_text[16];

        if (call->assigned_team != NO_TEAM)
            snprintf(team_text, sizeof(team_text), "B%d", call->assigned_team);
        else
            snprintf(team_text, sizeof(team_text), "-");

        fd_printf(STDOUT_FILENO, " #%03d %-8.8s p%d %-7.7s %-11.11s team %-4.4s waiting %d min\n", call->id,
                  call->district >= 0 && call->district < state->district_count
                      ? state->districts[call->district].name
                      : "?",
                  call->priority, spec_name((Specialization)call->spec), call_state_name(call->state), team_text,
                  state->now - call->arrival);
        shown++;
    }

    if (shown == 0)
        fd_printf(STDOUT_FILENO, " (no call is waiting or being served right now)\n");

    fd_printf(STDOUT_FILENO, "+------------------------------ LAST EVENTS ---------------------------------+\n");

    int first = log_count > log_shown ? log_count - log_shown : 0;

    for (int idx = first; idx < log_count; idx++)
        fd_printf(STDOUT_FILENO, " %s\n", log_lines[(log_first + idx) % LOG_LINES_MAX]);

    if (state->finished)
    {
        double wait = counters->served > 0 ? (double)counters->total_wait / counters->served : 0.0;
        double service = counters->served > 0 ? (double)counters->total_service / counters->served : 0.0;

        fd_printf(STDOUT_FILENO, "+================================ SHIFT OVER ================================+\n");
        fd_printf(STDOUT_FILENO, " stop reason: %s | end of the model time: %d min\n", reason_text(state->stop_reason),
                  state->now);
        fd_printf(STDOUT_FILENO, " average waiting %.2f min/call | average service %.2f min/call | invariants %s\n",
                  wait, service, state->invariant_errors == 0 ? "ok" : "VIOLATED");
    }

    fd_printf(STDOUT_FILENO, "+============================================================================+\n");
}

/**
 * @brief Prints one line about the state (plain mode)
 * @param state The state of the application
 */
static void digest(const NetState *state)
{
    int waiting = 0;
    int active = 0;

    for (int idx = 0; idx < state->call_count; idx++)
    {
        if (state->calls[idx].state == CALL_WAITING)
            waiting++;
        else if (state->calls[idx].state == CALL_EN_ROUTE || state->calls[idx].state == CALL_IN_SERVICE)
            active++;
    }

    const Counters *counters = &state->counters;

    fd_printf(STDOUT_FILENO,
              "[%02d:%02d] STATE waiting=%d active=%d received=%d served=%d cancelled=%d unserved=%d "
              "dispatches=%d events=%d clients=%d observers=%d\n",
              state->now / 60, state->now % 60, waiting, active, counters->received, counters->served,
              counters->cancelled, counters->unserved, counters->dispatches, state->event_count,
              state->clients_online, state->observers_online);

    if (!state->finished)
        return;

    double wait = counters->served > 0 ? (double)counters->total_wait / counters->served : 0.0;
    double service = counters->served > 0 ? (double)counters->total_service / counters->served : 0.0;

    fd_printf(STDOUT_FILENO,
              "FINAL stop_reason=%s end=%d min received=%d served=%d cancelled=%d unserved=%d "
              "escalations=%d dispatches=%d average_wait=%.2f average_service=%.2f invariant_errors=%d\n",
              reason_text(state->stop_reason), state->now, counters->received, counters->served, counters->cancelled,
              counters->unserved, counters->escalations, counters->dispatches, wait, service,
              state->invariant_errors);
}

/**
 * @brief Milliseconds of the monotonic clock
 * @return The time
 */
static long now_ms(void)
{
    struct timespec when;
    clock_gettime(CLOCK_MONOTONIC, &when);

    return when.tv_sec * 1000L + when.tv_nsec / 1000000L;
}

/**
 * @brief Entry point of the observer
 * @param argc Number of arguments
 * @param argv Arguments
 * @return 0 when the server has finished, 1 on a wrong start
 */
int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = NET_DEFAULT_PORT;
    int wait_seconds = DEFAULT_WAIT;
    int plain = 0;
    int number = 0;
    int option = 0;

    while ((option = getopt(argc, argv, "H:p:w:n:Ph")) != -1)
    {
        switch (option)
        {
        case 'H':
            if (strlen(optarg) == 0 || strlen(optarg) >= NET_HOST_LEN)
            {
                fprintf(stderr, "Invalid host '%s'\n", optarg);
                return 1;
            }
            host = optarg;
            break;

        case 'p':
            if (!parse_int_strict(optarg, &number) || number < 1 || number > 65535)
            {
                fprintf(stderr, "Invalid port '%s': a number between 1 and 65535 is expected\n", optarg);
                return 1;
            }
            port = number;
            break;

        case 'w':
            if (!parse_int_strict(optarg, &wait_seconds) || wait_seconds < 0)
            {
                fprintf(stderr, "Invalid waiting time '%s'\n", optarg);
                return 1;
            }
            break;

        case 'n':
            if (!parse_int_strict(optarg, &number) || number < 1 || number > LOG_LINES_MAX)
            {
                fprintf(stderr, "Invalid number of lines '%s' (1..%d)\n", optarg, LOG_LINES_MAX);
                return 1;
            }
            log_shown = number;
            break;

        case 'P':
            plain = 1;
            break;

        case 'h':
            usage(argv[0]);
            return 0;

        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind < argc)
    {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return 1;
    }

    // Ctrl+C only stops this observer: the simulation goes on without it
    signals_install_server();

    if (!ipc_observer_attach(host, port, wait_seconds))
        return 1;

    char where[NET_ADDR_LEN];
    snprintf(where, sizeof(where), "%s:%d", host, port);

    fd_printf(STDOUT_FILENO, "[observer] pid %d watches the server %s\n", (int)getpid(), where);

    static NetState state;
    memset(&state, 0, sizeof(state));

    char line[OUT_BUF_LEN];
    long last_draw = 0;
    int have_state = 0;
    int finished = 0;

    for (;;)
    {
        int kind = ipc_observer_next(&state, line, sizeof(line));

        if (kind == -2) // a signal: this observer leaves, the application goes on
        {
            if (signals_stop_requested())
                break;

            continue;
        }

        if (kind <= 0)
        {
            if (!plain)
                fd_printf(STDOUT_FILENO, "\n");

            fd_printf(STDOUT_FILENO, "[observer] the server has closed the connection%s\n",
                      finished ? " (the shift is over)" : " (it was stopped)");
            break;
        }

        if (kind == NET_LOG)
        {
            remember_line(line);

            if (plain)
            {
                fd_printf(STDOUT_FILENO, "%s\n", line);
                continue;
            }
        }

        if (kind == NET_STATE)
        {
            have_state = 1;
            finished = state.finished;

            if (plain)
            {
                digest(&state);
                continue;
            }
        }

        // the full screen is not redrawn for every single line
        if (!plain && have_state && (kind == NET_STATE || now_ms() - last_draw >= REDRAW_EVERY_MS))
        {
            draw(&state, where);
            last_draw = now_ms();
        }
    }

    ipc_observer_detach();

    return 0;
}
