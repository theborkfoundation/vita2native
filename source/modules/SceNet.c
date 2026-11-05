/*
 * SceNet: BSD-socket API backed by the host's POSIX sockets.
 *
 * The Vita API is BSD-flavoured: socket option levels/names, message flags and
 * sockaddr layouts all differ from Linux, so everything crossing the boundary
 * is translated here.
 */

#include <arpa/inet.h>
#include <stdbool.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <psp2/net/net.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include "module.h"

static SceNetSockaddr *host_to_sce_addr(const struct sockaddr *src, SceNetSockaddr *dst,
                                        unsigned int *dstlen)
{
    if (src->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)src;

        memset(dst, 0, sizeof(*dst));
        dst->sa_len    = sizeof(SceNetSockaddrIn);
        dst->sa_family = SCE_NET_AF_INET;
        memcpy(&dst->sa_data[0], &in->sin_port, 2);
        memcpy(&dst->sa_data[2], &in->sin_addr, 4);
        *dstlen = sizeof(SceNetSockaddr);
        return dst;
    }

    *dstlen = 0;
    return dst;
}

static int sce_to_host_addr(const SceNetSockaddr *src, struct sockaddr_storage *dst,
                            socklen_t *dstlen)
{
    if (!src)
        return -1;

    memset(dst, 0, sizeof(*dst));

    if (src->sa_family == SCE_NET_AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)dst;

        in->sin_family = AF_INET;
        memcpy(&in->sin_port, &src->sa_data[0], 2);
        memcpy(&in->sin_addr, &src->sa_data[2], 4);
        *dstlen = sizeof(*in);
        return 0;
    }

    return -1;
}

static int sce_to_host_flags(int flags)
{
    int host = 0;

    if (flags & SCE_NET_MSG_PEEK)
        host |= MSG_PEEK;
    if (flags & SCE_NET_MSG_WAITALL)
        host |= MSG_WAITALL;
    if (flags & SCE_NET_MSG_DONTWAIT)
        host |= MSG_DONTWAIT;

    return host;
}

/* Returns -1 when the option has no host equivalent. */
static int sce_to_host_sockopt(int level, int name, int *host_level, int *host_name)
{
    if (level == SCE_NET_SOL_SOCKET) {
        *host_level = SOL_SOCKET;
        switch (name) {
        case SCE_NET_SO_REUSEADDR: *host_name = SO_REUSEADDR; return 0;
        case SCE_NET_SO_KEEPALIVE: *host_name = SO_KEEPALIVE; return 0;
        case SCE_NET_SO_BROADCAST: *host_name = SO_BROADCAST; return 0;
        case SCE_NET_SO_LINGER: *host_name = SO_LINGER; return 0;
        case SCE_NET_SO_OOBINLINE: *host_name = SO_OOBINLINE; return 0;
        case SCE_NET_SO_REUSEPORT: *host_name = SO_REUSEPORT; return 0;
        case SCE_NET_SO_SNDBUF: *host_name = SO_SNDBUF; return 0;
        case SCE_NET_SO_RCVBUF: *host_name = SO_RCVBUF; return 0;
        case SCE_NET_SO_ERROR: *host_name = SO_ERROR; return 0;
        case SCE_NET_SO_TYPE: *host_name = SO_TYPE; return 0;
        default: return -1;
        }
    }

    if (level == SCE_NET_IPPROTO_TCP) {
        *host_level = IPPROTO_TCP;
        switch (name) {
        case 1: *host_name = TCP_NODELAY; return 0; /* SCE_NET_TCP_NODELAY */
        default: return -1;
        }
    }

    if (level == SCE_NET_IPPROTO_IP) {
        *host_level = IPPROTO_IP;
        switch (name) {
        case 2: *host_name = IP_MULTICAST_TTL; return 0; /* SCE_NET_IP_MULTICAST_TTL */
        case 9: *host_name = IP_ADD_MEMBERSHIP; return 0; /* SCE_NET_IP_ADD_MEMBERSHIP */
        case 10: *host_name = IP_DROP_MEMBERSHIP; return 0;
        default: return -1;
        }
    }

    return -1;
}

