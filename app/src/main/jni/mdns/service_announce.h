/*
 * service_announce.h  –  DNS-SD packet builder for _raop._tcp and _airplay._tcp
 *
 * Builds raw mDNS response packets according to RFC 6762 / RFC 6763.
 * No external dependencies – pure C, hand-coded DNS wire format.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * Build and send PTR + SRV + TXT records for both AirPlay service types.
 *
 * @param sock        mDNS UDP socket (already bound & joined).
 * @param hostname    device name without .local
 * @param hw_addr     6-byte MAC, used as "AABBCCDDEEFF@<hostname>" prefix
 * @param port        RTSP listening port
 * @param iface_addr  local IPv4 (used for A record in the additional section)
 */
void service_announce_send(int sock,
                           const char *hostname,
                           const uint8_t hw_addr[6],
                           uint16_t port,
                           const char *iface_addr);

/**
 * Inspect an incoming mDNS packet and send a unicast/multicast reply if it
 * queries one of our service names.
 */
void service_announce_handle_query(int sock,
                                   const uint8_t *pkt, size_t len,
                                   const char *hostname,
                                   const uint8_t hw_addr[6],
                                   uint16_t port,
                                   const char *iface_addr);
