/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <executorch/backends/vulkan/runtime/api/api.h>
#include <executorch/backends/vulkan/runtime/graph/ops/utils/StagingUtils.h>
#include <iostream>

#include "stats.h"
#include "utils.h"

using namespace vkapi;

class App {
 private:
  size_t buf_cache_size_;
  uint32_t sm_count_;
  uint32_t nthread_logic_;

 public:
  App() {
    context()->initialize_querypool();

    std::cout << context()->adapter_ptr()->stringize() << std::endl
              << std::endl;

    auto cl_device = get_cl_device();

    sm_count_ = cl_device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>();
    nthread_logic_ = cl_device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
    buf_cache_size_ = cl_device.getInfo<CL_DEVICE_GLOBAL_MEM_CACHE_SIZE>();

    std::cout << std::endl;
    std::cout << "SM count," << sm_count_ << std::endl;
    std::cout << "Logic Thread Count," << nthread_logic_ << std::endl;
    std::cout << "Cache Size," << buf_cache_size_ << std::endl;
  }

  void reg_count() {
    std::cout << std::endl;
    std::cout << "------ Register Count ------" << std::endl;
    const uint32_t NREG_MIN = 1;
    const uint32_t NREG_MAX = 512;
    const uint32_t NREG_STEP = 1;

    // TODO: Make these values configurable
    const double COMPENSATE = 0.01;
    const double THRESHOLD = 3;

    const uint32_t NGRP_MIN = 1;
    const uint32_t NGRP_MAX = 64;
    const uint32_t NGRP_STEP = 1;

    uint32_t NITER;

    auto bench = [&](uint32_t ngrp, uint32_t nreg) {
      StorageBuffer buffer(context(), vkapi::kFloat, 1);
      vkapi::PipelineBarrier pipeline_barrier{};

      auto shader_name = "reg_count_" + std::to_string(nreg);

      auto time = benchmark_on_gpu(shader_name, 100, [&]() {
        context()->submit_compute_job(
            VK_KERNEL_FROM_STR(shader_name),
            pipeline_barrier,
            {1, ngrp, 1},
            {1, 1, 1},
            {SV(NITER)},
            VK_NULL_HANDLE,
            0,
            buffer.buffer());
      });
      return time;
    };

    std::cout << "Calculating NITER..." << std::endl;
    ensure_min_niter(1000, NITER, [&]() { return bench(1, NREG_MIN); });
    std::cout << "NITER," << NITER << std::endl;

    uint32_t nreg_max;

    DtJumpFinder<5> dj(COMPENSATE, THRESHOLD);
    uint32_t nreg = NREG_MIN;
    for (; nreg <= NREG_MAX; nreg += NREG_STEP) {
      double time = bench(1, nreg);
      std::cout << "Testing nreg=\t" << nreg << "\tTime=\t" << time
                << std::endl;
      if (dj.push(time)) {
        nreg -= NREG_STEP;
        nreg_max = nreg;
        break;
      }
    }
    if (nreg >= NREG_MAX) {
      std::cout << "Unable to conclude a maximal register count" << std::endl;
      nreg_max = NREG_STEP;
    } else {
      std::cout << nreg_max << " registers are available at most" << std::endl;
    }

    auto find_ngrp_by_nreg = [&](const uint32_t nreg) {
      DtJumpFinder<5> dj(COMPENSATE, THRESHOLD);
      for (auto ngrp = NGRP_MIN; ngrp <= NGRP_MAX; ngrp += NGRP_STEP) {
        auto time = bench(ngrp, nreg);
        std::cout << "Testing occupation (nreg=" << nreg << "); ngrp=" << ngrp
                  << ", time=" << time << " us" << std::endl;

        if (dj.push(time)) {
          ngrp -= NGRP_STEP;
          std::cout << "Using " << nreg << " registers can have " << ngrp
                    << " concurrent single-thread workgroups" << std::endl;
          return ngrp;
        }
      }
      std::cout
          << "Unable to conclude a maximum number of concurrent single-thread workgroups when "
          << nreg << " registers are occupied" << std::endl;
      return (uint32_t)1;
    };

    uint32_t ngrp_full, ngrp_half;
    ngrp_full = find_ngrp_by_nreg(nreg_max);
    ngrp_half = find_ngrp_by_nreg(nreg_max / 2);

    std::string reg_ty;

    if (ngrp_full * 1.5 < ngrp_half) {
      std::cout << "All physical threads in an sm share " << nreg_max
                << " registers" << std::endl;
      reg_ty = "Pooled";

    } else {
      std::cout << "Each physical thread has " << nreg_max << " registers"
                << std::endl;
      reg_ty = "Dedicated";
    }

    std::cout << std::endl << std::endl;
    std::cout << "NITER," << NITER << std::endl;
    std::cout << "Max registers," << nreg_max << std::endl;
    std::cout << "Concurrent full single thread workgroups," << ngrp_full
              << std::endl;
    std::cout << "Concurrent half single thread workgroups," << ngrp_half
              << std::endl;
    std::cout << "Register type," << reg_ty << std::endl;
  }

