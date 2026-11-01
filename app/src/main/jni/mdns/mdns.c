/*
 * mdns.c  –  Minimal mDNS / DNS-SD multicast socket helpers
 *
 * Lifecycle:
 *   mdns_init()      – open socket, join 224.0.0.251
 *   mdns_announce()  – send 3 gratuitous announcements (RFC 6762 §8.3)
 *   mdns_loop()      – receive loop (dedicated thread)
 *   mdns_stop()      – teardown
 */

#include "mdns.h"
#include "service_announce.h"

#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <android/log.h>

#define TAG "mDNS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct mdns_ctx {
    int    sock;
    int    running;
    char   iface_addr[64];

    /* saved for re-announce on query */
    char   hostname[64];
    uint8_t hw_addr[6];
    uint16_t port;
};

/* ───────────────── helpers ───────────────── */

static int open_mdns_socket(const char *iface_addr) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { LOGE("socket: %s", strerror(errno)); return -1; }

    /* Allow multiple sockets to bind to 5353 (SO_REUSEADDR) */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* Bind to INADDR_ANY:5353 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("bind: %s", strerror(errno));
        close(fd); return -1;
    }

    /* Join multicast group on the Wi-Fi interface */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
    mreq.imr_interface.s_addr = inet_addr(iface_addr);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        LOGE("IP_ADD_MEMBERSHIP: %s", strerror(errno));
        /* Non-fatal – we can still send; queries from iOS just won't arrive */
    }

    /* Set multicast TTL = 255 and loop = 0 (don't receive our own packets) */
    uint8_t ttl = 255, loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,  &ttl,  sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    /* Outgoing multicast via our iface */
    struct in_addr iface;
    iface.s_addr = inet_addr(iface_addr);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));

    LOGI("mDNS socket fd=%d on iface %s", fd, iface_addr);
    return fd;
}

/* ───────────────── public API ───────────────── */

mdns_ctx_t *mdns_init(const char *iface_addr) {
    mdns_ctx_t *ctx = (mdns_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->sock = open_mdns_socket(iface_addr);
    if (ctx->sock < 0) { free(ctx); return NULL; }

    strncpy(ctx->iface_addr, iface_addr, sizeof(ctx->iface_addr) - 1);
    ctx->running = 1;
    return ctx;
}

void mdns_announce(mdns_ctx_t *ctx,
                   const char *hostname,
                   const uint8_t hw_addr[6],
                   uint16_t port)
{
    if (!ctx || ctx->sock < 0) return;

    /* Save for later re-announce */
    strncpy(ctx->hostname, hostname, sizeof(ctx->hostname) - 1);
    memcpy(ctx->hw_addr, hw_addr, 6);
    ctx->port = port;

    /* Send 3 gratuitous announcements with 1-second spacing (RFC 6762 §8.3) */
    for (int i = 0; i < 3; i++) {
        service_announce_send(ctx->sock, hostname, hw_addr, port, ctx->iface_addr, NULL);
        if (i < 2) sleep(1);
    }
    LOGI("mDNS announced '%s' on port %u", hostname, port);
}

void mdns_loop(mdns_ctx_t *ctx) {
    if (!ctx || ctx->sock < 0) return;

    uint8_t buf[MDNS_MAX_PACKET];
    struct sockaddr_in sender;
    socklen_t slen = sizeof(sender);
    int heartbeat_ticks = 0;

    LOGI("mDNS receive loop started (with 30s heartbeat re-announcement)");
    while (ctx->running) {
        struct pollfd pfd = { .fd = ctx->sock, .events = POLLIN };
        int pr = poll(&pfd, 1, 5000); /* 5 second timeout */
        if (!ctx->running) break;

        if (pr > 0 && (pfd.revents & POLLIN)) {
            slen = sizeof(sender);
            ssize_t len = recvfrom(ctx->sock, buf, sizeof(buf), 0,
                                   (struct sockaddr *)&sender, &slen);
            if (len > 0) {
                /* Reply directly to sender (unicast) AND multicast */
                service_announce_handle_query(ctx->sock,
                                              buf, (size_t)len,
                                              ctx->hostname,
                                              ctx->hw_addr,
                                              ctx->port,
                                              ctx->iface_addr,
                                              &sender);
            }
        }

        if (pr == 0) {
            /* 5-second poll timeout with no traffic: trigger 30s heartbeat */
            if (++heartbeat_ticks >= 6) {
                heartbeat_ticks = 0;
                if (ctx->hostname[0] != '\0') {
                    service_announce_send(ctx->sock, ctx->hostname, ctx->hw_addr,
                                          ctx->port, ctx->iface_addr, NULL);
                }
            }
        }
    }
    LOGI("mDNS receive loop exited");
}

void mdns_stop(mdns_ctx_t *ctx) {
    if (!ctx) return;
    ctx->running = 0;
    if (ctx->sock >= 0) {
        shutdown(ctx->sock, SHUT_RDWR);
        close(ctx->sock);
        ctx->sock = -1;
    }
    free(ctx);
}
