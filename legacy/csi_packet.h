/*
 * ============================================================================
 *  csi_packet.h  -  The UDP packet format sent from the S3 to the Pi
 * ============================================================================
 *
 *  !!! If you change anything here, make the same change in
 *  !!! pi_server/server.py (SECTION 2: PACKET FORMAT), or the Pi can't
 *  !!! read the data.
 *
 *  One UDP packet = one BATCH of CSI samples:
 *
 *    +----------------------+
 *    | batch header (12 B)  |
 *    +----------------------+
 *    | sample header (16 B) |  <- sample 1 (e.g. from node 3)
 *    | CSI bytes (csi_len)  |
 *    +----------------------+
 *    | sample header (16 B) |  <- sample 2 (e.g. from node 20)
 *    | CSI bytes (csi_len)  |
 *    +----------------------+
 *    | ...                  |
 *
 *  All numbers are little-endian (the ESP32-S3 and the Pi 5 both are,
 *  so no conversion is needed).
 *
 *  CSI bytes are signed 8-bit numbers in pairs, [imaginary, real] for each
 *  subcarrier, exactly as Espressif's WiFi driver gives them.
 * ============================================================================
 */
#pragma once
#include <stdint.h>

#define CSI_PACKET_MAGIC    0xC51A   /* Marks a UDP packet as ours */
#define CSI_PACKET_VERSION  2        /* v2 = one receiver, many transmitters */
#define CSI_MAX_LEN         612      /* Largest CSI buffer accepted (bytes) */

/* Batch header - once at the start of every UDP packet (12 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t magic;          /* Always CSI_PACKET_MAGIC */
    uint8_t  version;        /* Always CSI_PACKET_VERSION */
    uint8_t  receiver_id;    /* RECEIVER_ID from config.h */
    uint32_t batch_seq;      /* +1 per UDP packet (spot lost UDP packets) */
    uint8_t  sample_count;   /* How many samples follow */
    uint8_t  reserved[3];    /* Unused, always 0 */
} csi_batch_header_t;

/* Sample header - before each CSI sample (16 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  tx_node_id;         /* Which transmitter sent the packet */
    int8_t   rssi;               /* Signal strength at the S3, dBm */
    int8_t   noise_floor;        /* Background noise, dBm */
    uint8_t  channel;            /* WiFi channel */
    uint8_t  rx_format;          /* Packet type: 0 = 11b/g, 1 = 11n (HT), ... */
    uint8_t  first_word_invalid; /* 1 = first 4 CSI bytes are unreliable */
    uint16_t csi_len;            /* Number of CSI bytes that follow */
    uint32_t tx_seq;             /* Transmitter's packet counter (0 = unknown) */
    uint32_t rx_timestamp_us;    /* When the S3 received it (S3 radio clock) */
} csi_sample_header_t;

static_assert(sizeof(csi_batch_header_t) == 12, "batch header must be 12 bytes");
static_assert(sizeof(csi_sample_header_t) == 16, "sample header must be 16 bytes");
