/*
 * Command-line handling, on top of external/argparse.
 *
 * argparse is given a declarative table and does the parsing. It is not given
 * the error path: upstream calls exit() on an unknown option and prints usage
 * to stdout, while this tool needs its own exit status and its diagnostics on
 * stderr. So the arguments are validated against the same table first, and
 * argparse only ever sees input that will parse cleanly.
 */
#include <stdio.h>
#include <string.h>

#include "argparse.h"

#include "cli.h"
#include "version.h"

typedef struct {
    int help;
    int help_settings;
    int version;
    int headless;
} CliFlags;

/*
 * True when `arg` names `option`, as "--long", "--long=value" or "-s".
 * On a match, *consumed is how many characters the option itself took.
 */
static bool option_matches(const struct argparse_option *option, const char *arg, size_t *consumed)
{
    if (arg[0] == '-' && arg[1] == '-') {
        const char *name = arg + 2;
        size_t length;

        if (!option->long_name)
            return false;
        length = strlen(option->long_name);
        if (strncmp(name, option->long_name, length) != 0)
            return false;
        /* Must end there or be followed by '=': "--headlessx" is not ours. */
        if (name[length] != '\0' && name[length] != '=')
            return false;
        *consumed = 2 + length;
        return true;
    }

    if (arg[0] == '-' && arg[1] != '\0' && arg[1] != '-') {
        if (option->short_name && option->short_name == arg[1]) {
            *consumed = 2;
            return true;
        }
    }

    return false;
}

/*
 * Rejects what argparse would get wrong: an unknown option (it exits), an empty
 * argument (it takes one as a path), a value on a switch (it exits), or a second
 * title (silently ignored). Returns the offending index, or -1.
 *
 * `--help`/`--version`/`--help-settings` stop the scan: they are answered
 * before anything else runs, so `--help --bogus` must still show help.
 *
 * `*titles` counts the positional arguments.
 */
static int validate(int argc, char *argv[], const struct argparse_option *options,
                    const char **problem, int *titles, CliResult *terminal)
{
    bool after_separator = false;

    *titles = 0;
    *terminal = CLI_RUN;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!after_separator && strcmp(arg, "--") == 0) {
            after_separator = true;
            continue;
        }

        if (after_separator) {
            (*titles)++;
            continue;
        }

        if (arg[0] == '\0') {
            *problem = "empty argument";
            return i;
        }

        if (arg[0] != '-')
            goto positional;

        {
            const struct argparse_option *option = options;

            for (; option->type != ARGPARSE_OPT_END; option++) {
                size_t consumed = 0;

                if (!option_matches(option, arg, &consumed))
                    continue;
                if (option->type == ARGPARSE_OPT_BOOLEAN && arg[consumed] == '=') {
                    *problem = "option takes no value:";
                    return i;
                }
                if (option->long_name && strcmp(option->long_name, "help") == 0) {
                    *terminal = CLI_HELP;
                    return -1;
                }
                if (option->long_name && strcmp(option->long_name, "help-settings") == 0) {
                    *terminal = CLI_HELP_SETTINGS;
                    return -1;
                }
                if (option->long_name && strcmp(option->long_name, "version") == 0) {
                    *terminal = CLI_VERSION;
                    return -1;
                }
                goto next_argument;
            }
        }

        *problem = "unknown option:";
        return i;

positional:
        (*titles)++;
next_argument:
        continue;
    }

    return -1;
}

CliResult cli_parse(int argc, char *argv[], CliOptions *out, const char **error)
{
    static const char *const usages[] = {
        "v2n-arm [--headless] [title.vpk|eboot.bin]",
        NULL,
    };
    CliFlags flags = { 0 };
    struct argparse_option options[] = {
        OPT_BOOLEAN('h', "help", &flags.help, "show this message and exit", NULL, 0, 0),
        OPT_BOOLEAN(0, "help-settings", &flags.help_settings,
                    "list every VITA2NATIVE_* setting and exit", NULL, 0, 0),
        OPT_BOOLEAN('V', "version", &flags.version, "print the version and exit", NULL, 0, 0),
        OPT_BOOLEAN(0, "headless", &flags.headless, "run without a window", NULL, 0, 0),
        OPT_END(),
    };
    static char message[160];
    struct argparse parser;
    const char *problem = NULL;
    CliResult terminal = CLI_RUN;
    int titles = 0;
    int bad;
    int leftover;

    out->exe_path = NULL;
    out->headless = false;
    if (error)
        *error = NULL;

    bad = validate(argc, argv, options, &problem, &titles, &terminal);
    if (bad >= 0) {
        /* The offending argument is appended unless the message already says
           everything (it is empty, so quoting it would add only whitespace). */
        if (argv[bad][0] == '\0')
            snprintf(message, sizeof(message), "%s", problem);
        else
            snprintf(message, sizeof(message), "%s %s", problem, argv[bad]);
        if (error)
            *error = message;
        return CLI_ERROR;
    }

    /*
     * A terminal flag was seen before anything invalid, so answer it without
     * asking argparse to look at the rest: upstream would still exit() on a bad
     * option that came after --help.
     */
    if (terminal != CLI_RUN)
        return terminal;

    if (titles > 1) {
        if (error)
            *error = "more than one title given";
        return CLI_ERROR;
    }

    argparse_init(&parser, options, usages, 0);
    argparse_describe(&parser, "Run a PlayStation Vita title natively on 32-bit ARM Linux.",
                      "VITA2NATIVE_CLI_ENV_DUMP=1 prints every setting with its value.");
    leftover = argparse_parse(&parser, argc, (const char **)argv);

    /* A terminal flag wins over everything else, help first. */
    if (flags.help)
        return CLI_HELP;
    if (flags.help_settings)
        return CLI_HELP_SETTINGS;
    if (flags.version)
        return CLI_VERSION;

    out->headless = flags.headless != 0;
    /*
     * argparse moves the non-option arguments to the front of argv, so the
     * title - if any - is argv[0] now. argv[0] no longer names the program.
     */
    if (leftover > 0)
        out->exe_path = argv[0];

    return CLI_RUN;
}

void cli_print_usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [--headless] [title.vpk|eboot.bin]\n",
            program ? program : "v2n-arm");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help            show this message and exit\n");
    fprintf(stream, "      --help-settings   list every VITA2NATIVE_* setting and exit\n");
    fprintf(stream, "  -V, --version         print the version and exit\n");
    fprintf(stream, "      --headless        run without a window\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no title, <storage>/executable is loaded from the working\n");
    fprintf(stream, "directory. Settings are environment variables; VITA2NATIVE_CLI_ENV_DUMP=1\n");
    fprintf(stream, "lists them with their current values.\n");
    fflush(stream);
}

void cli_print_version(FILE *stream)
{
    fprintf(stream, "%s\n", VITA2NATIVE_VERSION_STRING);
    fflush(stream);
}
