/*
 * Checks for the command-line parser.
 *
 * The property that matters most here is absence of exit(): argparse calls
 * exit() on an unknown option, and cli_parse is supposed to reject those itself
 * first. If that ever regresses, this test dies instead of failing, which is a
 * loud enough signal.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "test.h"

/*
 * Builds a mutable argv the way main() receives one and parses it.
 *
 * argv has to stay writable: argparse rearranges it in place. The array is
 * static so the trailing NULL and the entries outlive the call.
 */
static CliResult parse(int count, CliOptions *out, const char **error, char **argv, ...)
{
    va_list args;
    int i;

    argv[0] = "v2n-arm";
    va_start(args, argv);
    for (i = 0; i < count; i++)
        argv[i + 1] = va_arg(args, char *);
    va_end(args);
    argv[count + 1] = NULL;

    return cli_parse(count + 1, argv, out, error);
}

#define ARGV \
    (char *[8]) { "v2n-arm", NULL, NULL, NULL, NULL, NULL, NULL, NULL }

int main(void)
{
    CliOptions out;
    const char *error = NULL;
    CliResult result;

    /* No arguments: default title, not headless. */
    memset(&out, 0xAA, sizeof(out)); /* proves cli_parse initialises it */
    CHECK(cli_parse(1, (char *[]){ "v2n-arm", NULL }, &out, &error) == CLI_RUN,
          "no args: not CLI_RUN");
    CHECK(out.exe_path == NULL, "no args: exe_path should be NULL");
    CHECK(!out.headless, "no args: should not be headless");

    /* A title, with --headless before and after it. */
    out = (CliOptions){ 0 };
    parse(1, &out, &error, ARGV, "game.vpk");
    CHECK(out.exe_path && strcmp(out.exe_path, "game.vpk") == 0, "title: wrong path '%s'",
          out.exe_path ? out.exe_path : "(null)");

    out = (CliOptions){ 0 };
    parse(2, &out, &error, ARGV, "--headless", "game.vpk");
    CHECK(out.headless, "headless first: not headless");
    CHECK(out.exe_path && strcmp(out.exe_path, "game.vpk") == 0, "headless first: wrong path");

    out = (CliOptions){ 0 };
    parse(2, &out, &error, ARGV, "game.vpk", "--headless");
    CHECK(out.headless, "headless last: not headless");
    CHECK(out.exe_path && strcmp(out.exe_path, "game.vpk") == 0, "headless last: wrong path");

    /* "--" ends option parsing, so a path may start with a dash. */
    out = (CliOptions){ 0 };
    parse(2, &out, &error, ARGV, "--", "-dashy.vpk");
    CHECK(out.exe_path && strcmp(out.exe_path, "-dashy.vpk") == 0, "separator: wrong path '%s'",
          out.exe_path ? out.exe_path : "(null)");

    /* Terminal flags. */
    result = parse(1, &out, &error, (char *[]){ "v2n-arm", "--help", NULL }, "--help");
    CHECK(result == CLI_HELP, "--help: got %d", (int)result);
    result = parse(1, &out, &error, ARGV, "-h");
    CHECK(result == CLI_HELP, "-h: got %d", (int)result);
    result = parse(1, &out, &error, ARGV, "--version");
    CHECK(result == CLI_VERSION, "--version: got %d", (int)result);
    result = parse(1, &out, &error, ARGV, "-V");
    CHECK(result == CLI_VERSION, "-V: got %d", (int)result);
    result = parse(1, &out, &error, ARGV, "--help-settings");
    CHECK(result == CLI_HELP_SETTINGS, "--help-settings: got %d", (int)result);

    /* A terminal flag wins over a bad option only when it comes first. */
    result = parse(2, &out, &error, ARGV, "--help", "--bogus");
    CHECK(result == CLI_HELP, "--help then --bogus: got %d", (int)result);

    /* The regression: unknown options used to be ignored in silence. */
    result = parse(1, &out, &error, ARGV, "--bogus");
    CHECK(result == CLI_ERROR, "--bogus: got %d (should be an error)", (int)result);
    CHECK(error && strstr(error, "--bogus"), "--bogus: message does not name it");

    result = parse(1, &out, &error, ARGV, "--headles");
    CHECK(result == CLI_ERROR, "--headles: got %d (typo accepted)", (int)result);

    result = parse(1, &out, &error, ARGV, "-x");
    CHECK(result == CLI_ERROR, "-x: got %d", (int)result);

    /* A prefix of a known option is not that option. */
    result = parse(1, &out, &error, ARGV, "--head");
    CHECK(result == CLI_ERROR, "--head: got %d (prefix matched)", (int)result);

    /* Two titles used to mean "the last one wins". */
    result = parse(2, &out, &error, ARGV, "a.vpk", "b.vpk");
    CHECK(result == CLI_ERROR, "two titles: got %d", (int)result);
    CHECK(error && strstr(error, "more than one"), "two titles: unclear message");

    /* Neither a value on a switch nor an empty argument is a title. */
    result = parse(1, &out, &error, ARGV, "--headless=x");
    CHECK(result == CLI_ERROR, "--headless=x: got %d", (int)result);

    result = parse(1, &out, &error, ARGV, "");
    CHECK(result == CLI_ERROR, "empty argument: got %d", (int)result);

    /* Output helpers must not crash on either stream. */
    cli_print_usage(stdout, "v2n-arm");
    cli_print_version(stdout);

    return test_status("cli_smoke");
}
