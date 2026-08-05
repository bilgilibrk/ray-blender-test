#include <stdio.h>
#include <string.h>

#include "raylib.h"

#include "tests.h"

int g_failures = 0;
int g_checks = 0;
int g_verbose = 0;

int main(int argc, char **argv)
{
    // The engine logs a lot at INFO; tests only care about problems.
    SetTraceLogLevel(LOG_WARNING);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) { SetTraceLogLevel(LOG_INFO); g_verbose = 1; }
    }

    printf("json\n");      RunJsonTests();
    printf("spline\n");    RunSplineTests();
    printf("collision\n"); RunCollisionTests();
    printf("light\n");     RunLightTests();
    printf("race\n");      RunRaceTests();

    printf("\n%d checks, %d failures — %s\n", g_checks, g_failures,
           g_failures ? "FAILED" : "ok");
    return g_failures ? 1 : 0;
}
