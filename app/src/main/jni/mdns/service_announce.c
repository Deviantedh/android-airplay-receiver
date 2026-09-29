/*
 * service_announce.c  –  DNS-SD packet builder for AirPlay 1 (RAOP)
 *
 * Configured as authentic Apple AirPort Express (AirPort4,107) audio receiver.
 * Advertises exclusively _raop._tcp (pure audio, no video/AirPlay 2 conflict).
 */

#include "mdns.h"
#include "service_announce.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <android/log.h>

#define TAG "SA"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ── DNS wire-format helpers ─────────────────────────────────────────────── */

typedef struct {
    uint8_t  *buf;
    size_t    cap;
    size_t    len;
} pkt_t;

static void pkt_init(pkt_t *p, uint8_t *buf, size_t cap) {
    p->buf = buf; p->cap = cap; p->len = 0;
}

static int pkt_u8(pkt_t *p, uint8_t v) {
    if (p->len + 1 > p->cap) return -1;
    p->buf[p->len++] = v;
    return 0;
}
static int pkt_u16(pkt_t *p, uint16_t v) {
    if (p->len + 2 > p->cap) return -1;
    p->buf[p->len++] = (v >> 8) & 0xFF;
    p->buf[p->len++] =  v       & 0xFF;
    return 0;
}
static int pkt_u32(pkt_t *p, uint32_t v) {
    pkt_u16(p, v >> 16); return pkt_u16(p, v & 0xFFFF);
}
static int pkt_bytes(pkt_t *p, const void *d, size_t n) {
    if (p->len + n > p->cap) return -1;
    memcpy(p->buf + p->len, d, n);
    p->len += n;
    return 0;
}

/* Encode DNS label sequence: "foo.bar.baz" → 3foo3bar3baz0 */
static int pkt_name(pkt_t *p, const char *name) {
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t llen = dot ? (size_t)(dot - name) : strlen(name);
        if (pkt_u8(p, (uint8_t)llen) < 0) return -1;
        if (pkt_bytes(p, name, llen)  < 0) return -1;
        name += llen;
        if (*name == '.') name++;
    }
    return pkt_u8(p, 0);
}

static size_t pkt_rdlen_placeholder(pkt_t *p) {
    size_t off = p->len;
    pkt_u16(p, 0);
    return off;
}
static void pkt_rdlen_fill(pkt_t *p, size_t off) {
    uint16_t len = (uint16_t)(p->len - off - 2);
    p->buf[off]   = (len >> 8) & 0xFF;
    p->buf[off+1] =  len       & 0xFF;
}

static void pkt_rr_hdr(pkt_t *p, const char *name,
                       uint16_t type, uint32_t ttl, int flush) {
    pkt_name(p, name);
    pkt_u16(p, type);
    pkt_u16(p, flush ? 0x8001 : 0x0001);   /* IN class, cache-flush bit */
    pkt_u32(p, ttl);
}

static void pkt_txt_kv(pkt_t *p, const char *kv) {
    uint8_t l = (uint8_t)strlen(kv);
    pkt_u8(p, l);
    pkt_bytes(p, kv, l);
}

#define DNS_HDR_SIZE  12
static void pkt_dns_header(pkt_t *p, uint16_t ancount, uint16_t arcount) {
    pkt_u16(p, 0x0000);              /* ID = 0 (mDNS) */
    pkt_u16(p, 0x8400);              /* QR=1 AA=1 */
    pkt_u16(p, 0);                   /* QDCOUNT */
    pkt_u16(p, ancount);             /* ANCOUNT */
    pkt_u16(p, 0);                   /* NSCOUNT */
    pkt_u16(p, arcount);             /* ARCOUNT */
}

/* ── RAOP TXT records matching authentic AirPort Express ─────────────────── */

static void append_raop_txt(pkt_t *p) {
    pkt_txt_kv(p, "txtvers=1");
    pkt_txt_kv(p, "ch=2");
    pkt_txt_kv(p, "cn=0,1");
    pkt_txt_kv(p, "et=0,1");
    pkt_txt_kv(p, "sv=false");
    pkt_txt_kv(p, "da=true");
    pkt_txt_kv(p, "sr=44100");
    pkt_txt_kv(p, "ss=16");
    pkt_txt_kv(p, "pw=false");
    pkt_txt_kv(p, "vn=65537");
    pkt_txt_kv(p, "tp=TCP,UDP");
    pkt_txt_kv(p, "md=0,1,2");
    pkt_txt_kv(p, "vs=105.1");
    pkt_txt_kv(p, "am=AirPort4,107");
    pkt_txt_kv(p, "sf=0x4");
}