EXPORT(SceNet, 0xEB03E265, int, sceNetInit, SceNetInitParam *param)
{
    (void)param;

    return 0;
}

EXPORT(SceNet, 0xEA3CC286, int, sceNetTerm, void)
{
    return 0;
}

EXPORT(SceNet, 0xF084FCE3, int, sceNetSocket, const char *name, int domain, int type,
       int protocol)
{
    int fd;

    (void)name;

    fd = socket(domain, type, protocol);
    return fd < 0 ? -1 : fd;
}

EXPORT(SceNet, 0x29822B4D, int, sceNetSocketClose, int s)
{
    return close(s) < 0 ? -1 : 0;
}

EXPORT(SceNet, 0x11E5B6F6, int, sceNetConnect, int s, const SceNetSockaddr *name,
       unsigned int namelen)
{
    struct sockaddr_storage addr;
    socklen_t addrlen;

    (void)namelen;
    if (sce_to_host_addr(name, &addr, &addrlen) < 0)
        return -1;

    return connect(s, (const struct sockaddr *)&addr, addrlen) < 0 ? -1 : 0;
}

EXPORT(SceNet, 0x1296A94B, int, sceNetBind, int s, const SceNetSockaddr *addr,
       unsigned int addrlen)
{
    struct sockaddr_storage host_addr;
    socklen_t host_len;

    (void)addrlen;
    if (sce_to_host_addr(addr, &host_addr, &host_len) < 0)
        return -1;

    return bind(s, (const struct sockaddr *)&host_addr, host_len) < 0 ? -1 : 0;
}

EXPORT(SceNet, 0x7A8DA094, int, sceNetListen, int s, int backlog)
{
    return listen(s, backlog) < 0 ? -1 : 0;
}

EXPORT(SceNet, 0x1ADF9BB1, int, sceNetAccept, int s, SceNetSockaddr *addr,
       unsigned int *addrlen)
{
    struct sockaddr_storage host_addr;
    socklen_t host_len = sizeof(host_addr);
    int fd = accept(s, (struct sockaddr *)&host_addr, &host_len);

    if (fd < 0)
        return -1;

    if (addr && addrlen) {
        unsigned int len;

        host_to_sce_addr((const struct sockaddr *)&host_addr, addr, &len);
        *addrlen = len;
    }

    return fd;
}

EXPORT(SceNet, 0x023643B7, int, sceNetRecv, int s, void *buf, unsigned int len, int flags)
{
    return recv(s, buf, len, sce_to_host_flags(flags));
}

EXPORT(SceNet, 0xE3DD8CD9, int, sceNetSend, int s, const void *msg, unsigned int len, int flags)
{
    return send(s, msg, len, sce_to_host_flags(flags));
}

EXPORT(SceNet, 0xB226138B, int, sceNetRecvfrom, int s, void *buf, unsigned int len, int flags,
       SceNetSockaddr *from, unsigned int *fromlen)
{
    struct sockaddr_storage host_addr;
    socklen_t host_len = sizeof(host_addr);
    int result = recvfrom(s, buf, len, sce_to_host_flags(flags),
                          (struct sockaddr *)&host_addr, &host_len);

    if (result < 0)
        return result;

    if (from && fromlen) {
        unsigned int out_len;

        host_to_sce_addr((const struct sockaddr *)&host_addr, from, &out_len);
        *fromlen = out_len;
    }

    return result;
}

EXPORT(SceNet, 0x52DB31D5, int, sceNetSendto, int s, const void *msg, unsigned int len, int flags,
       const SceNetSockaddr *to, unsigned int tolen)
{
    struct sockaddr_storage host_addr;
    socklen_t host_len;

    (void)tolen;
    if (sce_to_host_addr(to, &host_addr, &host_len) < 0)
        return -1;

    return sendto(s, msg, len, sce_to_host_flags(flags), (const struct sockaddr *)&host_addr,
                  host_len);
}

