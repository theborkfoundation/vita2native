#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/gxm.h>

#include "spirv_cross_glsl_es.h"
#include "vita3k_shader_recompiler_iface_c.h"

/* Logging shims required by Vita3K's shader sources. */
void log_print(const char *str)
{
    fprintf(stderr, "%s\n", str);
}

void log_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

void fatal_error(const char *message, const char *detail)
{
    fprintf(stderr, "fatal: %s: %s\n", message, detail);
    abort();
}

static void *read_file(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    long length;
    void *data;

    if (!fp)
        return NULL;

    fseek(fp, 0, SEEK_END);
    length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (length <= 0) {
        fclose(fp);
        return NULL;
    }

    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    uint32_t *spirv = NULL;
    uint32_t num_instr = 0;
    char *glsl = NULL;
    void *gxp;
    size_t gxp_size;
    int ok;

    if (argc < 2) {
        fprintf(stderr, "usage: %s shader.gxp\n", argv[0]);
        return 1;
    }

    gxp = read_file(argv[1], &gxp_size);
    if (!gxp) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    ok = convert_gxp_to_spirv_c(&spirv, &num_instr, (const SceGxmProgram *)gxp, argv[1], false,
                                false, false, false, NULL, 0, false, false, NULL);
    free(gxp);

    if (!ok) {
        fprintf(stderr, "GXP -> SPIR-V failed\n");
        return 1;
    }

    printf("SPIR-V words: %u\n", num_instr);

    if (spirv_to_glsl_es(spirv, num_instr, false, false, -1, NULL, &glsl) != 0) {
        fprintf(stderr, "SPIR-V -> GLSL ES failed\n");
        free(spirv);
        return 1;
    }

    free(spirv);

    if (!glsl || strstr(glsl, "#version 310 es") == NULL) {
        fprintf(stderr, "unexpected GLSL ES output:\n%s\n", glsl ? glsl : "(null)");
        free(glsl);
        return 1;
    }

    printf("GLSL ES length: %zu\n", strlen(glsl));
    if (argc > 2 && strcmp(argv[2], "--dump") == 0)
        printf("%s\n", glsl);
    printf("shader_smoke: ok\n");

    free(glsl);
    return 0;
}
