/*
 * SceNetCtl: network interface status.
 *
 * The host has ordinary kernel networking, so the information is gathered
 * from getifaddrs(), ioctl(SIOCGIF*), /proc/net/route and /etc/resolv.conf.
 * Scenario/adhoc APIs have no host equivalent and report "disconnected".
 */

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <psp2/types.h>
#include <psp2/net/netctl.h>

#include "host.h"
#include "module.h"

/* Not present in the public VitaSDK headers. */
#define SCE_NET_CTL_ERROR_NOT_INITIALIZED 0x80412101
#define SCE_NET_CTL_ERROR_CALLBACK_MAX    0x80412103
#define SCE_NET_CTL_ERROR_INVALID_ID      0x80412105
#define SCE_NET_CTL_ERROR_INVALID_CODE    0x80412106
#define SCE_NET_CTL_ERROR_INVALID_ADDR    0x80412107
#define SCE_NET_CTL_ERROR_NOT_CONNECTED   0x80412108

#define VITA_NETCTL_MAX_CALLBACKS 8

typedef struct {
    SceSize size;
    SceUInt32 total_sec;
    SceUInt64 tx_bytes;
    SceUInt64 rx_bytes;
    SceUInt32 reset_tick[2];
    SceUInt32 reserved[8];
} VitaNetCtlIfStat;

typedef struct {
    bool used;
    int id;
    SceNetCtlCallback callback;
    void *arg;
} VitaNetCtlCallbackEntry;

static bool g_netctl_initialized;
static int g_next_callback_id = 1;
static VitaNetCtlCallbackEntry g_inet_callbacks[VITA_NETCTL_MAX_CALLBACKS];
static VitaNetCtlCallbackEntry g_adhoc_callbacks[VITA_NETCTL_MAX_CALLBACKS];

/* Returns the first non-loopback, up, IPv4-capable interface. */
static bool find_primary_interface(struct ifaddrs **out)
{
    struct ifaddrs *list;

    if (getifaddrs(&list) != 0)
        return false;

    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;

        *out = list;
        return true;
    }

    freeifaddrs(list);
    return false;
}

static struct ifaddrs *find_named(struct ifaddrs *list, const char *name)
{
    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
            strcmp(ifa->ifa_name, name) == 0)
            return ifa;
    }
    return NULL;
}

static void copy_ipv4(char *out, size_t out_size, const struct sockaddr *addr)
{
    if (!addr) {
        out[0] = '\0';
        return;
    }

    inet_ntop(AF_INET, &((const struct sockaddr_in *)addr)->sin_addr, out, (socklen_t)out_size);
}

static void read_default_route(char *out, size_t out_size)
{
    FILE *file = fopen("/proc/net/route", "r");
    char line[256];

    out[0] = '\0';
    if (!file)
        return;

    /* Skip the header, then find the default destination (0.0.0.0). */
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return;
    }

    while (fgets(line, sizeof(line), file)) {
        char iface[64];
        unsigned int destination = 0, gateway = 0;

        if (sscanf(line, "%63s %x %x", iface, &destination, &gateway) != 3)
            continue;
        if (destination != 0)
            continue;

        struct in_addr addr;
        addr.s_addr = gateway;
        inet_ntop(AF_INET, &addr, out, (socklen_t)out_size);
        break;
    }

    fclose(file);
}

static void read_dns_servers(char *primary, size_t primary_size, char *secondary,
                             size_t secondary_size)
{
    FILE *file = fopen("/etc/resolv.conf", "r");
    char line[256];
    int found = 0;

    primary[0] = '\0';
    secondary[0] = '\0';
    if (!file)
        return;

    while (fgets(line, sizeof(line), file) && found < 2) {
        char keyword[32], address[64];

        if (sscanf(line, "%31s %63s", keyword, address) != 2)
            continue;
        if (strcmp(keyword, "nameserver") != 0)
            continue;

        if (found == 0)
            snprintf(primary, primary_size, "%s", address);
        else
            snprintf(secondary, secondary_size, "%s", address);
        found++;
    }

    fclose(file);
}

