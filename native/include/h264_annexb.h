#ifndef HELLOLG_H264_ANNEXB_H
#define HELLOLG_H264_ANNEXB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_H264_MAX_AU_BYTES ((size_t)32 * 1024 * 1024)

typedef enum NativeH264Result {
    NATIVE_H264_OK = 0,
    NATIVE_H264_INVALID = 1,
    NATIVE_H264_OUTPUT_TOO_SMALL = 2,
} NativeH264Result;

typedef struct NativeH264Info {
    bool has_sps;
    bool has_pps;
    bool has_idr;
    size_t nal_count;
} NativeH264Info;

NativeH264Result native_h264_scan_avc(const uint8_t *data, size_t len, NativeH264Info *info);
NativeH264Result native_h264_scan_annexb(const uint8_t *data, size_t len, NativeH264Info *info);
NativeH264Result native_h264_avc_annexb_size(const uint8_t *data, size_t len, NativeH264Info *info, size_t *out_len);
NativeH264Result native_h264_avc_to_annexb(const uint8_t *data, size_t len, uint8_t *out, size_t out_cap, size_t *out_len);
bool native_h264_annexb_is_keyframe(const uint8_t *data, size_t len);

#endif
