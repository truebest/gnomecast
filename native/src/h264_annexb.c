#include "h264_annexb.h"

#include <string.h>

#include "clog.h"

clog_define(g_native_log_h264, cLogLevelInfo, cLogFlags_Default, "video.h264", NULL);

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void scan_nal_type(uint8_t nal_header, NativeH264Info *info) {
    uint8_t nal_type = nal_header & 0x1f;
    if (nal_type == 7) {
        info->has_sps = true;
    } else if (nal_type == 8) {
        info->has_pps = true;
    } else if (nal_type == 5) {
        info->has_idr = true;
    }
}

NativeH264Result native_h264_avc_annexb_size(const uint8_t *data, size_t len, NativeH264Info *info, size_t *out_len) {
    if (!data || !out_len) {
        clog_limited(cLogLevelWarning, 2, 5000, "invalid AVC scan arguments (data=%s out_len=%s)",
                     data ? "set" : "NULL", out_len ? "set" : "NULL");
        return NATIVE_H264_INVALID;
    }

    NativeH264Info local_info;
    NativeH264Info *dst_info = info ? info : &local_info;
    memset(dst_info, 0, sizeof(*dst_info));
    *out_len = 0;

    size_t pos = 0;
    while (pos < len) {
        if (len - pos < 4) {
            clog_limited(cLogLevelDebug, 2, 5000, "AVC probe rejected %zu-byte AU: truncated length at offset %zu",
                         len, pos);
            return NATIVE_H264_INVALID;
        }
        uint32_t nal_len = read_be32(data + pos);
        pos += 4;
        if (nal_len == 0 || nal_len > len - pos) {
            clog_limited(cLogLevelDebug, 2, 5000,
                         "AVC probe rejected %zu-byte AU: invalid NAL length %u at offset %zu", len,
                         (unsigned)nal_len, pos - 4);
            return NATIVE_H264_INVALID;
        }
        if (SIZE_MAX - *out_len < 4 || SIZE_MAX - *out_len - 4 < nal_len) {
            clog_limited(cLogLevelWarning, 2, 5000, "AVC-to-Annex-B size overflow for %zu-byte AU", len);
            return NATIVE_H264_INVALID;
        }
        scan_nal_type(data[pos], dst_info);
        dst_info->nal_count++;
        *out_len += 4 + (size_t)nal_len;
        pos += nal_len;
    }
    if (!dst_info->nal_count) {
        clog_limited(cLogLevelDebug, 2, 5000, "AVC probe rejected empty access unit");
        return NATIVE_H264_INVALID;
    }
    return NATIVE_H264_OK;
}

NativeH264Result native_h264_scan_avc(const uint8_t *data, size_t len, NativeH264Info *info) {
    if (!info) {
        clog_limited(cLogLevelWarning, 2, 5000, "invalid AVC scan arguments (info=NULL)");
        return NATIVE_H264_INVALID;
    }

    size_t out_len = 0;
    return native_h264_avc_annexb_size(data, len, info, &out_len);
}

static bool annexb_prefix_at(const uint8_t *data, size_t len, size_t pos, size_t *prefix_len) {
    if (pos + 3 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01) {
        *prefix_len = 3;
        return true;
    }
    if (pos + 4 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x00 &&
        data[pos + 3] == 0x01) {
        *prefix_len = 4;
        return true;
    }
    return false;
}

static bool find_annexb_prefix(const uint8_t *data, size_t len, size_t pos, size_t *prefix_pos, size_t *prefix_len) {
    while (pos + 3 <= len) {
        if (annexb_prefix_at(data, len, pos, prefix_len)) {
            *prefix_pos = pos;
            return true;
        }
        pos++;
    }
    return false;
}

NativeH264Result native_h264_scan_annexb(const uint8_t *data, size_t len, NativeH264Info *info) {
    if (!data || !info) {
        clog_limited(cLogLevelWarning, 2, 5000, "invalid Annex-B scan arguments (data=%s info=%s)",
                     data ? "set" : "NULL", info ? "set" : "NULL");
        return NATIVE_H264_INVALID;
    }

    memset(info, 0, sizeof(*info));

    size_t pos = 0;
    size_t prefix_pos = 0;
    size_t prefix_len = 0;
    while (find_annexb_prefix(data, len, pos, &prefix_pos, &prefix_len)) {
        size_t nal_start = prefix_pos + prefix_len;
        size_t next_prefix_pos = len;
        size_t next_prefix_len = 0;
        if (find_annexb_prefix(data, len, nal_start, &next_prefix_pos, &next_prefix_len)) {
            (void)next_prefix_len;
        }

        if (nal_start < next_prefix_pos) {
            scan_nal_type(data[nal_start], info);
            info->nal_count++;
        }

        pos = next_prefix_pos;
    }

    if (!info->nal_count) {
        clog_limited(cLogLevelDebug, 2, 5000, "Annex-B probe found no NAL units in %zu-byte AU", len);
        return NATIVE_H264_INVALID;
    }
    return NATIVE_H264_OK;
}

NativeH264Result native_h264_avc_to_annexb(const uint8_t *data, size_t len, uint8_t *out, size_t out_cap, size_t *out_len) {
    static const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};

    if (!data || !out || !out_len) {
        clog_limited(cLogLevelWarning, 2, 5000, "invalid AVC conversion arguments (data=%s out=%s out_len=%s)",
                     data ? "set" : "NULL", out ? "set" : "NULL", out_len ? "set" : "NULL");
        return NATIVE_H264_INVALID;
    }
    *out_len = 0;

    size_t needed = 0;
    NativeH264Result size_result = native_h264_avc_annexb_size(data, len, NULL, &needed);
    if (size_result != NATIVE_H264_OK) {
        return size_result;
    }
    if (needed > out_cap) {
        clog_limited(cLogLevelWarning, 2, 5000, "AVC conversion needs %zu bytes, output capacity is %zu", needed,
                     out_cap);
        return NATIVE_H264_OUTPUT_TOO_SMALL;
    }

    size_t pos = 0;
    while (pos < len) {
        uint32_t nal_len = read_be32(data + pos);
        pos += 4;
        memcpy(out + *out_len, start_code, sizeof(start_code));
        *out_len += sizeof(start_code);
        memcpy(out + *out_len, data + pos, nal_len);
        *out_len += nal_len;
        pos += nal_len;
    }

    return *out_len ? NATIVE_H264_OK : NATIVE_H264_INVALID;
}

bool native_h264_annexb_is_keyframe(const uint8_t *data, size_t len) {
    if (!data || len < 4) {
        return false;
    }

    NativeH264Info info;
    if (native_h264_scan_annexb(data, len, &info) != NATIVE_H264_OK) {
        return false;
    }
    return info.has_idr && (info.has_sps || info.has_pps);
}
