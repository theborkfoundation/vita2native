/*
 * Exercises the diagnostic SceNet surface added on top of the BSD sockets:
 * socket introspection, statistics, emulation parameters, message vectors and
 * the dump API.
 */

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/types.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#include "module.h"
#include "test.h"

int sceNetSocket(const char *name, int domain, int type, int protocol);
int sceNetSocketClose(int s);
int sceNetBind(int s, const SceNetSockaddr *addr, unsigned int addrlen);
int sceNetGetsockname(int s, SceNetSockaddr *name, unsigned int *namelen);
int sceNetSendmsg(int s, const SceNetMsghdr *msg, int flags);
int sceNetRecvmsg(int s, SceNetMsghdr *msg, int flags);
int sceNetGetSockInfo(int s, SceNetSockInfo *info, int n, int flags);
int sceNetGetSockIdInfo(SceNetFdSet *fds, int sockinfo_flags, int flags);
int sceNetGetStatisticsInfo(SceNetStatisticsInfo *info, int flags);
int sceNetEmulationSet(SceNetEmulationParam *param, int flags);
int sceNetEmulationGet(SceNetEmulationParam *param, int flags);
int sceNetSetDnsInfo(SceNetDnsInfo *info, int flags);
int sceNetShowIfconfig(void *buffer, int length);
int sceNetShowRoute(void);
int sceNetShowNetstat(void);
int sceNetSocketAbort(int s, int flags);
int sceNetDumpCreate(const char *name, int length, int flags);
int sceNetDumpRead(int id, void *buffer, int length, int *flags);
int sceNetDumpDestroy(int id);
int sceNetDumpAbort(int id, int flags);

int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlInetGetState(int *state);
int sceNetCtlInetGetInfo(int code, SceNetCtlInfo *info);
int sceNetCtlGetNatInfo(SceNetCtlNatInfo *natinfo);
int sceNetCtlGetIfStat(int device, void *ifstat);
int sceNetCtlGetPhoneMaxDownloadableSize(int64_t *size);
int sceNetCtlInetRegisterCallback(SceNetCtlCallback callback, void *arg, int *callback_id);
int sceNetCtlInetUnregisterCallback(int callback_id);
int sceNetCtlAdhocGetState(int *state);
int sceNetCtlAdhocGetInAddr(SceNetInAddr *inaddr);

typedef struct {
    SceSize size;
    SceUInt32 total_sec;
    SceUInt64 tx_bytes;
    SceUInt64 rx_bytes;
    SceUInt32 reset_tick[2];
    SceUInt32 reserved[8];
} TestIfStat;

static void make_addr(SceNetSockaddr *addr, unsigned int port, unsigned int ip)
{
    uint16_t net_port = htons((uint16_t)port);
    uint32_t net_ip = htonl(ip);

    memset(addr, 0, sizeof(*addr));
    addr->sa_len = sizeof(SceNetSockaddrIn);
    addr->sa_family = SCE_NET_AF_INET;
    memcpy(&addr->sa_data[0], &net_port, sizeof(net_port));
    memcpy(&addr->sa_data[2], &net_ip, sizeof(net_ip));
}

static void *net_smoke_callback(int event_type, void *arg)
{
    (void)event_type;
    (void)arg;
    return NULL;
}

