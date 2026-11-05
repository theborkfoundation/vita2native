/*
 * ScePspnetAdhoc: local ad-hoc networking.
 *
 * There is no ad-hoc radio on the host. Initialisation succeeds and queries
 * report an empty network so titles can reach their single-player fallback
 * without hanging; every socket operation returns the firmware's
 * "not connected" error.
 */
#include <stdbool.h>
#include <string.h>

#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>

#include "module.h"

#define ADHOC_NOT_CONNECTED ((int)SCE_ERROR_NET_ADHOC_NOT_CONNECTED)
#define ADHOC_INVALID_SOCKET ((int)SCE_ERROR_NET_ADHOC_INVALID_SOCKET_ID)
#define ADHOC_INVALID_ARG ((int)SCE_ERROR_NET_ADHOC_INVALID_ARG)

EXPORT(ScePspnetAdhoc, 0x87805E13, int, sceNetAdhocInit, void)
{
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x6C0B4971, int, sceNetAdhocTerm, void)
{
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x54ED636D, int, sceNetAdhocPollSocket, SceNetAdhocPollSd *sds, int nsds,
       unsigned int timeout, int flag)
{
    (void)sds;
    (void)nsds;
    (void)timeout;
    (void)flag;
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x31F45B51, int, sceNetAdhocSetSocketAlert, int id, int flag)
{
    (void)id;
    (void)flag;
    return ADHOC_INVALID_SOCKET;
}

EXPORT(ScePspnetAdhoc, 0xCA249F53, int, sceNetAdhocGetSocketAlert, int id, int *flag)
{
    (void)id;
    if (flag)
        *flag = 0;
    return ADHOC_INVALID_SOCKET;
}

EXPORT(ScePspnetAdhoc, 0xF2EC12CD, int, sceNetAdhocPdpCreate, const SceNetEtherAddr *saddr,
       SceUShort16 sport, unsigned int bufsize, int flag)
{
    (void)saddr;
    (void)sport;
    (void)bufsize;
    (void)flag;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0xB56238BA, int, sceNetAdhocPdpSend, int id, const SceNetEtherAddr *daddr,
       SceUShort16 dport, const void *data, int len, unsigned int timeout, int flag)
{
    (void)id;
    (void)daddr;
    (void)dport;
    (void)data;
    (void)len;
    (void)timeout;
    (void)flag;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0x1172398C, int, sceNetAdhocPdpRecv, int id, SceNetEtherAddr *saddr,
       SceUShort16 *sport, void *buf, int *len, unsigned int timeout, int flag)
{
    (void)id;
    (void)saddr;
    (void)sport;
    (void)buf;
    (void)timeout;
    (void)flag;
    if (len)
        *len = 0;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0xB7B6A662, int, sceNetAdhocPdpDelete, int id, int flag)
{
    (void)id;
    (void)flag;
    return ADHOC_INVALID_SOCKET;
}

EXPORT(ScePspnetAdhoc, 0x02098B69, int, sceNetAdhocGetPdpStat, int *buflen, void *buf)
{
    (void)buf;
    if (!buflen)
        return ADHOC_INVALID_ARG;
    *buflen = 0;
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x81498625, int, sceNetAdhocPtpOpen, const SceNetEtherAddr *saddr,
       SceUShort16 sport, const SceNetEtherAddr *daddr, SceUShort16 dport, unsigned int bufsize,
       unsigned int rexmt_int, int rexmt_cnt, int flag)
{
    (void)saddr;
    (void)sport;
    (void)daddr;
    (void)dport;
    (void)bufsize;
    (void)rexmt_int;
    (void)rexmt_cnt;
    (void)flag;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0x632EDB11, int, sceNetAdhocPtpConnect, int id, unsigned int timeout, int flag)
{
    (void)id;
    (void)timeout;
    (void)flag;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0x8F683810, int, sceNetAdhocPtpListen, const SceNetEtherAddr *saddr,
       SceUShort16 sport, unsigned int bufsize, unsigned int rexmt_int, int rexmt_cnt, int backlog,
       int flag)
{
    (void)saddr;
    (void)sport;
    (void)bufsize;
    (void)rexmt_int;
    (void)rexmt_cnt;
    (void)backlog;
    (void)flag;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0x0A0423DD, int, sceNetAdhocPtpAccept, int id, SceNetEtherAddr *addr,
       SceUShort16 *port, unsigned int timeout, int flag)
{
    (void)id;
    (void)addr;
    (void)port;
    (void)timeout;
    (void)flag;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0x0A4E32EF, int, sceNetAdhocPtpSend, int id, const void *data, int *len,
       unsigned int timeout, int flag)
{
    (void)id;
    (void)data;
    (void)timeout;
    (void)flag;
    if (len)
        *len = 0;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0x0291EF5B, int, sceNetAdhocPtpRecv, int id, void *buf, int *len,
       unsigned int timeout, int flag)
{
    (void)id;
    (void)buf;
    (void)timeout;
    (void)flag;
    if (len)
        *len = 0;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0xAD5F570D, int, sceNetAdhocPtpFlush, int id, unsigned int timeout, int flag)
{
    (void)id;
    (void)timeout;
    (void)flag;
    return ADHOC_NOT_CONNECTED;
}

