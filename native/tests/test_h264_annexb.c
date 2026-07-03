#include "h264_annexb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int expect_true(int condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

static size_t append_nal(uint8_t *dst, size_t pos, const uint8_t *payload, size_t payload_len) {
    dst[pos++] = (uint8_t)((payload_len >> 24) & 0xff);
    dst[pos++] = (uint8_t)((payload_len >> 16) & 0xff);
    dst[pos++] = (uint8_t)((payload_len >> 8) & 0xff);
    dst[pos++] = (uint8_t)(payload_len & 0xff);
    memcpy(dst + pos, payload, payload_len);
    return pos + payload_len;
}

static size_t build_one_byte_avc(uint8_t *dst, const uint8_t *nal_headers, size_t nal_count) {
    size_t pos = 0;
    for (size_t i = 0; i < nal_count; i++) {
        pos = append_nal(dst, pos, &nal_headers[i], 1);
    }
    return pos;
}

static int expect_scan_case(const char *name, const uint8_t *nal_headers, size_t nal_count, bool has_sps, bool has_pps, bool has_idr, bool keyframe) {
    uint8_t avc[64];
    uint8_t annexb[64];
    size_t avc_len = build_one_byte_avc(avc, nal_headers, nal_count);
    size_t annexb_len = 0;
    NativeH264Info info;
    int failures = 0;

    failures += expect_true(native_h264_scan_avc(avc, avc_len, &info) == NATIVE_H264_OK, name);
    failures += expect_true(info.nal_count == nal_count, "matrix nal count");
    failures += expect_true(info.has_sps == has_sps, "matrix SPS flag");
    failures += expect_true(info.has_pps == has_pps, "matrix PPS flag");
    failures += expect_true(info.has_idr == has_idr, "matrix IDR flag");
    failures += expect_true(native_h264_avc_to_annexb(avc, avc_len, annexb, sizeof(annexb), &annexb_len) == NATIVE_H264_OK, "matrix convert");
    failures += expect_true(annexb_len == nal_count * 5, "matrix Annex-B length");
    failures += expect_true(native_h264_annexb_is_keyframe(annexb, annexb_len) == keyframe, "matrix keyframe flag");
    return failures;
}

static int test_sps_pps_idr_matrix(void) {
    int failures = 0;
    const uint8_t non_idr[] = {0x41};
    const uint8_t sps[] = {0x67};
    const uint8_t pps[] = {0x68};
    const uint8_t idr[] = {0x65};
    const uint8_t sps_pps[] = {0x67, 0x68};
    const uint8_t sps_idr[] = {0x67, 0x65};
    const uint8_t pps_idr[] = {0x68, 0x65};
    const uint8_t sps_pps_idr[] = {0x67, 0x68, 0x65};

    failures += expect_scan_case("matrix non-IDR", non_idr, 1, false, false, false, false);
    failures += expect_scan_case("matrix SPS", sps, 1, true, false, false, false);
    failures += expect_scan_case("matrix PPS", pps, 1, false, true, false, false);
    failures += expect_scan_case("matrix IDR", idr, 1, false, false, true, false);
    failures += expect_scan_case("matrix SPS+PPS", sps_pps, 2, true, true, false, false);
    failures += expect_scan_case("matrix SPS+IDR", sps_idr, 2, true, false, true, true);
    failures += expect_scan_case("matrix PPS+IDR", pps_idr, 2, false, true, true, true);
    failures += expect_scan_case("matrix SPS+PPS+IDR", sps_pps_idr, 3, true, true, true, true);
    return failures;
}

static int test_exact_capacity_and_bytes(void) {
    const uint8_t avc[] = {
        0x00, 0x00, 0x00, 0x02, 0x67, 0xaa,
        0x00, 0x00, 0x00, 0x02, 0x68, 0xbb,
        0x00, 0x00, 0x00, 0x03, 0x65, 0xcc, 0xdd,
    };
    const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0xaa,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xbb,
        0x00, 0x00, 0x00, 0x01, 0x65, 0xcc, 0xdd,
    };
    NativeH264Info info;
    uint8_t out[sizeof(expected)];
    uint8_t tiny[sizeof(expected) - 1];
    size_t out_len = 0;
    int failures = 0;

    failures += expect_true(native_h264_scan_avc(avc, sizeof(avc), &info) == NATIVE_H264_OK, "scan ok");
    failures += expect_true(info.nal_count == 3, "nal count");
    failures += expect_true(info.has_sps && info.has_pps && info.has_idr, "sps pps idr");

    size_t required_len = 0;
    failures += expect_true(native_h264_avc_annexb_size(avc, sizeof(avc), &info, &required_len) == NATIVE_H264_OK, "annexb size ok");
    failures += expect_true(required_len == sizeof(expected), "annexb size len");
    failures += expect_true(native_h264_avc_annexb_size(avc, sizeof(avc), NULL, &required_len) == NATIVE_H264_OK, "annexb size no info");
    failures += expect_true(required_len == sizeof(expected), "annexb size no info len");
    failures += expect_true(NATIVE_H264_MAX_AU_BYTES >= sizeof(avc), "au cap sane");

    failures += expect_true(native_h264_avc_to_annexb(avc, sizeof(avc), out, sizeof(out), &out_len) == NATIVE_H264_OK, "exact capacity convert");
    failures += expect_true(out_len == sizeof(expected), "annexb len");
    failures += expect_true(memcmp(out, expected, sizeof(expected)) == 0, "annexb bytes");
    failures += expect_true(native_h264_annexb_is_keyframe(out, out_len), "keyframe");
    failures += expect_true(native_h264_avc_to_annexb(avc, sizeof(avc), tiny, sizeof(tiny), &out_len) == NATIVE_H264_OUTPUT_TOO_SMALL, "small output");
    return failures;
}

