#ifndef TB330FU_VIDEO_RING_H
#define TB330FU_VIDEO_RING_H

#include <stdint.h>
#include <stddef.h>

#define TB330FU_VIDEO_RING_MAGIC "TBV3RING"
#define TB330FU_VIDEO_RING_VERSION 1u
#define TB330FU_VIDEO_RING_PATH "/run/tb330fu-camera/video-ring.shm"
#define TB330FU_VIDEO_RECORDER_SOCKET "/run/tb330fu-camera/video-recorder.sock"
#define TB330FU_VIDEO_OUTPUT "/var/lib/tb330fu-camera/video.mp4"
#define TB330FU_VIDEO_OUTPUT_PART "/var/lib/tb330fu-camera/video.mp4.part"
#define TB330FU_VIDEO_RING_SLOTS 4u
#define TB330FU_VIDEO_RING_SLOT_BYTES (1920u * 1080u * 3u / 2u)
#define TB330FU_VIDEO_LAYOUT_MAPPER_Y_VU 1u
#define TB330FU_VIDEO_TIMESTAMP_MONOTONIC_NS 1u

struct tb330fu_video_ring_header {
    char magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint32_t slot_count;
    uint32_t slot_bytes;
    uint64_t slots_offset;
    uint64_t write_sequence;
    uint64_t read_sequence;
    uint64_t frames_dropped;
    uint32_t timestamp_domain;
    uint32_t reserved;
};

struct tb330fu_video_plane {
    uint64_t offset;
    uint64_t payload_bytes;
    uint32_t row_stride;
    uint32_t sample_stride;
};

struct tb330fu_video_slot {
    uint64_t sequence;
    uint64_t capture_timestamp_ns;
    uint32_t width;
    uint32_t height;
    uint32_t layout;
    uint32_t plane_count;
    struct tb330fu_video_plane planes[2];
    uint8_t payload[];
};

static constexpr size_t tb330fu_video_slot_bytes(uint32_t payload_bytes) {
    return sizeof(struct tb330fu_video_slot) + payload_bytes;
}

#endif
