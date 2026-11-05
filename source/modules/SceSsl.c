/*
 * SceSsl: certificate inspection helpers.
 *
 * TLS itself is performed by SceHttp through the host OpenSSL, so Init/Term
 * only track lifecycle. The certificate getters have no live certificate to
 * inspect; they fail cleanly.
 */

#include <stdbool.h>

#include <psp2/types.h>
#include <psp2/libssl.h>

#include "module.h"

static bool g_ssl_initialized;

EXPORT(SceSsl, 0x3C733316, int, sceSslInit, unsigned int pool_size)
{
    (void)pool_size;

    g_ssl_initialized = true;
    return 0;
}

EXPORT(SceSsl, 0x03CE6E3A, int, sceSslTerm, void)
{
    g_ssl_initialized = false;
    return 0;
}

static int ssl_unimplemented(void)
{
    return -1;
}

EXPORT_FUNC(SceSsl, 0xC73687E4, ssl_unimplemented) /* sceSslFreeSslCertName */
EXPORT_FUNC(SceSsl, 0x412711E5, ssl_unimplemented) /* sceSslGetIssuerName */
EXPORT_FUNC(SceSsl, 0xBD203262, ssl_unimplemented) /* sceSslGetMemoryPoolStats */
EXPORT_FUNC(SceSsl, 0x95E14CA6, ssl_unimplemented) /* sceSslGetNameEntryCount */
EXPORT_FUNC(SceSsl, 0x2A857867, ssl_unimplemented) /* sceSslGetNameEntryInfo */
EXPORT_FUNC(SceSsl, 0xF5ED7B68, ssl_unimplemented) /* sceSslGetNotAfter */
EXPORT_FUNC(SceSsl, 0x70DEA174, ssl_unimplemented) /* sceSslGetNotBefore */
EXPORT_FUNC(SceSsl, 0x901C5C15, ssl_unimplemented) /* sceSslGetSerialNumber */
EXPORT_FUNC(SceSsl, 0x9B2F1BC1, ssl_unimplemented) /* sceSslGetSubjectName */

DECLARE_LIBRARY(SceSsl, 0xCAF5F2E9);