static int test_invalid_and_truncated_avc(void) {
    const uint8_t empty = 0;
    const uint8_t truncated_len[] = {0x00, 0x00, 0x00};
    const uint8_t zero_len[] = {0x00, 0x00, 0x00, 0x00};
    const uint8_t too_long[] = {0x00, 0x00, 0x00, 0x02, 0x65};
    const uint8_t valid[] = {0x00, 0x00, 0x00, 0x01, 0x65};
    NativeH264Info info;
    uint8_t out[16];
    size_t out_len = 99;
    int failures = 0;

    failures += expect_true(native_h264_scan_avc(NULL, sizeof(valid), &info) == NATIVE_H264_INVALID, "scan null data");
    failures += expect_true(native_h264_scan_avc(valid, sizeof(valid), NULL) == NATIVE_H264_INVALID, "scan null info");
    failures += expect_true(native_h264_scan_avc(&empty, 0, &info) == NATIVE_H264_INVALID, "scan empty");
    failures += expect_true(native_h264_scan_avc(truncated_len, sizeof(truncated_len), &info) == NATIVE_H264_INVALID, "scan truncated length");
    failures += expect_true(native_h264_scan_avc(zero_len, sizeof(zero_len), &info) == NATIVE_H264_INVALID, "scan zero length");
    failures += expect_true(native_h264_scan_avc(too_long, sizeof(too_long), &info) == NATIVE_H264_INVALID, "scan too long");

    failures += expect_true(native_h264_avc_to_annexb(NULL, sizeof(valid), out, sizeof(out), &out_len) == NATIVE_H264_INVALID, "convert null data");
    failures += expect_true(native_h264_avc_to_annexb(valid, sizeof(valid), NULL, sizeof(out), &out_len) == NATIVE_H264_INVALID, "convert null output");
    failures += expect_true(native_h264_avc_to_annexb(valid, sizeof(valid), out, sizeof(out), NULL) == NATIVE_H264_INVALID, "convert null out_len");
    failures += expect_true(native_h264_avc_to_annexb(&empty, 0, out, sizeof(out), &out_len) == NATIVE_H264_INVALID, "convert empty");
    failures += expect_true(native_h264_avc_to_annexb(truncated_len, sizeof(truncated_len), out, sizeof(out), &out_len) == NATIVE_H264_INVALID, "convert truncated length");
    failures += expect_true(native_h264_avc_to_annexb(zero_len, sizeof(zero_len), out, sizeof(out), &out_len) == NATIVE_H264_INVALID, "convert zero length");
    failures += expect_true(native_h264_avc_to_annexb(too_long, sizeof(too_long), out, sizeof(out), &out_len) == NATIVE_H264_INVALID, "convert too long");
    failures += expect_true(!native_h264_annexb_is_keyframe(NULL, 5), "annexb null");
    failures += expect_true(!native_h264_annexb_is_keyframe(out, 0), "annexb empty");
    return failures;
}

static int test_annexb_prefix_detection(void) {
    const uint8_t prefix3[] = {
        0x00, 0x00, 0x01, 0x67, 0xaa,
        0x00, 0x00, 0x01, 0x65, 0xbb,
    };
    const uint8_t prefix4[] = {
        0x00, 0x00, 0x00, 0x01, 0x68, 0xaa,
        0x00, 0x00, 0x00, 0x01, 0x65, 0xbb,
    };
    const uint8_t mixed_prefix[] = {
        0x12, 0x34,
        0x00, 0x00, 0x01, 0x67,
        0x00, 0x00, 0x00, 0x01, 0x65,
    };
    const uint8_t idr_only[] = {0x00, 0x00, 0x01, 0x65, 0xaa};
    const uint8_t sps_only[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0xaa};
    int failures = 0;
    NativeH264Info info;

    failures += expect_true(native_h264_scan_annexb(prefix3, sizeof(prefix3), &info) == NATIVE_H264_OK, "scan 3-byte Annex-B prefix");
    failures += expect_true(info.nal_count == 2 && info.has_sps && info.has_idr && !info.has_pps, "scan 3-byte Annex-B flags");
    failures += expect_true(native_h264_scan_annexb(prefix4, sizeof(prefix4), &info) == NATIVE_H264_OK, "scan 4-byte Annex-B prefix");
    failures += expect_true(info.nal_count == 2 && info.has_pps && info.has_idr && !info.has_sps, "scan 4-byte Annex-B flags");
    failures += expect_true(native_h264_annexb_is_keyframe(prefix3, sizeof(prefix3)), "3-byte Annex-B prefix keyframe");
    failures += expect_true(native_h264_annexb_is_keyframe(prefix4, sizeof(prefix4)), "4-byte Annex-B prefix keyframe");
    failures += expect_true(native_h264_annexb_is_keyframe(mixed_prefix, sizeof(mixed_prefix)), "mixed Annex-B prefixes keyframe");
    failures += expect_true(!native_h264_annexb_is_keyframe(idr_only, sizeof(idr_only)), "IDR without SPS/PPS is not keyframe");
    failures += expect_true(!native_h264_annexb_is_keyframe(sps_only, sizeof(sps_only)), "SPS without IDR is not keyframe");
    failures += expect_true(native_h264_scan_annexb((const uint8_t *)"abcd", 4, &info) == NATIVE_H264_INVALID, "scan invalid Annex-B");
    failures += expect_true(native_h264_scan_annexb(prefix3, sizeof(prefix3), NULL) == NATIVE_H264_INVALID, "scan Annex-B null info");
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_sps_pps_idr_matrix();
    failures += test_exact_capacity_and_bytes();
    failures += test_invalid_and_truncated_avc();
    failures += test_annexb_prefix_detection();
    return failures ? 1 : 0;
}
