#ifndef UTILS_EXTRA_H
#define UTILS_EXTRA_H
#include "model.h"
#include <stddef.h>

#define OUT_BUF_LEN 2048// max length of one formatted output line

const char *spec_name(Specialization spec);

const char *call_state_name(CallState state);

const char *team_state_name(TeamState state);

const char *strategy_name(Strategy strategy);

int parse_spec(const char *text);

int district_index(const Simulation *sim, const char *name);

int find_call_index_by_id(const Simulation *sim, int call_id);

int find_team_index_by_id(const Simulation *sim, int team_id);

int write_all(int fd, const char *buf, size_t len);

int fd_printf(int fd, const char *format, ...) __attribute__((format(printf, 2, 3)));

int log_event(Simulation *sim, const char *format, ...) __attribute__((format(printf, 2, 3)));

int sleep_visual(const Simulation *sim);

// Sets the identification tag of the current process for log messages
void set_process_tag(const char *tag);

// Descriptor of the log file of the current process
void set_log_fd(int fd);
int get_log_fd(void);

// Locks the output semaphore to prevent mixed output of several processes
void output_begin(Simulation *sim);

// Unlocks the output semaphore after the output operation is completed
void output_end(Simulation *sim);

#endif
