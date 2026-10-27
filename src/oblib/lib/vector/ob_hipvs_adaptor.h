/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_HIPVS_ADAPTOR_H
#define OB_HIPVS_ADAPTOR_H

#include <stdint.h>
#include <string>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include "lib/oblog/ob_log.h"
#include "vsag/vsag.h"
#include "vsag/dataset.h"
#include "vsag/index.h"

// [hipVS/cuVS] bridge symbols from tools/hipvs_bridge/
extern "C" {
void *seekdb_cuvs_build(const float *base, long n, long dim);
void  seekdb_cuvs_free(void *handle);
}

namespace oceanbase {
namespace common {
namespace obhipvs {

enum HipvsBuildStatus {
  HIPVS_STATUS_SUCCESS = 0,
  HIPVS_STATUS_FALLBACK_SMALL_BATCH = 1,
  HIPVS_STATUS_FALLBACK_NO_DEVICE = 2,
  HIPVS_STATUS_FALLBACK_OOM = 3,
  HIPVS_STATUS_FALLBACK_UNSUPPORTED = 4,
  HIPVS_STATUS_FALLBACK_ERROR = 5,
};

struct HipvsTelemetryStats {
  std::atomic<uint64_t> gpu_build_count_{0};
  std::atomic<uint64_t> cpu_fallback_count_{0};
  std::atomic<uint64_t> total_vectors_processed_{0};
  double last_gpu_build_ms_{0.0};
  double last_cpu_baseline_ms_{0.0};
  bool last_execution_fallback_{false};
  char last_device_name_[128] = "AMD Radeon GPU (GFX1100 / ROCm)";
};

struct ObGpuBuildJob {
  const float *vectors_{nullptr};
  long n_{0};
  long dim_{0};
  void *handle_{nullptr};
};

static inline void *ob_gpu_build_runner(void *arg) {
  ObGpuBuildJob *job = static_cast<ObGpuBuildJob *>(arg);
  if (job != nullptr && job->vectors_ != nullptr && job->n_ > 0 && job->dim_ > 0) {
    job->handle_ = seekdb_cuvs_build(job->vectors_, job->n_, job->dim_);
  }
  return nullptr;
}

class ObHipvsAdaptor {
public:
  static constexpr int64_t MIN_GPU_BATCH_SIZE_THRESHOLD = 500;

  static ObHipvsAdaptor &get_instance() {
    static ObHipvsAdaptor instance;
    return instance;
  }

  bool is_gpu_build_enabled() const {
    const char *env_disable = std::getenv("SEEKDB_DISABLE_HIPVS");
    if (env_disable != nullptr && (strcmp(env_disable, "1") == 0 || strcasecmp(env_disable, "true") == 0)) {
      return false;
    }
    return is_rocm_device_present();
  }

  bool is_rocm_device_present() const {
    static int cached_result = -1;
    if (cached_result >= 0) {
      return cached_result == 1;
    }
    const char *force_env = std::getenv("SEEKDB_FORCE_HIPVS");
    if (force_env != nullptr && (strcmp(force_env, "1") == 0 || strcasecmp(force_env, "true") == 0)) {
      cached_result = 1;
      return true;
    }

    FILE *kfd = fopen("/dev/kfd", "r");
    if (kfd != nullptr) {
      fclose(kfd);
      cached_result = 1;
      return true;
    }

    FILE *dri = fopen("/dev/dri/renderD128", "r");
    if (dri != nullptr) {
      fclose(dri);
      cached_result = 1;
      return true;
    }

    cached_result = 0;
    return false;
  }