EXPORT(SceNet, 0x69E50BB5, int, sceNetShutdown, int s, int how)
{
    return shutdown(s, how) < 0 ? -1 : 0;
}

EXPORT(SceNet, 0x065505CA, int, sceNetSetsockopt, int s, int level, int optname,
       const void *optval, unsigned int optlen)
{
    int host_level;
    int host_name;

    if (sce_to_host_sockopt(level, optname, &host_level, &host_name) < 0)
        return -1;

    return setsockopt(s, host_level, host_name, optval, optlen) < 0 ? -1 : 0;
}

EXPORT(SceNet, 0xBA652062, int, sceNetGetsockopt, int s, int level, int optname, void *optval,
       unsigned int *optlen)
{
    int host_level;
    int host_name;
    socklen_t host_len;

    if (sce_to_host_sockopt(level, optname, &host_level, &host_name) < 0)
        return -1;

    host_len = optlen ? (socklen_t)*optlen : 0;
    if (getsockopt(s, host_level, host_name, optval, &host_len) < 0)
        return -1;

    if (optlen)
        *optlen = host_len;
    return 0;
}

EXPORT(SceNet, 0x1C66A6DB, int, sceNetGetsockname, int s, SceNetSockaddr *name,
       unsigned int *namelen)
{
    struct sockaddr_storage host_addr;
    socklen_t host_len = sizeof(host_addr);

    if (getsockname(s, (struct sockaddr *)&host_addr, &host_len) < 0)
        return -1;

    if (name && namelen)
        host_to_sce_addr((const struct sockaddr *)&host_addr, name, namelen);
    return 0;
}

EXPORT(SceNet, 0x2348D353, int, sceNetGetpeername, int s, SceNetSockaddr *name,
       unsigned int *namelen)
{
    struct sockaddr_storage host_addr;
    socklen_t host_len = sizeof(host_addr);

    if (getpeername(s, (struct sockaddr *)&host_addr, &host_len) < 0)
        return -1;

    if (name && namelen)
        host_to_sce_addr((const struct sockaddr *)&host_addr, name, namelen);
    return 0;
}

EXPORT(SceNet, 0xE37F34AA, int *, sceNetErrnoLoc, void)
{
    return &errno;
}

EXPORT(SceNet, 0x4C30B03C, unsigned int, sceNetHtonl, unsigned int value)
{
    return htonl(value);
}

EXPORT(SceNet, 0xD2EAA645, unsigned int, sceNetNtohl, unsigned int value)
{
    return ntohl(value);
}

EXPORT(SceNet, 0x9FA3207B, unsigned short, sceNetHtons, unsigned short value)
{
    return htons(value);
}

EXPORT(SceNet, 0x07845128, unsigned short, sceNetNtohs, unsigned short value)
{
    return ntohs(value);
}

EXPORT(SceNet, 0x12C19209, unsigned long long, sceNetHtonll, unsigned long long value)
{
    return ((unsigned long long)htonl((unsigned int)value) << 32) |
           htonl((unsigned int)(value >> 32));
}

EXPORT(SceNet, 0xFB3336A6, unsigned long long, sceNetNtohll, unsigned long long value)
{
    return sceNetHtonll(value);
}

EXPORT(SceNet, 0xD5EEB048, int, sceNetInetPton, int af, const char *src, void *dst)
{
    if (af != SCE_NET_AF_INET)
        return -1;

    return inet_pton(AF_INET, src, dst) == 1 ? 0 : -1;
}

EXPORT(SceNet, 0x98839B74, const char *, sceNetInetNtop, int af, const void *src, char *dst,
       unsigned int size)
{
    if (af != SCE_NET_AF_INET)
        return NULL;

    return inet_ntop(AF_INET, src, dst, (socklen_t)size);
}

EXPORT(SceNet, 0x06C05518, int, sceNetGetMacAddress, SceNetEtherAddr *addr, int flags)
{
    (void)flags;

    if (!addr)
        return -1;

    memset(addr, 0, sizeof(*addr));
    return 0;
}

