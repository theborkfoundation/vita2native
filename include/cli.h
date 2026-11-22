#ifndef VITA2NATIVE_CLI_H
#define VITA2NATIVE_CLI_H

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Command-line handling.
 *
 * The parsing itself is external/argparse, driven by a declarative option table.
 * That library is not used for the error path: it calls exit() on an unknown
 * option and writes usage to stdout, neither of which suits a tool that has to
 * return its own status and keep diagnostics on stderr. cli_parse therefore
 * rejects bad arguments itself, against the same table, before argparse runs.
 */

typedef enum {
    CLI_RUN = 0, /* carry on and load a title */
    CLI_HELP,
    CLI_HELP_SETTINGS,
    CLI_VERSION,
    CLI_ERROR,
} CliResult;

typedef struct {
    /* NULL when the caller should fall back to VITA2NATIVE_EXE_FILE. */
    const char *exe_path;
    bool headless;
} CliOptions;

/*
 * Parses `argc`/`argv` into `out`, which is initialised first.
 *
 * Returns CLI_ERROR with *error pointing at a static message for an unknown
 * option, an empty argument, or more than one title. Nothing here exits the
 * process, so the whole surface is reachable from a test.
 *
 * argv is rearranged in place by the underlying parser; do not reuse it after
 * this returns.
 */
CliResult cli_parse(int argc, char *argv[], CliOptions *out, const char **error);

/* Usage text, for --help (stdout) and for a usage error (stderr). */
void cli_print_usage(FILE *stream, const char *program);

void cli_print_version(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif
