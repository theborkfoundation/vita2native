/*
 * Exercises the SceLibc printf family, with emphasis on the UTF-16 wide
 * strings Vita titles use: the host printf reads %ls as a 32-bit wchar_t
 * string and silently formats nothing, which used to turn file names built
 * with sprintf("%ls") into empty paths (Fort Defense).
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test.h"

int vita_sprintf(char *buffer, const char *format, ...);
int vita_snprintf(char *buffer, size_t size, const char *format, ...);
int vita_vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int vita_printf(const char *format, ...);
int vita_fprintf(FILE *stream, const char *format, ...);

static int check_format(char *buffer, size_t size, const char *expected, const char *format, ...)
{
    va_list args;
    int result;

    memset(buffer, 0xaa, size);
    va_start(args, format);
    result = vita_vsnprintf(buffer, size, format, args);
    va_end(args);

    if (strcmp(buffer, expected) != 0) {
        printf("libc_format_smoke: got \"%s\", expected \"%s\"\n", buffer, expected);
        return -1;
    }
    if (result != (int)strlen(expected)) {
        printf("libc_format_smoke: length %d, expected %d\n", result, (int)strlen(expected));
        return -1;
    }

    return 0;
}

int main(void)
{
    /* UTF-16 strings, little endian, as Vita titles build them. */
    static const uint16_t wide_path[] = { 't', 'd', '/', 'd', 'a', 't', 'a', '/',
                                          'l', 'o', 'a', 'd', 'i', 'n', 'g', 0 };
    static const uint16_t wide_accent[] = {
        'a',
        0x00e9,
        'b',
        0,
    }; /* "aéb" */
    char buffer[256];

    REQUIRE(vita_sprintf(buffer, "app0:/resources/%ls", wide_path) == 31, "wide path length");
    REQUIRE(strcmp(buffer, "app0:/resources/td/data/loading") == 0, "wide path text");

    REQUIRE(vita_sprintf(buffer, "%ls", wide_accent) == 4, "wide accent length");
    REQUIRE(strcmp(buffer, "a\xc3\xa9"
                           "b") == 0,
            "wide accent encoding");

    REQUIRE(vita_sprintf(buffer, "[%10ls]", wide_path) == 17, "wide width padding");
    REQUIRE(strcmp(buffer, "[td/data/loading]") == 0, "wide width text");

    REQUIRE(vita_sprintf(buffer, "%.4ls", wide_path) == 4, "wide precision truncation");
    REQUIRE(strcmp(buffer, "td/d") == 0, "wide precision text");

    /* The non-wide conversions must keep the host's behaviour. */
    REQUIRE(vita_sprintf(buffer, "%d %u %x %c %s", -42, 42u, 0xbeefu, 'Z', "str") == 17,
            "narrow conversions length");
    REQUIRE(strcmp(buffer, "-42 42 beef Z str") == 0, "narrow conversions text");

    REQUIRE(vita_sprintf(buffer, "%08.3f|%-5d|%+d|%lld", 3.14159, 7, 3, 1ll << 40) == 31,
            "flags length");
    REQUIRE(strcmp(buffer, "0003.142|7    |+3|1099511627776") == 0, "flags text");

    REQUIRE(vita_sprintf(buffer, "%%%s%%", "x") == 3, "percent escaping");
    REQUIRE(strcmp(buffer, "%x%") == 0, "percent text");

    REQUIRE(vita_sprintf(buffer, "%*d|%-*d|%.*d", 5, 12, 5, 12, 3, 7) == 15, "star arguments");
    REQUIRE(strcmp(buffer, "   12|12   |007") == 0, "star text");

    REQUIRE(vita_sprintf(buffer, "%ls", (const uint16_t *)0) == 6, "null wide string");
    REQUIRE(strcmp(buffer, "(null)") == 0, "null wide text");

    /* snprintf truncates and still reports the full length. */
    memset(buffer, 0xaa, sizeof(buffer));
    REQUIRE(vita_snprintf(buffer, 5, "%s", "truncated") == 9, "snprintf return");
    REQUIRE(strcmp(buffer, "trun") == 0, "snprintf text");

    /* The stream entry points share the formatter. */
    REQUIRE(vita_printf("libc_format_smoke: printf %ls\n", wide_path) == 42, "printf length");
    REQUIRE(vita_fprintf(stdout, "libc_format_smoke: fprintf %d\n", 7) == 29, "fprintf length");

    puts("libc_format_smoke: ok");
    return 0;
}