static void read_ether_addr(const char *name, SceNetEtherAddr *addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    memset(addr, 0, sizeof(*addr));
    if (fd < 0)
        return;

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(addr->data, ifr.ifr_hwaddr.sa_data, sizeof(addr->data));

    close(fd);
}

static unsigned int read_mtu(const char *name)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    unsigned int mtu = 1500;

    if (fd < 0)
        return mtu;

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(fd, SIOCGIFMTU, &ifr) == 0)
        mtu = (unsigned int)ifr.ifr_mtu;

    close(fd);
    return mtu;
}

static bool network_forced_off(void);

static int fill_info(int code, SceNetCtlInfo *info)
{
    struct ifaddrs *list = NULL;
    struct ifaddrs *primary;
    char name[IFNAMSIZ] = "";

    if (!info)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    memset(info, 0, sizeof(*info));

    if (network_forced_off() || !find_primary_interface(&list)) {
        /* No network: still answer identity constants, but no addresses. */
        switch (code) {
        case SCE_NETCTL_INFO_GET_CNF_NAME:
            snprintf(info->cnf_name, sizeof(info->cnf_name), "vita2native");
            return 0;
        case SCE_NETCTL_INFO_GET_DEVICE:
            info->device = 1;
            return 0;
        case SCE_NETCTL_INFO_GET_IP_CONFIG:
            info->ip_config = 1;
            return 0;
        case SCE_NETCTL_INFO_GET_DHCP_HOSTNAME:
            snprintf(info->dhcp_hostname, sizeof(info->dhcp_hostname), "vita2native");
            return 0;
        case SCE_NETCTL_INFO_GET_LINK:
            info->link = 0;
            return 0;
        default:
            return SCE_NET_CTL_ERROR_NOT_CONNECTED;
        }
    }

    /* Recover the interface name from the first matching entry. */
    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
            (ifa->ifa_flags & IFF_UP) && !(ifa->ifa_flags & IFF_LOOPBACK)) {
            snprintf(name, sizeof(name), "%s", ifa->ifa_name);
            break;
        }
    }

    primary = name[0] ? find_named(list, name) : NULL;

    switch (code) {
    case SCE_NETCTL_INFO_GET_CNF_NAME:
        snprintf(info->cnf_name, sizeof(info->cnf_name), "vita2native");
        break;
    case SCE_NETCTL_INFO_GET_DEVICE:
        info->device = 1;
        break;
    case SCE_NETCTL_INFO_GET_ETHER_ADDR:
        read_ether_addr(name, &info->ether_addr);
        break;
    case SCE_NETCTL_INFO_GET_MTU:
        info->mtu = read_mtu(name);
        break;
    case SCE_NETCTL_INFO_GET_LINK:
        info->link = 1;
        break;
    case SCE_NETCTL_INFO_GET_BSSID:
        memset(&info->bssid, 0, sizeof(info->bssid));
        break;
    case SCE_NETCTL_INFO_GET_SSID:
        info->ssid[0] = '\0';
        break;
    case SCE_NETCTL_INFO_GET_WIFI_SECURITY:
        info->wifi_security = 0;
        break;
    case SCE_NETCTL_INFO_GET_RSSI_DBM:
        info->rssi_dbm = (unsigned int)-50;
        break;
    case SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE:
        info->rssi_percentage = 80;
        break;
    case SCE_NETCTL_INFO_GET_CHANNEL:
        info->channel = 6;
        break;
    case SCE_NETCTL_INFO_GET_IP_CONFIG:
        info->ip_config = 1;
        break;
    case SCE_NETCTL_INFO_GET_DHCP_HOSTNAME:
        snprintf(info->dhcp_hostname, sizeof(info->dhcp_hostname), "vita2native");
        break;
    case SCE_NETCTL_INFO_GET_PPPOE_AUTH_NAME:
        info->pppoe_auth_name[0] = '\0';
        break;
    case SCE_NETCTL_INFO_GET_IP_ADDRESS:
        if (primary)
            copy_ipv4(info->ip_address, sizeof(info->ip_address), primary->ifa_addr);
        break;
    case SCE_NETCTL_INFO_GET_NETMASK:
        if (primary)
            copy_ipv4(info->netmask, sizeof(info->netmask), primary->ifa_netmask);
        break;
    case SCE_NETCTL_INFO_GET_DEFAULT_ROUTE:
        read_default_route(info->default_route, sizeof(info->default_route));
        break;
    case SCE_NETCTL_INFO_GET_PRIMARY_DNS:
    case SCE_NETCTL_INFO_GET_SECONDARY_DNS: {
        char primary_dns[64], secondary_dns[64];

        read_dns_servers(primary_dns, sizeof(primary_dns), secondary_dns, sizeof(secondary_dns));
        if (code == SCE_NETCTL_INFO_GET_PRIMARY_DNS)
            snprintf(info->primary_dns, sizeof(info->primary_dns), "%s", primary_dns);
        else
            snprintf(info->secondary_dns, sizeof(info->secondary_dns), "%s", secondary_dns);
        break;
    }
    case SCE_NETCTL_INFO_GET_HTTP_PROXY_CONFIG:
        info->http_proxy_config = 0;
        break;
    case SCE_NETCTL_INFO_GET_HTTP_PROXY_SERVER:
        info->http_proxy_server[0] = '\0';
        break;
    case SCE_NETCTL_INFO_GET_HTTP_PROXY_PORT:
        info->http_proxy_port = 0;
        break;
    default:
        freeifaddrs(list);
        return SCE_NET_CTL_ERROR_INVALID_CODE;
    }

    freeifaddrs(list);
    return 0;
}