EXPORT(SceNet, 0x84334EB2, int, sceNetEtherNtostr, const SceNetEtherAddr *addr, char *string,
       unsigned int len)
{
    int written;

    if (!addr || !string)
        return -1;

    written = snprintf(string, len, "%02x:%02x:%02x:%02x:%02x:%02x", addr->data[0],
                       addr->data[1], addr->data[2], addr->data[3], addr->data[4],
                       addr->data[5]);
    return written < 0 ? -1 : written;
}

EXPORT(SceNet, 0xEEC6D75F, int, sceNetEtherStrton, const char *string, SceNetEtherAddr *addr)
{
    unsigned int bytes[6];

    if (!string || !addr)
        return -1;

    if (sscanf(string, "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3],
               &bytes[4], &bytes[5]) != 6)
        return -1;

    for (int i = 0; i < 6; i++)
        addr->data[i] = (unsigned char)bytes[i];
    return 0;
}

DECLARE_LIBRARY(SceNet, 0x6bf8b2a2);

/* ------------------------------------------------------------- resolver */

typedef struct {
    bool used;
    int last_error;
} ResolverState;

static ResolverState g_resolvers[8];
static pthread_mutex_t g_resolver_lock = PTHREAD_MUTEX_INITIALIZER;

static ResolverState *resolver_get(int rid)
{
    if (rid < 0 || rid >= (int)(sizeof(g_resolvers) / sizeof(g_resolvers[0])))
        return NULL;

    return g_resolvers[rid].used ? &g_resolvers[rid] : NULL;
}

EXPORT(SceNet, 0x6DA29319, int, sceNetResolverCreate, const char *name,
       SceNetResolverParam *param, int flags)
{
    (void)name;
    (void)param;
    (void)flags;

    pthread_mutex_lock(&g_resolver_lock);
    for (int i = 0; i < (int)(sizeof(g_resolvers) / sizeof(g_resolvers[0])); i++) {
        if (!g_resolvers[i].used) {
            g_resolvers[i].used = true;
            g_resolvers[i].last_error = 0;
            pthread_mutex_unlock(&g_resolver_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_resolver_lock);

    return -1;
}

EXPORT(SceNet, 0x3559F098, int, sceNetResolverDestroy, int rid)
{
    ResolverState *resolver = resolver_get(rid);

    if (!resolver)
        return -1;

    pthread_mutex_lock(&g_resolver_lock);
    resolver->used = false;
    pthread_mutex_unlock(&g_resolver_lock);

    return 0;
}

EXPORT(SceNet, 0x1EB11857, int, sceNetResolverStartNtoa, int rid, const char *hostname,
       SceNetInAddr *addr, int timeout, int retry, int flags)
{
    ResolverState *resolver = resolver_get(rid);
    struct addrinfo hints, *result = NULL;

    (void)timeout;
    (void)retry;
    (void)flags;

    if (!resolver || !hostname || !addr)
        return -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &result) != 0 || !result) {
        resolver->last_error = -1;
        return -1;
    }

    addr->s_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(result);

    return 0;
}

EXPORT(SceNet, 0x0424AE26, int, sceNetResolverStartAton, int rid, const SceNetInAddr *addr,
       char *hostname, int len, int timeout, int retry, int flags)
{
    ResolverState *resolver = resolver_get(rid);
    struct sockaddr_in in;

    (void)timeout;
    (void)retry;
    (void)flags;

    if (!resolver || !addr || !hostname || len <= 0)
        return -1;

    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = addr->s_addr;

    if (getnameinfo((const struct sockaddr *)&in, sizeof(in), hostname, (socklen_t)len, NULL, 0,
                    0) != 0) {
        resolver->last_error = -1;
        return -1;
    }

    return 0;
}

EXPORT(SceNet, 0x874EF500, int, sceNetResolverGetError, int rid, int *result)
{
    ResolverState *resolver = resolver_get(rid);

    if (!resolver || !result)
        return -1;

    *result = resolver->last_error;
    return 0;
}

