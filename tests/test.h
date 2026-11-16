#ifndef VITA2NATIVE_TESTS_TEST_H
#define VITA2NATIVE_TESTS_TEST_H

/*
 * Shared harness for the smoke tests.
 *
 * Every smoke test is a standalone program built from a few module sources, so
 * a header is the whole harness. Two styles are supported because they mean
 * different things and both are in use:
 *
 *   CHECK(cond[, format, ...])    record the failure and carry on. For checks
 *                                 that are independent of each other; the test
 *                                 returns test_status() at the end.
 *   REQUIRE(cond[, format, ...])  report and return 1 from the current
 *                                 function. For prerequisites that the rest of
 *                                 the test would crash without.
 *
 * Both write to `failures`, which tests may also add to directly when a helper
 * function reports more than one failure (e.g. `failures += check_export(...)`).
 */

#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

/*
 * Removes a directory tree. Tests that write to a temporary root call this on
 * the way out, so a rerun starts from nothing: a leftover save slot or database
 * makes the next run fail in a way that looks like a product bug.
 */
__attribute__((unused)) static void test_remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (!directory) {
        unlink(path);
        return;
    }

    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat status;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (lstat(child, &status) == 0 && S_ISDIR(status.st_mode))
            test_remove_tree(child);
        else
            unlink(child);
    }

    closedir(directory);
    rmdir(path);
}

/* Returns 1 when the check failed, so REQUIRE can propagate it. */
static int test_check(const char *file, int line, const char *expression,
                      const char *format, ...)
{
    va_list args;

    if (format && format[0]) {
        fprintf(stderr, "%s:%d: check failed: %s (", file, line, expression);
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fputs(")\n", stderr);
    } else {
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    }
    failures++;
    return 1;
}

#define TEST_CHECK_(cond) \
    ((cond) ? 0 : test_check(__FILE__, __LINE__, #cond, NULL))
#define TEST_CHECK_ARGS_(cond, ...) \
    ((cond) ? 0 : test_check(__FILE__, __LINE__, #cond, __VA_ARGS__))

/* Pick the bare form for CHECK(cond) and the format form otherwise. The pad is
   all-format-args followed by one bare entry, so the name lands correctly for
   every arity up to 10 arguments. */
#define TEST_PICK_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, NAME, ...) NAME
#define TEST_PAD_ \
    TEST_CHECK_ARGS_, TEST_CHECK_ARGS_, TEST_CHECK_ARGS_, TEST_CHECK_ARGS_, TEST_CHECK_ARGS_, \
    TEST_CHECK_ARGS_, TEST_CHECK_ARGS_, TEST_CHECK_ARGS_, TEST_CHECK_ARGS_, TEST_CHECK_
/* The extra layer matters: the pad has to be expanded before TEST_PICK_
   splits its argument list. */
#define TEST_SELECT_(...) TEST_PICK_(__VA_ARGS__)

#define CHECK(...) \
    (void)TEST_SELECT_(__VA_ARGS__, TEST_PAD_)(__VA_ARGS__)

#define REQUIRE(...) \
    do { \
        if (TEST_SELECT_(__VA_ARGS__, TEST_PAD_)(__VA_ARGS__)) \
            return 1; \
    } while (0)

/* Result for the accumulate style: 0 when every check passed. */
static int test_status(const char *name)
{
    if (failures) {
        fprintf(stderr, "%s: %d check(s) failed\n", name, failures);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#endif
