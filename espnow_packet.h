/*
 * ============================================================================
 *  espnow_packet.h  -  What is inside each packet a transmitter sends
 * ============================================================================
 *
 *  !!! The S3 receiver sketch has an identical copy of this file.
 *  !!! If you change one, change the other.
 *
 *  The packet's real job is just to EXIST: the S3 measures CSI on the
 *  radio signal itself, not on these bytes. The contents only help check
 *  the data:
 *
 *    node_id       which transmitter sent it (also in the MAC address)
 *    seq           counts up by 1 per packet, so lost packets can be spotted
 *    tx_time_us    when it was sent, by this board's own clock
 *
 *  12 bytes total, kept tiny so each packet uses as little airtime as
 *  possible.
 * ============================================================================
 */
#pragma once
#include <stdint.h>

#define TX_PACKET_MAGIC    0xC5A1   /* Marks the packet as one of ours */
#define TX_PACKET_VERSION  1        /* Bump this if the layout changes */

typedef struct __attribute__((packed)) {
    uint16_t magic;        /* Always TX_PACKET_MAGIC */
    uint8_t  version;      /* Always TX_PACKET_VERSION */
    uint8_t  node_id;      /* NODE_ID from config.h */
    uint32_t seq;          /* Packet counter, starts at 1 after each boot */
    uint32_t tx_time_us;   /* Microseconds since this board booted */
} tx_packet_t;

static_assert(sizeof(tx_packet_t) == 12, "tx_packet_t must be 12 bytes");
