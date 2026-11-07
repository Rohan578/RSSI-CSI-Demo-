/*
 * ============================================================================
 *  espnow_packet.h  -  The two small ESP-NOW packets in the air
 * ============================================================================
 *
 *  !!! All three sketches (csi_transmitter, csi_receiver_24, csi_receiver_5g)
 *  !!! have an IDENTICAL copy of this file. If you change one, copy it to
 *  !!! the other two.
 *
 *  1. tx_packet_t     transmitter -> air, SEND_RATE_HZ times per second
 *     Its real job is just to EXIST: the receiver measures CSI on the radio
 *     signal itself, not on these bytes. The contents only help check data:
 *       node_id       which transmitter sent it (also in the MAC address)
 *       seq           +1 per packet, so lost packets can be counted
 *       tx_time_us    when it was sent, by the transmitter's own clock
 *
 *  2. rx_beacon_t     2.4 GHz receiver -> air, BEACON_RATE_HZ times per second
 *     "I'm a receiver, I'm listening on channel N." Transmitters lock onto
 *     the channel where they hear this, and search again if it goes quiet.
 *     That way they follow the home router when it changes its 2.4 GHz
 *     channel by itself, instead of going silent until power-cycled.
 *
 *  Both are tiny so they use as little airtime as possible.
 * ============================================================================
 */
#pragma once
#include <stdint.h>

/* --- 1. Transmitter packet ------------------------------------------------- */
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

/* --- 2. Receiver beacon ----------------------------------------------------- */
#define RX_BEACON_MAGIC    0xC5B0   /* Marks a receiver beacon */
#define RX_BEACON_VERSION  1

typedef struct __attribute__((packed)) {
    uint16_t magic;        /* Always RX_BEACON_MAGIC */
    uint8_t  version;      /* Always RX_BEACON_VERSION */
    uint8_t  receiver_id;  /* RECEIVER_ID of the receiver sending it */
    uint8_t  channel;      /* The channel the receiver is on right now */
    uint8_t  reserved[3];  /* Always 0 */
} rx_beacon_t;

static_assert(sizeof(rx_beacon_t) == 8, "rx_beacon_t must be 8 bytes");
