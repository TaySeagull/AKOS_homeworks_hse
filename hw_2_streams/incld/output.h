#ifndef OUTPUT_H
#define OUTPUT_H
#include "model.h"

int print_header(const Simulation *sim);

int show_snapshot(const Simulation *sim);

int print_summary(const Simulation *sim);

int print_usage(const char *program);

#endif