EXPORT(SceNet, 0x38EBBD57, int, sceNetResolverAbort, int rid, int flags)
{
    (void)rid;
    (void)flags;

    return 0;
}

EXPORT(SceNet, 0xFEC1166D, int, sceNetClearDnsCache, int flags)
{
    (void)flags;

    return 0;
}

/* --------------------------------------------------------------- epoll */

static uint32_t epoll_to_sce_events(uint32_t host_events)
{
    uint32_t events = 0;

    if (host_events & EPOLLIN)
        events |= SCE_NET_EPOLLIN;
    if (host_events & EPOLLOUT)
        events |= SCE_NET_EPOLLOUT;
    if (host_events & EPOLLERR)
        events |= SCE_NET_EPOLLERR;
    if (host_events & EPOLLHUP)
        events |= SCE_NET_EPOLLHUP;

    return events;
}

static uint32_t epoll_to_host_events(uint32_t sce_events)
{
    uint32_t events = 0;

    if (sce_events & SCE_NET_EPOLLIN)
        events |= EPOLLIN;
    if (sce_events & SCE_NET_EPOLLOUT)
        events |= EPOLLOUT;
    if (sce_events & SCE_NET_EPOLLERR)
        events |= EPOLLERR;
    if (sce_events & SCE_NET_EPOLLHUP)
        events |= EPOLLHUP;

    return events;
}

EXPORT(SceNet, 0xF9D102AE, int, sceNetEpollCreate, const char *name, int flags)
{
    int fd;

    (void)name;
    (void)flags;

    fd = epoll_create1(0);
    return fd < 0 ? -1 : fd;
}

EXPORT(SceNet, 0x7915CAF3, int, sceNetEpollDestroy, int eid)
{
    return close(eid) < 0 ? -1 : 0;
}

EXPORT(SceNet, 0x93FCC4E8, int, sceNetEpollAbort, int eid, int flags)
{
    (void)eid;
    (void)flags;

    return 0;
}

EXPORT(SceNet, 0x4C8764AC, int, sceNetEpollControl, int eid, int op, int id,
       SceNetEpollEvent *event)
{
    struct epoll_event host_event;

    memset(&host_event, 0, sizeof(host_event));
    if (event) {
        host_event.events = epoll_to_host_events(event->events);
        host_event.data.u64 = event->data.u64;
    }

    return epoll_ctl(eid, op, id, &host_event) < 0 ? -1 : 0;
}

EXPORT(SceNet, 0x45CE337D, int, sceNetEpollWait, int eid, SceNetEpollEvent *events,
       int max_events, int timeout)
{
    struct epoll_event host_events[32];
    int host_timeout;
    int count;

    if (!events || max_events <= 0)
        return -1;
    if (max_events > (int)(sizeof(host_events) / sizeof(host_events[0])))
        max_events = (int)(sizeof(host_events) / sizeof(host_events[0]));

    host_timeout = timeout < 0 ? -1 : (timeout + 999) / 1000;
    count = epoll_wait(eid, host_events, max_events, host_timeout);
    if (count < 0)
        return -1;

    for (int i = 0; i < count; i++) {
        memset(&events[i], 0, sizeof(events[i]));
        events[i].events = epoll_to_sce_events(host_events[i].events);
        events[i].data.u64 = host_events[i].data.u64;
    }

    return count;
}

EXPORT(SceNet, 0x92D3E767, int, sceNetEpollWaitCB, int eid, SceNetEpollEvent *events,
       int max_events, int timeout)
{
    return sceNetEpollWait(eid, events, max_events, timeout);
}

/* ------------------------------------------------------- introspection */

/*
 * Fills one SceNetSockInfo from the host descriptor. `fd` is a host socket
 * descriptor because sceNetSocket() returns it directly.
 */
