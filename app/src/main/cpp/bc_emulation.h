#pragma once

#include <stdint.h>

struct BcEmulationResult {
    double mpix_per_second[10];
    uint32_t checksum[10];
};

int bench_bc_emulation(uint32_t block_count, struct BcEmulationResult *result);
