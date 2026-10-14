/*
 * main.c -- the test runner.
 *
 * Runs every suite, keeps going after a failure and prints a summary.
 * Pass suite names as arguments to run only those.
 */
#include <stdarg.h>
#include <string.h>

#include "test.h"

static int total_run;
static int total_failed;
static const char *current_case;

void test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list args;

    total_failed++;
    printf("\n  FAIL %s\n       %s:%d: ", current_case ? current_case : "?",
           file, line);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

int test_run_suite(const TestCase *cases)
{
    int failures_before = total_failed;

    for (const TestCase *c = cases; c->fn; c++) {
        char label[160];
        int before = total_failed;

        total_run++;
        snprintf(label, sizeof(label), "%s.%s", c->suite, c->name);
        current_case = label;

        c->fn();

        printf("  %-4s %s\n", total_failed == before ? "ok" : "FAIL", label);
    }

    current_case = NULL;
    return total_failed - failures_before;
}

extern const TestCase cpu_tests[];
extern const TestCase timing_tests[];
extern const TestCase memory_tests[];
extern const TestCase cart_tests[];
extern const TestCase timer_tests[];
extern const TestCase ppu_tests[];
extern const TestCase apu_tests[];
extern const TestCase input_tests[];
extern const TestCase state_tests[];
extern const TestCase layout_tests[];
extern const TestCase platform_tests[];
extern const TestCase integration_tests[];

static const struct {
    const char *name;
    const TestCase *cases;
} suites[] = {
    { "cpu", cpu_tests },
    { "timing", timing_tests },
    { "memory", memory_tests },
    { "cart", cart_tests },
    { "timer", timer_tests },
    { "ppu", ppu_tests },
    { "apu", apu_tests },
    { "input", input_tests },
    { "state", state_tests },
    { "platform", platform_tests },
    { "layout", layout_tests },
    { "integration", integration_tests }
};

int main(int argc, char **argv)
{
    bool filter[sizeof(suites) / sizeof(suites[0])];

    memset(filter, 0, sizeof(filter));

    if (argc <= 1) {
        for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++)
            filter[i] = true;
    } else {
        for (int i = 1; i < argc; i++) {
            bool found = false;
            for (size_t s = 0; s < sizeof(suites) / sizeof(suites[0]); s++) {
                if (strcmp(suites[s].name, argv[i]) == 0) {
                    filter[s] = true;
                    found = true;
                }
            }
            if (!found) {
                printf("unknown suite '%s'\n", argv[i]);
                return 2;
            }
        }
    }

    for (size_t s = 0; s < sizeof(suites) / sizeof(suites[0]); s++) {
        if (!filter[s])
            continue;
        printf("\n== %s ==\n", suites[s].name);
        test_run_suite(suites[s].cases);
    }

    printf("\n----------------------------------------\n");
    printf("%d tests, %d failed\n", total_run, total_failed);
    return total_failed == 0 ? 0 : 1;
}