/* ── Send helper ─────────────────────────────────────────────────────────── */

static void send_pkt(int sock, const uint8_t *buf, size_t len) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(MDNS_PORT);
    dst.sin_addr.s_addr = inet_addr(MDNS_ADDR);

    sendto(sock, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ── Build & send announcement ──────────────────────────────────────────── */

void service_announce_send(int sock,
                           const char *hostname,
                           const uint8_t hw_addr[6],
                           uint16_t port,
                           const char *iface_addr)
{
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             hw_addr[0], hw_addr[1], hw_addr[2],
             hw_addr[3], hw_addr[4], hw_addr[5]);

    /* Display name on Apple devices is whatever follows the '@' */
    char raop_instance[128];
    snprintf(raop_instance, sizeof(raop_instance),
             "%s@%s._raop._tcp.local", mac_str, hostname);

    /* Use a non-reserved device hostname for SRV & A records */
    char local_host[128];
    snprintf(local_host, sizeof(local_host), "GalaxyMini.local");

    uint8_t buf[1400];
    pkt_t p;
    pkt_init(&p, buf, sizeof(buf));
    pkt_dns_header(&p, 3, 1);   /* 3 answers (PTR, SRV, TXT) + 1 additional (A) */

    /* 1. PTR: _raop._tcp.local → <mac>@AirPlay._raop._tcp.local */
    pkt_rr_hdr(&p, "_raop._tcp.local", 12 /*PTR*/, 4500, 0);
    size_t rdl = pkt_rdlen_placeholder(&p);
    pkt_name(&p, raop_instance);
    pkt_rdlen_fill(&p, rdl);

    /* 2. SRV: <mac>@AirPlay._raop._tcp.local → GalaxyMini.local:5000 */
    pkt_rr_hdr(&p, raop_instance, 33 /*SRV*/, 4500, 1);
    rdl = pkt_rdlen_placeholder(&p);
    pkt_u16(&p, 0); pkt_u16(&p, 0); /* priority, weight */
    pkt_u16(&p, port);
    pkt_name(&p, local_host);
    pkt_rdlen_fill(&p, rdl);

    /* 3. TXT: RAOP parameters */
    pkt_rr_hdr(&p, raop_instance, 16 /*TXT*/, 4500, 1);
    rdl = pkt_rdlen_placeholder(&p);
    append_raop_txt(&p);
    pkt_rdlen_fill(&p, rdl);

    /* 4. Additional: A record GalaxyMini.local → iface IP */
    pkt_rr_hdr(&p, local_host, 1 /*A*/, 4500, 1);
    rdl = pkt_rdlen_placeholder(&p);
    struct in_addr ia;
    inet_aton(iface_addr, &ia);
    pkt_bytes(&p, &ia.s_addr, 4);
    pkt_rdlen_fill(&p, rdl);

    send_pkt(sock, buf, p.len);
    LOGI("Sent RAOP mDNS announcement for '%s' -> %s:%d", hostname, local_host, port);
}


/* ── Query handler ───────────────────────────────────────────────────────── */

void service_announce_handle_query(int sock,
                                   const uint8_t *pkt, size_t len,
                                   const char *hostname,
                                   const uint8_t hw_addr[6],
                                   uint16_t port,
                                   const char *iface_addr)
{
    if (len < DNS_HDR_SIZE) return;
    if (pkt[2] & 0x80) return; /* ignore responses */

    int found = 0;

    /*
     * In DNS wire format domain labels are length-prefixed:
     * e.g. \x05_raop\x04_tcp\x05local\x00 (no ASCII dot!).
     * Checking substring matches on "_raop" and "_airplay" reliably catches
     * all standard Apple AirPlay/RAOP service discovery queries.
     */
    for (size_t i = DNS_HDR_SIZE; i + 4 < len; i++) {
        if (i + 5 <= len && memcmp(pkt + i, "_raop", 5) == 0) {
            found = 1; break;
        }
        if (i + 8 <= len && memcmp(pkt + i, "_airplay", 8) == 0) {
            found = 1; break;
        }
        if (hostname && hostname[0] && i + strlen(hostname) <= len &&
            memcmp(pkt + i, hostname, strlen(hostname)) == 0) {
            found = 1; break;
        }
        if (i + 10 <= len && memcmp(pkt + i, "GalaxyMini", 10) == 0) {
            found = 1; break;
        }
    }

    if (found) {
        LOGI("mDNS query match – sending RAOP response");
        service_announce_send(sock, hostname, hw_addr, port, iface_addr);
    }
}
