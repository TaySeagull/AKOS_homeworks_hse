#ifndef UTILS_H
#define UTILS_H
#include <stdio.h>
#include <stdlib.h>

/**
 * @def SOFT_ASSERT
 * @brief Checks a condition and prints an error message to stderr if false
 * @param condition The condition to check
 * @param message Error message to print if the condition is false
 * @param ret_val Value to return if the condition is false
 */
#define SOFT_ASSERT(cond, err, ret)                                            \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            fprintf(stderr, "Condition `%s` failed. Error: %s\n", #cond, err); \
            return (ret);                                                      \
        }                                                                      \
    } while (0)
#endif
