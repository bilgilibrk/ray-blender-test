// Shared harness for the engine and gameplay tests.
#ifndef TESTS_H
#define TESTS_H

#include <stdio.h>

extern int g_failures;
extern int g_checks;

#define CHECK(cond, ...) do {                       \
    g_checks++;                                     \
    if (!(cond)) {                                  \
        g_failures++;                               \
        printf("  FAIL: ");                         \
        printf(__VA_ARGS__);                        \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

extern int g_verbose;

void RunJsonTests(void);
void RunSplineTests(void);
void RunCollisionTests(void);
void RunRaceTests(void);

#endif // TESTS_H