/* Diagnostic: pretend the console is offline, which is what Vita3K reports and
   what makes titles take their "no network" fallback (usually a message dialog
   before the menu becomes interactive). */
static bool network_forced_off(void)
{
    static int forced = -1;

    if (forced < 0)
        forced = getenv("VITA2NATIVE_NO_NETWORK") != NULL;
    return forced != 0;
}

static bool is_connected(void)
{
    struct ifaddrs *list = NULL;

    if (network_forced_off())
        return false;
    if (!find_primary_interface(&list))
        return false;

    freeifaddrs(list);
    return true;
}

EXPORT(SceNetCtl, 0x495CA1DB, int, sceNetCtlInit, void)
{
    g_netctl_initialized = true;
    return 0;
}

EXPORT(SceNetCtl, 0xCD188648, void, sceNetCtlTerm, void)
{
    g_netctl_initialized = false;
    memset(g_inet_callbacks, 0, sizeof(g_inet_callbacks));
    memset(g_adhoc_callbacks, 0, sizeof(g_adhoc_callbacks));
}

EXPORT(SceNetCtl, 0xDFFC3ED4, int, sceNetCtlCheckCallback, void)
{
    return 0;
}

EXPORT(SceNetCtl, 0x6D26AC68, int, sceNetCtlInetGetState, int *state)
{
    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    if (!state)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    *state = is_connected() ? SCE_NETCTL_STATE_CONNECTED : SCE_NETCTL_STATE_DISCONNECTED;

    if (getenv("VITA2NATIVE_INPUT_DEBUG")) {
        static int last = -2;

        if (*state != last) {
            fprintf(stderr, "vita2native: NetCtl state -> %d\n", *state);
            fflush(stderr);
            last = *state;
        }
    }
    return 0;
}

EXPORT(SceNetCtl, 0xB26D07F3, int, sceNetCtlInetGetInfo, int code, SceNetCtlInfo *info)
{
    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;

    return fill_info(code, info);
}

EXPORT(SceNetCtl, 0x4DDD6149, int, sceNetCtlGetNatInfo, SceNetCtlNatInfo *natinfo)
{
    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    if (!natinfo)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    memset(natinfo, 0, sizeof(*natinfo));
    natinfo->size = sizeof(*natinfo);
    natinfo->stun_status = 0;
    natinfo->nat_type = 0;
    return 0;
}

EXPORT(SceNetCtl, 0xDA130450, int, sceNetCtlGetIfStat, int device, VitaNetCtlIfStat *ifstat)
{
    (void)device;

    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    if (!ifstat)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    memset(ifstat, 0, sizeof(*ifstat));
    ifstat->size = sizeof(*ifstat);
    ifstat->total_sec = (SceUInt32)(host_monotonic_ns() / 1000000000ull);
    return 0;
}