EXPORT(ScePspnetAdhoc, 0xA083BF7C, int, sceNetAdhocPtpClose, int id, int flag)
{
    (void)id;
    (void)flag;
    return ADHOC_INVALID_SOCKET;
}

EXPORT(ScePspnetAdhoc, 0xE45F0801, int, sceNetAdhocGetPtpStat, int *buflen, void *buf)
{
    (void)buf;
    if (!buflen)
        return ADHOC_INVALID_ARG;
    *buflen = 0;
    return 0;
}

/* --------------------------------------------------------------- adhoc ctl */

EXPORT(ScePspnetAdhoc, 0xE2FE9DD6, int, sceNetAdhocctlInit,
       const struct SceNetAdhocctlAdhocId *adhocId)
{
    (void)adhocId;
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x97A80065, int, sceNetAdhocctlTerm, void)
{
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x49AF74AE, int, sceNetAdhocctlGetAdhocId,
       struct SceNetAdhocctlAdhocId *adhocId)
{
    if (adhocId)
        memset(adhocId, 0, sizeof(*adhocId));
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x356AA25E, int, sceNetAdhocctlGetPeerList, int *buflen, void *buf)
{
    (void)buf;
    if (!buflen)
        return 0;
    *buflen = 0;
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x14552BB1, int, sceNetAdhocctlGetPeerInfo,
       const struct SceNetEtherAddr *addr, int size,
       struct SceNetAdhocctlPeerInfo *peerInfo)
{
    (void)addr;
    (void)size;
    (void)peerInfo;
    return -1;
}

EXPORT(ScePspnetAdhoc, 0xD3230114, int, sceNetAdhocctlGetAddrByName,
       const struct SceNetAdhocctlNickname *nickname, int *buflen, void *buf)
{
    (void)nickname;
    (void)buf;
    if (buflen)
        *buflen = 0;
    return 0;
}

EXPORT(ScePspnetAdhoc, 0xFCA965F6, int, sceNetAdhocctlGetNameByAddr,
       const struct SceNetEtherAddr *addr, struct SceNetAdhocctlNickname *nickname)
{
    (void)addr;
    if (nickname)
        memset(nickname, 0, sizeof(*nickname));
    return 0;
}

EXPORT(ScePspnetAdhoc, 0x051D8368, int, sceNetAdhocctlGetParameter,
       struct SceNetAdhocctlParameter *parameter)
{
    if (parameter)
        memset(parameter, 0, sizeof(*parameter));
    return 0;
}

EXPORT(ScePspnetAdhoc, 0xA40F17E0, int, sceNetAdhocctlGetEtherAddr,
       struct SceNetEtherAddr *addr)
{
    static const unsigned char local_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

    if (!addr)
        return ADHOC_INVALID_ARG;
    memcpy(addr->data, local_mac, sizeof(local_mac));
    return 0;
}

DECLARE_LIBRARY(ScePspnetAdhoc, 0x19C6AFDC);
