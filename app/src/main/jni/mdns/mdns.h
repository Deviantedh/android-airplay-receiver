/*
 * mdns.h  –  Minimal mDNS / DNS-SD helpers
 *
 * We implement only what we need:
 *   - Open a multicast UDP socket on 224.0.0.251:5353
 *   - Send mDNS announcements (PTR + SRV + TXT) for _raop._tcp and _airplay._tcp
 *   - Reply to incoming mDNS queries for our service names
 *
 * This is NOT a full mDNS stack. For a complete implementation we will later
 * integrate mjansson/mdns (single-header, BSD license, no deps).
 */
#pragma once

#include <stdint.h>

#define MDNS_PORT        5353
#define MDNS_ADDR        "224.0.0.251"
#define MDNS_MAX_PACKET  1400   /* conservative – under typical Wi-Fi MTU */

typedef struct mdns_ctx mdns_ctx_t;

/**
 * mdns_init – create multicast socket, join group on the given interface.
 * @param iface_addr  IPv4 address string of the Wi-Fi interface (e.g. "192.168.1.42").
 * @returns opaque context or NULL on error.
 */
mdns_ctx_t *mdns_init(const char *iface_addr);

/**
 * mdns_announce – send unsolicited PTR/SRV/TXT announcements for both
 *                 _raop._tcp.local and _airplay._tcp.local.
 * @param ctx       context from mdns_init.
 * @param hostname  device hostname without .local (e.g. "SplendidAirPlay").
 * @param hw_addr   6-byte MAC address (used as AirPlay device ID prefix).
 * @param port      TCP port the RTSP server is listening on (default 5000).
 */
void mdns_announce(mdns_ctx_t *ctx,
                   const char *hostname,
                   const uint8_t hw_addr[6],
                   uint16_t port);

/**
 * mdns_loop – blocking receive loop; replies to queries. Call from a
 *             dedicated thread. Returns when mdns_stop() is called.
 */
void mdns_loop(mdns_ctx_t *ctx);

/** Signal the loop to exit and free all resources. */
void mdns_stop(mdns_ctx_t *ctx);
