#ifndef CONFIG_H
#define CONFIG_H
#include "model.h"

int set_defaults(Simulation *sim);

int load_config(Simulation *sim, const char *path);

int validate_simulation(const Simulation *sim);

#endif
