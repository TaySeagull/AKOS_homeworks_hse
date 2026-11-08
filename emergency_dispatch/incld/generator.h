/**
 * @file generator.h
 * @brief Random call generator for the simulation
 */

#ifndef GENERATOR_H
#define GENERATOR_H
#include "model.h"

int create_random_call(Simulation *sim);

int schedule_next_random_call(Simulation *sim);

#endif