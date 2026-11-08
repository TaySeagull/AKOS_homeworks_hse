#ifndef CONFIG_H
#define CONFIG_H

#include "model.h"

int set_defaults(Simulation *sim);

int load_config(Simulation *sim, const char *path);

int validate_simulation(const Simulation *sim);

// func to parse an integer from a string with additional validation, returns 1 if successful, 0 otherwise
int parse_int_strict(const char *text, int *value);

#endif