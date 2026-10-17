#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "util/compiler.h"
#include "util/glheader.h"
#include "util/u_math.h"

static float
bc_half_to_float(uint16_t value)
{
    uint32_t sign = (uint32_t)(value & 0x8000u) << 16;
    int exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x3ffu;
    uint32_t bits;
    float result;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x3ffu;
            bits = sign | ((uint32_t)(exponent + 112) << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((uint32_t)(exponent + 112) << 23) | (mantissa << 13);
    }
    memcpy(&result, &bits, sizeof result);
    return result;
}

static uint16_t
bc_float_to_half(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int exponent = (int)((bits >> 23) & 0xffu) - 112;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10)
            return (uint16_t)sign;
        mantissa = (mantissa | 0x800000u) >> (1 - exponent);
        return (uint16_t)(sign | ((mantissa + 0x1000u) >> 13));
    }
    if (exponent >= 31)
        return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x0200u : 0));
    return (uint16_t)(sign | ((uint32_t)exponent << 10) |
                      ((mantissa + 0x1000u) >> 13));
}

#define _mesa_half_to_float bc_half_to_float
#define _mesa_float_to_half bc_float_to_half
#define _HALF_FLOAT_H_
#define FP16_ONE ((uint16_t)0x3c00u)
#define FP16_ZERO ((uint16_t)0)
#include "util/format/texcompress_s3tc_tmp.h"
#include "util/format/texcompress_bptc_tmp.h"
#include "bc_emulation.h"

static uint64_t
read_bits(const uint8_t *data, unsigned bit, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value |= (uint64_t)((data[(bit + i) >> 3] >> ((bit + i) & 7)) & 1u) << i;
    return value;
}

static void
decode_rgtc(const uint8_t *block, uint8_t *rgba, unsigned channels)
{
    uint8_t palette[2][8];
    for (unsigned channel = 0; channel < channels; channel++) {
        const uint8_t *src = block + channel * 8;
        uint8_t *p = palette[channel];
        p[0] = src[0];
        p[1] = src[1];
        if (p[0] > p[1]) {
            for (unsigned i = 1; i <= 6; i++)
                p[i + 1] = (uint8_t)(((7 - i) * p[0] + i * p[1]) / 7);
        } else {
            for (unsigned i = 1; i <= 4; i++)
                p[i + 1] = (uint8_t)(((5 - i) * p[0] + i * p[1]) / 5);
            p[6] = 0;
            p[7] = 255;
        }
        uint64_t selectors = read_bits(src + 2, 0, 48);
        for (unsigned pixel = 0; pixel < 16; pixel++)
            rgba[pixel * 4 + channel] =
                p[(selectors >> (pixel * 3)) & 7u];
    }
    for (unsigned pixel = 0; pixel < 16; pixel++) {
        if (channels == 1)
            rgba[pixel * 4 + 1] = rgba[pixel * 4 + 2] = 0;
        else
            rgba[pixel * 4 + 2] = 0;
        rgba[pixel * 4 + 3] = 255;
    }
}

static void
decode_rgtc_snorm(const uint8_t *block, float *rgba, unsigned channels)
{
    for (unsigned channel = 0; channel < channels; channel++) {
        const uint8_t *src = block + channel * 8;
        int endpoint0 = (int8_t)src[0];
        int endpoint1 = (int8_t)src[1];
        float palette[8];
        uint64_t selectors = read_bits(src + 2, 0, 48);
        if (endpoint0 == -128)
            endpoint0 = -127;
        if (endpoint1 == -128)
            endpoint1 = -127;
        palette[0] = endpoint0 / 127.0f;
        palette[1] = endpoint1 / 127.0f;
        if (endpoint0 > endpoint1) {
            for (unsigned i = 1; i <= 6; i++)
                palette[i + 1] =
                    ((7 - (float)i) * endpoint0 + i * endpoint1) / (7.0f * 127.0f);
        } else {
            for (unsigned i = 1; i <= 4; i++)
                palette[i + 1] =
                    ((5 - (float)i) * endpoint0 + i * endpoint1) / (5.0f * 127.0f);
            palette[6] = -1.0f;
            palette[7] = 1.0f;
        }
        for (unsigned pixel = 0; pixel < 16; pixel++)
            rgba[pixel * 4 + channel] =
                palette[(selectors >> (pixel * 3)) & 7u];
    }
    for (unsigned pixel = 0; pixel < 16; pixel++) {
        if (channels == 1)
            rgba[pixel * 4 + 1] = rgba[pixel * 4 + 2] = 0.0f;
        else
            rgba[pixel * 4 + 2] = 0.0f;
        rgba[pixel * 4 + 3] = 1.0f;
    }
}