EXPORT(SceNetCtl, 0x4FB4BEA1, int, sceNetCtlGetPhoneMaxDownloadableSize, SceInt64 *size)
{
    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    if (!size)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    *size = INT64_MAX;
    return 0;
}

/* --------------------------------------------------------------- results */

EXPORT(SceNetCtl, 0x6B20EC02, int, sceNetCtlInetGetResult, int event_type, int *error_code)
{
    (void)event_type;

    if (error_code)
        *error_code = 0;
    return 0;
}

EXPORT(SceNetCtl, 0x7AE0ED19, int, sceNetCtlAdhocGetResult, int event_type, int *error_code)
{
    (void)event_type;

    if (error_code)
        *error_code = 0;
    return 0;
}

/* ------------------------------------------------------------- callbacks */

static int register_callback(VitaNetCtlCallbackEntry *table, SceNetCtlCallback callback,
                             void *arg, int *callback_id)
{
    if (!callback || !callback_id)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    for (int i = 0; i < VITA_NETCTL_MAX_CALLBACKS; i++) {
        if (table[i].used)
            continue;
        table[i].used = true;
        table[i].callback = callback;
        table[i].arg = arg;
        table[i].id = g_next_callback_id;
        *callback_id = g_next_callback_id++;
        return 0;
    }

    return SCE_NET_CTL_ERROR_CALLBACK_MAX;
}

static int unregister_callback(VitaNetCtlCallbackEntry *table, int callback_id)
{
    if (callback_id <= 0)
        return SCE_NET_CTL_ERROR_INVALID_ID;

    for (int i = 0; i < VITA_NETCTL_MAX_CALLBACKS; i++) {
        if (table[i].used && table[i].id == callback_id) {
            memset(&table[i], 0, sizeof(table[i]));
            return 0;
        }
    }

    return SCE_NET_CTL_ERROR_INVALID_ID;
}

EXPORT(SceNetCtl, 0xEAEE6185, int, sceNetCtlInetRegisterCallback, SceNetCtlCallback callback,
       void *arg, int *callback_id)
{
    return register_callback(g_inet_callbacks, callback, arg, callback_id);
}

EXPORT(SceNetCtl, 0xD0C3BF3F, int, sceNetCtlInetUnregisterCallback, int callback_id)
{
    return unregister_callback(g_inet_callbacks, callback_id);
}

EXPORT(SceNetCtl, 0xFFA9D594, int, sceNetCtlAdhocRegisterCallback, SceNetCtlCallback callback,
       void *arg, int *callback_id)
{
    return register_callback(g_adhoc_callbacks, callback, arg, callback_id);
}

EXPORT(SceNetCtl, 0xA4471E10, int, sceNetCtlAdhocUnregisterCallback, int callback_id)
{
    return unregister_callback(g_adhoc_callbacks, callback_id);
}

/* ---------------------------------------------------------------- adhoc */

EXPORT(SceNetCtl, 0x0961A561, int, sceNetCtlAdhocGetState, int *state)
{
    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    if (!state)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    *state = SCE_NETCTL_STATE_DISCONNECTED;
    return 0;
}

EXPORT(SceNetCtl, 0xED43B79A, int, sceNetCtlAdhocDisconnect, void)
{
    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;

    return 0;
}

EXPORT(SceNetCtl, 0x77586C59, int, sceNetCtlAdhocGetPeerList, unsigned int *buffer_length,
       void *buffer)
{
    (void)buffer;

    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    if (!buffer_length)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    *buffer_length = 0;
    return 0;
}

EXPORT(SceNetCtl, 0x7118C99D, int, sceNetCtlAdhocGetInAddr, SceNetInAddr *inaddr)
{
    if (!g_netctl_initialized)
        return SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    if (!inaddr)
        return SCE_NET_CTL_ERROR_INVALID_ADDR;

    memset(inaddr, 0, sizeof(*inaddr));
    return 0;
}

DECLARE_LIBRARY(SceNetCtl, 0xC6083AE1);