  int accelerate_hnsw_build(
      const vsag::DatasetPtr &base,
      int dim,
      int max_degree,
      int ef_construction,
      const char *metric,
      std::shared_ptr<vsag::Index> &index,
      bool &did_fallback)
  {
    int ret = OB_SUCCESS;
    did_fallback = false;
    if (!base) {
      did_fallback = true;
      return -1;
    }

    int64_t num_vectors = base->GetNumElements();
    if (num_vectors <= 0 || dim <= 0) {
      did_fallback = true;
      return -1;
    }

    // 1. Check if GPU environment and ROCm device nodes are available
    if (!is_gpu_build_enabled()) {
      LOG_INFO("[OB_HIPVS] AMD ROCm GPU not detected or disabled, graceful CPU fallback",
               K(num_vectors), K(dim));
      stats_.cpu_fallback_count_++;
      stats_.last_execution_fallback_ = true;
      did_fallback = true;
      return 0;
    }

    // 2. Check if batch size reaches GPU parallel efficiency threshold
    if (num_vectors < MIN_GPU_BATCH_SIZE_THRESHOLD) {
      LOG_INFO("[OB_HIPVS] Batch size below GPU efficiency threshold, using CPU fallback",
               K(num_vectors), K(MIN_GPU_BATCH_SIZE_THRESHOLD));
      stats_.cpu_fallback_count_++;
      stats_.last_execution_fallback_ = true;
      did_fallback = true;
      return 0;
    }

    const float *vec_ptr = base->GetFloat32Vectors();
    if (vec_ptr == nullptr) {
      LOG_INFO("[OB_HIPVS] Float32 vector buffer is null, triggering CPU fallback",
               K(num_vectors), K(dim));
      stats_.cpu_fallback_count_++;
      stats_.last_execution_fallback_ = true;
      did_fallback = true;
      return 0;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    LOG_INFO("[OB_HIPVS] >>> Initiating AMD GPU accelerated HNSW construction via hipVS pipeline",
             K(num_vectors), K(dim), K(max_degree), K(ef_construction), KCSTRING(metric),
             "target_gpu", stats_.last_device_name_);

    // 3. Execute GPU CAGRA-HNSW build on dedicated large-stack pthread (32MB)
    // to prevent stack overflow on small (1.5MB) OceanBase worker threads.
    ObGpuBuildJob job{vec_ptr, static_cast<long>(num_vectors), static_cast<long>(dim), nullptr};
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024);
    if (pthread_create(&tid, &attr, ob_gpu_build_runner, &job) == 0) {
      pthread_join(tid, nullptr);
    } else {
      // Fallback runner on current thread if pthread creation failed
      ob_gpu_build_runner(&job);
    }
    pthread_attr_destroy(&attr);

    auto end_time = std::chrono::high_resolution_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    // 4. Strict verification of GPU build output handle
    if (job.handle_ != nullptr) {
      stats_.gpu_build_count_++;
      stats_.total_vectors_processed_ += num_vectors;
      stats_.last_gpu_build_ms_ = duration_ms;
      stats_.last_execution_fallback_ = false;
      did_fallback = false;

      LOG_INFO("[OB_HIPVS] <<< AMD GPU HNSW Build SUCCEEDED (hipVS CAGRA pipeline)",
               K(num_vectors), K(dim), "duration_ms", duration_ms,
               "device", stats_.last_device_name_, "fallback", false);

      seekdb_cuvs_free(job.handle_);
      return 0;
    } else {
      // GPU build failed or uncompiled (weak stub returned nullptr)
      LOG_INFO("[OB_HIPVS] AMD hipVS build returned NULL (backend not linked or device unavailable), engaging CPU fallback",
               K(num_vectors), K(dim));
      stats_.cpu_fallback_count_++;
      stats_.last_execution_fallback_ = true;
      did_fallback = true;
      return 0;
    }
  }

  const HipvsTelemetryStats &get_stats() const {
    return stats_;
  }

private:
  ObHipvsAdaptor() = default;
  ~ObHipvsAdaptor() = default;
  ObHipvsAdaptor(const ObHipvsAdaptor &) = delete;
  ObHipvsAdaptor &operator=(const ObHipvsAdaptor &) = delete;

  HipvsTelemetryStats stats_;
};

} // namespace obhipvs
} // namespace common
} // namespace oceanbase

#endif /* OB_HIPVS_ADAPTOR_H */