static float
srgb_to_linear(uint8_t value)
{
    float encoded = value / 255.0f;
    return encoded <= 0.04045f
               ? encoded / 12.92f
               : powf((encoded + 0.055f) / 1.055f, 2.4f);
}

static double
now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}

int
bench_bc_emulation(uint32_t block_count, struct BcEmulationResult *result)
{
    uint8_t blocks[10][16];
    uint8_t decoded[16 * 4];
    float decoded_float[16 * 4];
    if (!block_count || !result)
        return 0;
    memset(result, 0, sizeof *result);

    for (unsigned format = 0; format < 10; format++) {
        for (unsigned i = 0; i < 16; i++)
            blocks[format][i] = (uint8_t)(i * 37u + format * 19u);
        if (format >= 8)
            blocks[format][0] |= 0x40u;
        uint64_t checksum = 0;
        double start = now_seconds();
        for (uint32_t block = 0; block < block_count; block++) {
            for (unsigned i = 0; i < 16; i++)
                blocks[format][i] ^= (uint8_t)((block + i * 13u) & 0xffu);
            if (format >= 8)
                blocks[format][0] &= (uint8_t)~3u;
            if (format >= 8)
                blocks[format][0] = (blocks[format][0] & 0x80u) | 0x40u;
            switch (format) {
            case 0:
                for (unsigned y = 0; y < 4; y++)
                    for (unsigned x = 0; x < 4; x++)
                        fetch_2d_texel_rgba_dxt3(4, blocks[format], x, y,
                                                 decoded + (y * 4 + x) * 4);
                break;
            case 1:
                for (unsigned y = 0; y < 4; y++)
                    for (unsigned x = 0; x < 4; x++)
                        fetch_2d_texel_rgba_dxt5(4, blocks[format], x, y,
                                                 decoded + (y * 4 + x) * 4);
                break;
            case 2:
                decode_rgtc(blocks[format], decoded, 1);
                break;
            case 3:
                decode_rgtc_snorm(blocks[format], decoded_float, 1);
                break;
            case 4:
                decode_rgtc(blocks[format], decoded, 2);
                break;
            case 5:
                decode_rgtc_snorm(blocks[format], decoded_float, 2);
                break;
            case 6:
                decompress_rgb_float(4, 4, blocks[format], 16,
                                     decoded_float, 4 * 4 * sizeof(float), false);
                break;
            case 7:
                decompress_rgb_float(4, 4, blocks[format], 16,
                                     decoded_float, 4 * 4 * sizeof(float), true);
                break;
            default:
                decompress_rgba_unorm(4, 4, blocks[format], 16, decoded, 16);
                if (format == 9) {
                    for (unsigned pixel = 0; pixel < 16; pixel++) {
                        decoded_float[pixel * 4 + 0] =
                            srgb_to_linear(decoded[pixel * 4 + 0]);
                        decoded_float[pixel * 4 + 1] =
                            srgb_to_linear(decoded[pixel * 4 + 1]);
                        decoded_float[pixel * 4 + 2] =
                            srgb_to_linear(decoded[pixel * 4 + 2]);
                        decoded_float[pixel * 4 + 3] =
                            decoded[pixel * 4 + 3] / 255.0f;
                    }
                }
                break;
            }
            if (format == 3 || format == 5 || format >= 6) {
                const uint8_t *bytes = (const uint8_t *)decoded_float;
                for (unsigned i = 0; i < sizeof decoded_float; i++)
                    checksum = checksum * 33u + bytes[i];
            } else {
                if (format == 8) {
                    for (unsigned pixel = 0; pixel < 16; pixel++) {
                        decoded_float[pixel * 4 + 0] = decoded[pixel * 4 + 0] / 255.0f;
                        decoded_float[pixel * 4 + 1] = decoded[pixel * 4 + 1] / 255.0f;
                        decoded_float[pixel * 4 + 2] = decoded[pixel * 4 + 2] / 255.0f;
                        decoded_float[pixel * 4 + 3] = decoded[pixel * 4 + 3] / 255.0f;
                    }
                    const uint8_t *bytes = (const uint8_t *)decoded_float;
                    for (unsigned i = 0; i < sizeof decoded_float; i++)
                        checksum = checksum * 33u + bytes[i];
                    continue;
                }
                for (unsigned i = 0; i < sizeof decoded; i++)
                    checksum = checksum * 33u + decoded[i];
            }
        }
        double elapsed = now_seconds() - start;
        result->checksum[format] = (uint32_t)(checksum ^ (checksum >> 32));
        result->mpix_per_second[format] =
            elapsed > 0.0 ? (double)block_count * 16.0 / (elapsed * 1.0e6) : 0.0;
    }
    return 1;
}