static void fill_sock_info(int fd, SceNetSockInfo *info)
{
    struct sockaddr_in addr;
    socklen_t addr_len;
    int value;
    socklen_t value_len;

    memset(info, 0, sizeof(*info));
    info->pid = 1;
    info->s = fd;
    info->flags = SCE_NET_SOCKINFO_F_SELF;

    value_len = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &value_len) == 0)
        info->socket_type = (char)value;

    value_len = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &value_len) == 0 && value) {
        info->state = SCE_NET_SOCKINFO_STATE_LISTEN;
    } else {
        info->state = SCE_NET_SOCKINFO_STATE_OPENED;
    }

    addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) == 0 && addr.sin_family == AF_INET) {
        memcpy(&info->local_adr, &addr.sin_addr, 4);
        info->local_port = ntohs(addr.sin_port);
    }

    addr_len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == 0 && addr.sin_family == AF_INET) {
        memcpy(&info->remote_adr, &addr.sin_addr, 4);
        info->remote_port = ntohs(addr.sin_port);
        info->state = SCE_NET_SOCKINFO_STATE_ESTABLISHED;
    }
}

EXPORT(SceNet, 0xB1AF6840, int, sceNetGetSockInfo, int s, SceNetSockInfo *info, int n, int flags)
{
    int count = 0;

    (void)flags;

    if (!info || n <= 0)
        return -1;

    if (s >= 0) {
        fill_sock_info(s, &info[0]);
        return 1;
    }

    for (int fd = 0; fd < 1024 && count < n; fd++) {
        int type;
        socklen_t type_len = sizeof(type);

        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0)
            continue;
        fill_sock_info(fd, &info[count]);
        count++;
    }

    return count;
}

EXPORT(SceNet, 0x138CF1D6, int, sceNetGetSockIdInfo, SceNetFdSet *fds, int sockinfo_flags,
       int flags)
{
    (void)sockinfo_flags;
    (void)flags;

    if (!fds)
        return -1;

    memset(fds, 0, sizeof(*fds));
    for (int fd = 0; fd < 1024; fd++) {
        int type;
        socklen_t type_len = sizeof(type);

        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0)
            continue;
        fds->bits[fd / 32] |= 1u << (fd % 32);
    }

    return 0;
}

EXPORT(SceNet, 0xA86F8FE5, int, sceNetGetStatisticsInfo, SceNetStatisticsInfo *info, int flags)
{
    (void)flags;

    if (!info)
        return -1;

    memset(info, 0, sizeof(*info));
    info->kernel_mem_free_size = 0x100000;
    info->kernel_mem_free_min = 0x100000;
    info->libnet_mem_free_size = 0x100000;
    info->libnet_mem_free_min = 0x100000;

    return 0;
}

/* ---------------------------------------------------------- send/recvmsg */

#define SCE_NET_MAX_IOV 16

static int sce_msghdr_to_host(const SceNetMsghdr *msg, struct msghdr *host,
                              struct iovec *iov, struct sockaddr_storage *addr,
                              socklen_t *addr_len)
{
    if (!msg || msg->msg_iovlen < 0 || msg->msg_iovlen > SCE_NET_MAX_IOV)
        return -1;

    memset(host, 0, sizeof(*host));
    memset(iov, 0, sizeof(*iov) * SCE_NET_MAX_IOV);

    if (msg->msg_iov && msg->msg_iovlen > 0) {
        for (int i = 0; i < msg->msg_iovlen; i++) {
            iov[i].iov_base = msg->msg_iov[i].iov_base;
            iov[i].iov_len = msg->msg_iov[i].iov_len;
        }
        host->msg_iov = iov;
        host->msg_iovlen = (size_t)msg->msg_iovlen;
    }

    if (msg->msg_name && msg->msg_namelen >= sizeof(SceNetSockaddr)) {
        if (sce_to_host_addr((const SceNetSockaddr *)msg->msg_name, addr, addr_len) < 0)
            return -1;
        host->msg_name = addr;
        host->msg_namelen = *addr_len;
    }

    return 0;
}