int main(void)
{
    SceNetEmulationParam emulation;
    SceNetStatisticsInfo statistics;
    SceNetSockaddr local;
    SceNetSockInfo info;
    SceNetFdSet fdset;
    SceNetDnsInfo dns;
    SceNetMsghdr msg;
    SceNetIovec iov[2];
    int receiver, sender;
    unsigned int addr_len;
    const char *part1 = "hello ";
    const char *part2 = "net";
    char buf1[16] = { 0 };
    char buf2[16] = { 0 };
    int dump;
    int ret;

    receiver = sceNetSocket("receiver", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    REQUIRE(receiver >= 0, "could not create the receiver socket");

    make_addr(&local, 0, INADDR_LOOPBACK);
    REQUIRE(sceNetBind(receiver, &local, sizeof(local)) == 0, "bind failed");

    addr_len = sizeof(local);
    memset(&local, 0, sizeof(local));
    REQUIRE(sceNetGetsockname(receiver, &local, &addr_len) == 0, "getsockname failed");
    REQUIRE(local.sa_family == SCE_NET_AF_INET, "getsockname returned the wrong family");

    memset(&info, 0, sizeof(info));
    REQUIRE(sceNetGetSockInfo(receiver, &info, 1, 0) == 1, "GetSockInfo did not report a socket");
    REQUIRE(info.s == receiver, "GetSockInfo reported the wrong descriptor");
    REQUIRE(info.socket_type == SCE_NET_SOCK_DGRAM, "GetSockInfo reported the wrong type");
    REQUIRE(info.local_port != 0, "GetSockInfo did not report the bound port");
    REQUIRE(info.flags & SCE_NET_SOCKINFO_F_SELF, "GetSockInfo did not mark the owner");

    REQUIRE(sceNetGetSockInfo(-1, &info, 1, 0) == 1, "GetSockInfo enumeration failed");

    memset(&fdset, 0, sizeof(fdset));
    REQUIRE(sceNetGetSockIdInfo(&fdset, 0, 0) == 0, "GetSockIdInfo failed");
    REQUIRE(fdset.bits[receiver / 32] & (1u << (receiver % 32)),
            "GetSockIdInfo did not report the socket");

    REQUIRE(sceNetGetStatisticsInfo(&statistics, 0) == 0, "GetStatisticsInfo failed");
    REQUIRE(statistics.libnet_mem_free_size > 0, "statistics reported no memory");

    memset(&emulation, 0, sizeof(emulation));
    emulation.version = 1;
    emulation.seed = 0x1234;
    REQUIRE(sceNetEmulationSet(&emulation, 0) == 0, "EmulationSet failed");
    memset(&emulation, 0, sizeof(emulation));
    REQUIRE(sceNetEmulationGet(&emulation, 0) == 0, "EmulationGet failed");
    REQUIRE(emulation.seed == 0x1234, "the emulation parameters did not round-trip");

    memset(&dns, 0, sizeof(dns));
    REQUIRE(sceNetSetDnsInfo(&dns, 0) == 0, "SetDnsInfo failed");
    REQUIRE(sceNetShowIfconfig(NULL, 0) == 0, "ShowIfconfig failed");
    REQUIRE(sceNetShowRoute() == 0, "ShowRoute failed");
    REQUIRE(sceNetShowNetstat() == 0, "ShowNetstat failed");

    sender = sceNetSocket("sender", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    REQUIRE(sender >= 0, "could not create the sender socket");

    iov[0].iov_base = (void *)part1;
    iov[0].iov_len = (unsigned int)strlen(part1);
    iov[1].iov_base = (void *)part2;
    iov[1].iov_len = (unsigned int)strlen(part2);

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &local;
    msg.msg_namelen = sizeof(local);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    ret = sceNetSendmsg(sender, &msg, 0);
    REQUIRE(ret == (int)(strlen(part1) + strlen(part2)), "Sendmsg returned the wrong length");

    iov[0].iov_base = buf1;
    iov[0].iov_len = (unsigned int)strlen(part1);
    iov[1].iov_base = buf2;
    iov[1].iov_len = sizeof(buf2);

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    ret = sceNetRecvmsg(receiver, &msg, 0);
    REQUIRE(ret == (int)(strlen(part1) + strlen(part2)), "Recvmsg returned the wrong length");
    REQUIRE(memcmp(buf1, part1, strlen(part1)) == 0, "the first message vector was corrupted");
    REQUIRE(memcmp(buf2, part2, strlen(part2)) == 0, "the second message vector was corrupted");

    dump = sceNetDumpCreate("smoke", 64, 0);
    REQUIRE(dump >= 0, "DumpCreate failed");
    REQUIRE(sceNetDumpRead(dump, buf1, sizeof(buf1), NULL) == 0, "DumpRead returned data");
    REQUIRE(sceNetDumpDestroy(dump) == 0, "DumpDestroy failed");
    REQUIRE(sceNetDumpAbort(dump, 0) == 0, "DumpAbort failed");

    REQUIRE(sceNetSocketAbort(sender, 0) == 0, "SocketAbort failed");
    REQUIRE(sceNetSocketClose(sender) == 0, "close sender failed");
    REQUIRE(sceNetSocketClose(receiver) == 0, "close receiver failed");

    /* --- SceNetCtl ---------------------------------------------------- */
    {
        SceNetCtlInfo netctl_info;
        SceNetCtlNatInfo nat_info;
        SceNetInAddr adhoc_addr;
        TestIfStat ifstat;
        int state = -1;
        int callback_id = 0;
        int64_t max_size = 0;

        REQUIRE(sceNetCtlInit() == 0, "NetCtlInit failed");
        REQUIRE(sceNetCtlInetGetState(&state) == 0, "InetGetState failed");
        REQUIRE(state == SCE_NETCTL_STATE_CONNECTED || state == SCE_NETCTL_STATE_DISCONNECTED,
                "InetGetState returned an unknown state");

        memset(&netctl_info, 0, sizeof(netctl_info));
        REQUIRE(sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &netctl_info) == 0 ||
                    state == SCE_NETCTL_STATE_DISCONNECTED,
                "InetGetInfo failed");
        if (state == SCE_NETCTL_STATE_CONNECTED)
            REQUIRE(netctl_info.ip_address[0] != '\0', "the connected interface has no address");

        memset(&nat_info, 0, sizeof(nat_info));
        REQUIRE(sceNetCtlGetNatInfo(&nat_info) == 0, "GetNatInfo failed");
        REQUIRE(nat_info.size == sizeof(nat_info), "GetNatInfo did not set the size");

        memset(&ifstat, 0, sizeof(ifstat));
        REQUIRE(sceNetCtlGetIfStat(0, &ifstat) == 0, "GetIfStat failed");
        REQUIRE(ifstat.size == sizeof(ifstat), "GetIfStat did not set the size");

        REQUIRE(sceNetCtlGetPhoneMaxDownloadableSize(&max_size) == 0, "GetPhoneMax failed");
        REQUIRE(max_size == INT64_MAX, "the downloadable size is not unlimited");

        REQUIRE(sceNetCtlInetRegisterCallback(NULL, NULL, &callback_id) < 0,
                "a null NetCtl callback was accepted");
        REQUIRE(sceNetCtlInetRegisterCallback((SceNetCtlCallback)net_smoke_callback, NULL,
                                              &callback_id) == 0,
                "NetCtl callback registration failed");
        REQUIRE(sceNetCtlInetUnregisterCallback(callback_id) == 0,
                "NetCtl callback unregistration failed");

        REQUIRE(sceNetCtlAdhocGetState(&state) == 0 && state == SCE_NETCTL_STATE_DISCONNECTED,
                "adhoc is not disconnected");
        memset(&adhoc_addr, 0xff, sizeof(adhoc_addr));
        REQUIRE(sceNetCtlAdhocGetInAddr(&adhoc_addr) == 0, "AdhocGetInAddr failed");
        REQUIRE(adhoc_addr.s_addr == 0, "adhoc returned an address");
        sceNetCtlTerm();
    }

    puts("net_smoke: ok");
    return 0;
}
