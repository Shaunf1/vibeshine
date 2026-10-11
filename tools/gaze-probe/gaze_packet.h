/**
 * @file tools/gaze-probe/gaze_packet.h
 * @brief Wire format shared by gaze_probe (headset) and gaze_listener.py (PC).
 *
 * All fields are little-endian. The probe sends one sample packet per gaze
 * sample. The listener answers each sample with an ack so the probe can measure
 * round-trip time without synchronised clocks.
 */
#pragma once

#include <stdint.h>

#define GAZE_SAMPLE_MAGIC "GZP1"
#define GAZE_ACK_MAGIC "GZA1"
#define GAZE_PACKET_VERSION 1

/* flags */
#define GAZE_FLAG_VALID 0x0001u  ///< isActive and orientation valid and tracked.
#define GAZE_FLAG_FOCUSED 0x0002u  ///< The OpenXR session was FOCUSED for this sample.
#define GAZE_FLAG_SAMPLE_TIME 0x0004u  ///< xr_sample_time came from XrEyeGazeSampleTimeEXT.

#pragma pack(push, 1)

/** Sample packet, 48 bytes. */
typedef struct gaze_sample_packet_t {
  char magic[4];  ///< "GZP1"
  uint16_t version;  ///< GAZE_PACKET_VERSION
  uint16_t flags;  ///< GAZE_FLAG_*
  uint32_t seq;  ///< Increments by one per packet. Gaps mean loss.
  uint32_t rtt_us;  ///< Latest round trip measured from acks, 0 when unknown.
  uint64_t send_time_ns;  ///< Sender CLOCK_REALTIME at send, ns since the Unix epoch.
  int64_t xr_sample_time;  ///< XrTime of the gaze sample (runtime clock, ns).
  float dir[3];  ///< Unit gaze direction in the reference space (-Z is straight ahead).
  float reserved;  ///< Zero.
} gaze_sample_packet_t;

/** Ack packet sent back by the listener, 16 bytes. */
typedef struct gaze_ack_packet_t {
  char magic[4];  ///< "GZA1"
  uint32_t seq;  ///< seq of the acked sample.
  uint64_t echo_send_time_ns;  ///< send_time_ns copied from the sample.
} gaze_ack_packet_t;

#pragma pack(pop)

_Static_assert(sizeof(gaze_sample_packet_t) == 48, "sample packet size");
_Static_assert(sizeof(gaze_ack_packet_t) == 16, "ack packet size");