  void buf_cacheline_size() {
    std::cout << std::endl;
    std::cout << "------ Buffer Cacheline Size ------" << std::endl;

    // TODO: Make these values configurable
    const double COMPENSATE = 0.01;
    const double THRESHOLD = 10;

    const uint32_t PITCH = buf_cache_size_ / nthread_logic_;
    const uint32_t BUF_SIZE = buf_cache_size_;
    const uint32_t MAX_STRIDE = PITCH;

    uint32_t NITER;

    auto bench = [&](int stride) {
      StorageBuffer in_buf(context(), vkapi::kFloat, BUF_SIZE);
      StorageBuffer out_buf(context(), vkapi::kFloat, 1);
      vkapi::PipelineBarrier pipeline_barrier{};

      auto shader_name = "buf_cacheline_size";

      auto time = benchmark_on_gpu(shader_name, 100, [&]() {
        context()->submit_compute_job(
            VK_KERNEL_FROM_STR(shader_name),
            pipeline_barrier,
            {nthread_logic_, 1, 1},
            {nthread_logic_, 1, 1},
            {SV(NITER), SV(stride), SV(PITCH)},
            VK_NULL_HANDLE,
            0,
            in_buf.buffer(),
            out_buf.buffer());
      });
      return time;
    };

    ensure_min_niter(1000, NITER, [&]() { return bench(1); });

    uint32_t cacheline_size;

    DtJumpFinder<5> dj(COMPENSATE, THRESHOLD);
    uint32_t stride = 1;
    for (; stride <= MAX_STRIDE; ++stride) {
      double time = bench(stride);
      std::cout << "Testing stride=\t" << stride << "\t, time=\t" << time
                << std::endl;

      if (dj.push(time)) {
        cacheline_size = stride * sizeof(float);
        break;
      }
    }
    if (stride >= MAX_STRIDE) {
      std::cout << "Unable to conclude a top level buffer cacheline size."
                << std::endl;
      cacheline_size = MAX_STRIDE;
    }

    std::cout << "BufTopLevelCachelineSize," << cacheline_size << std::endl;
  }

  void buf_bandwidth() {
    std::cout << "\n------ Memory Bandwidth ------" << std::endl;

    // TODO: Make these values configurable

    // Maximum memory space read - 128MB
    // For regular devices, bandwidth plateaus at less memory than this, so more
    // is not needed.
    const uint32_t RANGE = 128 * 1024 * 1024;
    // Cache lines flushed
    const uint32_t NFLUSH = 4;
    // Number of loop unrolls. Changing this value requires an equal change in
    // buf_bandwidth.yaml
    const uint32_t NUNROLL = 16;
    // Number of iterations. Increasing this value reduces noise in exchange for
    // higher latency.
    const uint32_t NITER = 10;
    // Vector dimensions (vec4)
    const uint32_t VEC_WIDTH = 4;
    const uint32_t VEC_SIZE = VEC_WIDTH * sizeof(float);
    // Number of vectors that fit in the selected memory space
    const uint32_t NVEC = RANGE / VEC_SIZE;
    // Number of memory reads per thread
    const uint32_t NREAD_PER_THREAD = NUNROLL * NITER;
    // Number of threads needed to read all vectors
    const uint32_t NTHREAD = NVEC / NREAD_PER_THREAD;
    // Occupy all threads
    const uint32_t local_x = nthread_logic_;
    // Ensure that global is a multiple of local, and distribute across all SMs
    const uint32_t global_x =
        (NTHREAD / local_x * local_x) * sm_count_ * NFLUSH;

    auto bench = [&](uint32_t access_size) {
      // Number of vectors that fit in this iteration
      const uint32_t nvec_access = access_size / VEC_SIZE;

      // The address mask works as a modulo because x % 2^n == x & (2^n - 1).
      // This will help us limit address accessing to a specific set of unique
      // addresses depending on the iteration
      const uint32_t addr_mask = nvec_access - 1;

      StorageBuffer in_buf(context(), vkapi::kFloat, RANGE / sizeof(float));
      StorageBuffer out_buf(
          context(), vkapi::kFloat, VEC_WIDTH * nthread_logic_);
      vkapi::PipelineBarrier pipeline_barrier{};

      auto shader_name = "buf_bandwidth";

      auto time = benchmark_on_gpu(shader_name, 10, [&]() {
        context()->submit_compute_job(
            VK_KERNEL_FROM_STR(shader_name),
            pipeline_barrier,
            {global_x, 1, 1},
            {local_x, 1, 1},
            {SV(NITER), SV(addr_mask), SV(local_x)},
            VK_NULL_HANDLE,
            0,
            in_buf.buffer(),
            out_buf.buffer());
      });

      const uint32_t SIZE_TRANS = global_x * NREAD_PER_THREAD * VEC_SIZE;
      auto gbps = SIZE_TRANS * 1e-3 / time;
      std::cout << "Memory bandwidth accessing \t" << access_size
                << "\tB unique data is \t" << gbps << " \tgbps (\t" << time
                << "\tus)" << std::endl;
      return gbps;
    };

    double max_bandwidth = 0;
    double min_bandwidth = DBL_MAX;
    for (uint32_t access_size = VEC_SIZE; access_size < RANGE;
         access_size *= 2) {
      double gbps = bench(access_size);
      max_bandwidth = std::max(gbps, max_bandwidth);
      min_bandwidth = std::min(gbps, min_bandwidth);
    }

    std::cout << "MaxBandwidth (GB/s)," << max_bandwidth << std::endl;
    std::cout << "MinBandwidth (GB/s)," << min_bandwidth << std::endl;
  }
};

int main(int argc, const char** argv) {
  App app;

  // TODO: Allow user to skip tests
  app.reg_count();
  app.buf_cacheline_size();
  app.buf_bandwidth();

  return 0;
}