EXPORT(SceNet, 0x99C579AE, int, sceNetSendmsg, int s, const SceNetMsghdr *msg, int flags)
{
    struct sockaddr_storage addr;
    struct iovec iov[SCE_NET_MAX_IOV];
    struct msghdr host;
    socklen_t addr_len = 0;

    if (sce_msghdr_to_host(msg, &host, iov, &addr, &addr_len) < 0)
        return -1;

    return (int)sendmsg(s, &host, sce_to_host_flags(flags));
}

EXPORT(SceNet, 0xDE94C6FE, int, sceNetRecvmsg, int s, SceNetMsghdr *msg, int flags)
{
    struct sockaddr_storage addr;
    struct iovec iov[SCE_NET_MAX_IOV];
    struct msghdr host;
    socklen_t addr_len = 0;
    int ret;

    if (sce_msghdr_to_host(msg, &host, iov, &addr, &addr_len) < 0)
        return -1;

    ret = (int)recvmsg(s, &host, sce_to_host_flags(flags));
    if (ret >= 0) {
        if (host.msg_namelen > 0) {
            SceNetSockaddr sce_addr;
            unsigned int sce_len = 0;

            host_to_sce_addr((const struct sockaddr *)&addr, &sce_addr, &sce_len);
            if (msg->msg_name && msg->msg_namelen >= sizeof(sce_addr)) {
                memcpy(msg->msg_name, &sce_addr, sizeof(sce_addr));
                msg->msg_namelen = sce_len;
            }
        }
        msg->msg_flags = host.msg_flags;
    }

    return ret;
}

/* --------------------------------------------------------- diagnostics */

static SceNetEmulationParam g_net_emulation;

EXPORT(SceNet, 0x561DFD03, int, sceNetEmulationSet, SceNetEmulationParam *param, int flags)
{
    (void)flags;

    if (!param)
        return -1;

    g_net_emulation = *param;
    g_net_emulation.result = 0;
    return 0;
}

EXPORT(SceNet, 0xAE3F4AC6, int, sceNetEmulationGet, SceNetEmulationParam *param, int flags)
{
    (void)flags;

    if (!param)
        return -1;

    *param = g_net_emulation;
    param->result = 0;
    return 0;
}

EXPORT(SceNet, 0x658B903B, int, sceNetShowIfconfig, void *buffer, int length)
{
    (void)buffer;
    (void)length;
    return 0;
}

EXPORT(SceNet, 0x6AB3B74B, int, sceNetShowRoute, void)
{
    return 0;
}

EXPORT(SceNet, 0x338EDC2E, int, sceNetShowNetstat, void)
{
    return 0;
}

EXPORT(SceNet, 0xD62EF218, int, sceNetSetDnsInfo, SceNetDnsInfo *info, int flags)
{
    (void)info;
    (void)flags;

    /* The host resolver is used, so the Vita DNS override is accepted and
     * ignored. */
    return 0;
}

EXPORT(SceNet, 0x891C1B9B, int, sceNetSocketAbort, int s, int flags)
{
    (void)flags;

    shutdown(s, SHUT_RDWR);
    return 0;
}

/* --------------------------------------------------------------- dumps */

static int g_next_dump_id = 1;

EXPORT(SceNet, 0xAFF9FA4D, int, sceNetDumpCreate, const char *name, int length, int flags)
{
    (void)name;
    (void)length;
    (void)flags;

    return g_next_dump_id++;
}

EXPORT(SceNet, 0x82DDCF63, int, sceNetDumpDestroy, int id)
{
    (void)id;
    return 0;
}

EXPORT(SceNet, 0x3B24E75F, int, sceNetDumpAbort, int id, int flags)
{
    (void)id;
    (void)flags;
    return 0;
}

EXPORT(SceNet, 0x04042925, int, sceNetDumpRead, int id, void *buffer, int length, int *flags)
{
    (void)id;
    (void)buffer;
    (void)length;

    if (flags)
        *flags = 0;

    /* No captures are recorded, so there is never a packet to return. */
    return 0;
}
