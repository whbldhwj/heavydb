/*
 * Copyright 2022 HEAVY.AI, Inc.
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

#ifndef __CUDACC__

#include "TableFunctionsCommon.h"

#include <cstring>  // std::memcpy
#include <filesystem>
#include <memory>
#include <mutex>
#include <regex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#define NANOSECONDS_PER_SECOND 1000000000

template <typename T>
NEVER_INLINE HOST std::pair<T, T> get_column_min_max(const Column<T>& col) {
  T col_min = std::numeric_limits<T>::max();
  T col_max = std::numeric_limits<T>::lowest();
  const int64_t num_rows = col.size();
  const size_t max_thread_count = std::thread::hardware_concurrency();
  const size_t max_inputs_per_thread = 200000;
  const size_t num_threads = std::min(
      max_thread_count, ((num_rows + max_inputs_per_thread - 1) / max_inputs_per_thread));

  std::vector<T> local_col_mins(num_threads, std::numeric_limits<T>::max());
  std::vector<T> local_col_maxes(num_threads, std::numeric_limits<T>::lowest());
  tbb::task_arena limited_arena(num_threads);

  limited_arena.execute([&] {
    tbb::parallel_for(tbb::blocked_range<int64_t>(0, num_rows),
                      [&](const tbb::blocked_range<int64_t>& r) {
                        const int64_t start_idx = r.begin();
                        const int64_t end_idx = r.end();
                        T local_col_min = std::numeric_limits<T>::max();
                        T local_col_max = std::numeric_limits<T>::lowest();
                        for (int64_t r = start_idx; r < end_idx; ++r) {
                          if (col.isNull(r)) {
                            continue;
                          }
                          if (col[r] < local_col_min) {
                            local_col_min = col[r];
                          }
                          if (col[r] > local_col_max) {
                            local_col_max = col[r];
                          }
                        }
                        size_t thread_idx = tbb::this_task_arena::current_thread_index();
                        if (local_col_min < local_col_mins[thread_idx]) {
                          local_col_mins[thread_idx] = local_col_min;
                        }
                        if (local_col_max > local_col_maxes[thread_idx]) {
                          local_col_maxes[thread_idx] = local_col_max;
                        }
                      });
  });

  for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    if (local_col_mins[thread_idx] < col_min) {
      col_min = local_col_mins[thread_idx];
    }
    if (local_col_maxes[thread_idx] > col_max) {
      col_max = local_col_maxes[thread_idx];
    }
  }
  return std::make_pair(col_min, col_max);
}

template NEVER_INLINE HOST std::pair<int8_t, int8_t> get_column_min_max(
    const Column<int8_t>& col);
template NEVER_INLINE HOST std::pair<int16_t, int16_t> get_column_min_max(
    const Column<int16_t>& col);
template NEVER_INLINE HOST std::pair<int32_t, int32_t> get_column_min_max(
    const Column<int32_t>& col);
template NEVER_INLINE HOST std::pair<int64_t, int64_t> get_column_min_max(
    const Column<int64_t>& col);
template NEVER_INLINE HOST std::pair<float, float> get_column_min_max(
    const Column<float>& col);
template NEVER_INLINE HOST std::pair<double, double> get_column_min_max(
    const Column<double>& col);

std::pair<int32_t, int32_t> get_column_min_max(const Column<TextEncodingDict>& col) {
  Column<int32_t> int_alias_col;
  int_alias_col.ptr_ = reinterpret_cast<int32_t*>(col.ptr_);
  int_alias_col.size_ = col.size_;
  return get_column_min_max(int_alias_col);
}

template <typename T>
NEVER_INLINE HOST double get_column_mean(const Column<T>& col) {
  return get_column_mean(col.ptr_, col.size_);
}

template NEVER_INLINE HOST double get_column_mean(const Column<int32_t>& col);
template NEVER_INLINE HOST double get_column_mean(const Column<int64_t>& col);
template NEVER_INLINE HOST double get_column_mean(const Column<float>& col);
template NEVER_INLINE HOST double get_column_mean(const Column<double>& col);

template <typename T>
NEVER_INLINE HOST double get_column_mean(const T* data, const int64_t num_rows) {
  // const int64_t num_rows = col.size();
  const size_t max_thread_count = std::thread::hardware_concurrency();
  const size_t max_inputs_per_thread = 200000;
  const size_t num_threads = std::min(
      max_thread_count, ((num_rows + max_inputs_per_thread - 1) / max_inputs_per_thread));

  std::vector<double> local_col_sums(num_threads, 0.);
  std::vector<int64_t> local_col_non_null_counts(num_threads, 0L);
  tbb::task_arena limited_arena(num_threads);

  limited_arena.execute([&] {
    tbb::parallel_for(tbb::blocked_range<int64_t>(0, num_rows),
                      [&](const tbb::blocked_range<int64_t>& r) {
                        const int64_t start_idx = r.begin();
                        const int64_t end_idx = r.end();
                        double local_col_sum = 0.;
                        int64_t local_col_non_null_count = 0;
                        for (int64_t r = start_idx; r < end_idx; ++r) {
                          const T val = data[r];
                          if (val == inline_null_value<T>()) {
                            continue;
                          }
                          local_col_sum += data[r];
                          local_col_non_null_count++;
                        }
                        size_t thread_idx = tbb::this_task_arena::current_thread_index();
                        local_col_sums[thread_idx] += local_col_sum;
                        local_col_non_null_counts[thread_idx] += local_col_non_null_count;
                      });
  });

  double col_sum = 0.0;
  int64_t col_non_null_count = 0L;

  for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    col_sum += local_col_sums[thread_idx];
    col_non_null_count += local_col_non_null_counts[thread_idx];
  }

  return col_non_null_count == 0 ? 0 : col_sum / col_non_null_count;
}

// Todo(todd): we should use a functor approach for gathering whatever stats
// a table function needs so we're not repeating boilerplate code (although
// should confirm it doesn't have an adverse affect on performance).
// Leaving as a follow-up though until we have more examples of real-world
// usage patterns.

template NEVER_INLINE HOST double get_column_mean(const int32_t* data,
                                                  const int64_t num_rows);
template NEVER_INLINE HOST double get_column_mean(const int64_t* data,
                                                  const int64_t num_rows);
template NEVER_INLINE HOST double get_column_mean(const float* data,
                                                  const int64_t num_rows);
template NEVER_INLINE HOST double get_column_mean(const double* data,
                                                  const int64_t num_rows);

template <typename T>
NEVER_INLINE HOST double get_column_std_dev(const Column<T>& col, const double mean) {
  return get_column_std_dev(col.ptr_, col.size_, mean);
}

template NEVER_INLINE HOST double get_column_std_dev(const Column<int32_t>& col,
                                                     const double mean);
template NEVER_INLINE HOST double get_column_std_dev(const Column<int64_t>& col,
                                                     const double mean);
template NEVER_INLINE HOST double get_column_std_dev(const Column<float>& col,
                                                     const double mean);
template NEVER_INLINE HOST double get_column_std_dev(const Column<double>& col,
                                                     const double mean);

template <typename T>
NEVER_INLINE HOST double get_column_std_dev(const T* data,
                                            const int64_t num_rows,
                                            const double mean) {
  // const int64_t num_rows = col.size();
  const size_t max_thread_count = std::thread::hardware_concurrency();
  const size_t max_inputs_per_thread = 200000;
  const size_t num_threads = std::min(
      max_thread_count, ((num_rows + max_inputs_per_thread - 1) / max_inputs_per_thread));

  std::vector<double> local_col_squared_residuals(num_threads, 0.);
  std::vector<int64_t> local_col_non_null_counts(num_threads, 0L);
  tbb::task_arena limited_arena(num_threads);

  limited_arena.execute([&] {
    tbb::parallel_for(tbb::blocked_range<int64_t>(0, num_rows),
                      [&](const tbb::blocked_range<int64_t>& r) {
                        const int64_t start_idx = r.begin();
                        const int64_t end_idx = r.end();
                        double local_col_squared_residual = 0.;
                        int64_t local_col_non_null_count = 0;
                        for (int64_t r = start_idx; r < end_idx; ++r) {
                          const T val = data[r];
                          if (val == inline_null_value<T>()) {
                            continue;
                          }
                          const double residual = val - mean;
                          local_col_squared_residual += (residual * residual);
                          local_col_non_null_count++;
                        }
                        size_t thread_idx = tbb::this_task_arena::current_thread_index();
                        local_col_squared_residuals[thread_idx] +=
                            local_col_squared_residual;
                        local_col_non_null_counts[thread_idx] += local_col_non_null_count;
                      });
  });

  double col_sum_squared_residual = 0.0;
  int64_t col_non_null_count = 0;

  for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    col_sum_squared_residual += local_col_squared_residuals[thread_idx];
    col_non_null_count += local_col_non_null_counts[thread_idx];
  }

  return col_non_null_count == 0 ? 0
                                 : sqrt(col_sum_squared_residual / col_non_null_count);
}

template NEVER_INLINE HOST double get_column_std_dev(const int32_t* data,
                                                     const int64_t num_rows,
                                                     const double mean);
template NEVER_INLINE HOST double get_column_std_dev(const int64_t* data,
                                                     const int64_t num_rows,
                                                     const double mean);
template NEVER_INLINE HOST double get_column_std_dev(const float* data,
                                                     const int64_t num_rows,
                                                     const double mean);
template NEVER_INLINE HOST double get_column_std_dev(const double* data,
                                                     const int64_t num_rows,
                                                     const double mean);

template <typename T>
NEVER_INLINE HOST std::tuple<T, T, bool> get_column_metadata(const Column<T>& col) {
  T col_min = std::numeric_limits<T>::max();
  T col_max = std::numeric_limits<T>::lowest();
  bool has_nulls = false;
  const int64_t num_rows = col.size();
  const size_t max_thread_count = std::thread::hardware_concurrency();
  const size_t max_inputs_per_thread = 200000;
  const size_t num_threads = std::min(
      max_thread_count, ((num_rows + max_inputs_per_thread - 1) / max_inputs_per_thread));

  std::vector<T> local_col_mins(num_threads, std::numeric_limits<T>::max());
  std::vector<T> local_col_maxes(num_threads, std::numeric_limits<T>::lowest());
  std::vector<bool> local_col_has_nulls(num_threads, false);
  tbb::task_arena limited_arena(num_threads);

  limited_arena.execute([&] {
    tbb::parallel_for(tbb::blocked_range<int64_t>(0, num_rows),
                      [&](const tbb::blocked_range<int64_t>& r) {
                        const int64_t start_idx = r.begin();
                        const int64_t end_idx = r.end();
                        T local_col_min = std::numeric_limits<T>::max();
                        T local_col_max = std::numeric_limits<T>::lowest();
                        bool local_has_nulls = false;
                        for (int64_t r = start_idx; r < end_idx; ++r) {
                          if (col.isNull(r)) {
                            local_has_nulls = true;
                            continue;
                          }
                          if (col[r] < local_col_min) {
                            local_col_min = col[r];
                          }
                          if (col[r] > local_col_max) {
                            local_col_max = col[r];
                          }
                        }
                        const size_t thread_idx =
                            tbb::this_task_arena::current_thread_index();
                        if (local_has_nulls) {
                          local_col_has_nulls[thread_idx] = true;
                        }
                        if (local_col_min < local_col_mins[thread_idx]) {
                          local_col_mins[thread_idx] = local_col_min;
                        }
                        if (local_col_max > local_col_maxes[thread_idx]) {
                          local_col_maxes[thread_idx] = local_col_max;
                        }
                      });
  });

  for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    if (local_col_has_nulls[thread_idx]) {
      has_nulls = true;
    }
    if (local_col_mins[thread_idx] < col_min) {
      col_min = local_col_mins[thread_idx];
    }
    if (local_col_maxes[thread_idx] > col_max) {
      col_max = local_col_maxes[thread_idx];
    }
  }
  return {col_min, col_max, has_nulls};
}

template NEVER_INLINE HOST std::tuple<int8_t, int8_t, bool> get_column_metadata(
    const Column<int8_t>& col);
template NEVER_INLINE HOST std::tuple<int16_t, int16_t, bool> get_column_metadata(
    const Column<int16_t>& col);
template NEVER_INLINE HOST std::tuple<int32_t, int32_t, bool> get_column_metadata(
    const Column<int32_t>& col);
template NEVER_INLINE HOST std::tuple<int64_t, int64_t, bool> get_column_metadata(
    const Column<int64_t>& col);
template NEVER_INLINE HOST std::tuple<float, float, bool> get_column_metadata(
    const Column<float>& col);
template NEVER_INLINE HOST std::tuple<double, double, bool> get_column_metadata(
    const Column<double>& col);

std::tuple<int32_t, int32_t, bool> get_column_metadata(
    const Column<TextEncodingDict>& col) {
  Column<int32_t> int_alias_col;
  int_alias_col.ptr_ = reinterpret_cast<int32_t*>(col.ptr_);
  int_alias_col.size_ = col.size_;
  return get_column_metadata(int_alias_col);
}

template <typename T>
void z_std_normalize_col(const T* input_data,
                         T* output_data,
                         const int64_t num_rows,
                         const double mean,
                         const double std_dev) {
  if (std_dev <= 0.0) {
    throw std::runtime_error("Standard deviation cannot be <= 0");
  }
  const double inv_std_dev = 1.0 / std_dev;

  tbb::parallel_for(tbb::blocked_range<int64_t>(0, num_rows),
                    [&](const tbb::blocked_range<int64_t>& r) {
                      const int64_t start_idx = r.begin();
                      const int64_t end_idx = r.end();
                      for (int64_t row_idx = start_idx; row_idx < end_idx; ++row_idx) {
                        output_data[row_idx] = (input_data[row_idx] - mean) * inv_std_dev;
                      }
                    });
}

template void z_std_normalize_col(const float* input_data,
                                  float* output_data,
                                  const int64_t num_rows,
                                  const double mean,
                                  const double std_dev);
template void z_std_normalize_col(const double* input_data,
                                  double* output_data,
                                  const int64_t num_rows,
                                  const double mean,
                                  const double std_dev);

template <typename T>
std::vector<std::vector<T>> z_std_normalize_data(const std::vector<T*>& input_data,
                                                 const int64_t num_rows) {
  const int64_t num_features = input_data.size();
  std::vector<std::vector<T>> normalized_data(num_features);
  for (int64_t feature_idx = 0; feature_idx < num_features; ++feature_idx) {
    const auto mean = get_column_mean(input_data[feature_idx], num_rows);
    const auto std_dev = get_column_std_dev(input_data[feature_idx], num_rows, mean);
    normalized_data[feature_idx].resize(num_rows);
    z_std_normalize_col(input_data[feature_idx],
                        normalized_data[feature_idx].data(),
                        num_rows,
                        mean,
                        std_dev);
  }
  return normalized_data;
}

template std::vector<std::vector<float>> z_std_normalize_data(
    const std::vector<float*>& input_data,
    const int64_t num_rows);
template std::vector<std::vector<double>> z_std_normalize_data(
    const std::vector<double*>& input_data,
    const int64_t num_rows);

template <typename T1, typename T2>
NEVER_INLINE HOST T1
distance_in_meters(const T1 fromlon, const T1 fromlat, const T2 tolon, const T2 tolat) {
  T1 latitudeArc = (fromlat - tolat) * 0.017453292519943295769236907684886;
  T1 longitudeArc = (fromlon - tolon) * 0.017453292519943295769236907684886;
  T1 latitudeH = sin(latitudeArc * 0.5);
  latitudeH *= latitudeH;
  T1 lontitudeH = sin(longitudeArc * 0.5);
  lontitudeH *= lontitudeH;
  T1 tmp = cos(fromlat * 0.017453292519943295769236907684886) *
           cos(tolat * 0.017453292519943295769236907684886);
  return 6372797.560856 * (2.0 * asin(sqrt(latitudeH + tmp * lontitudeH)));
}

template NEVER_INLINE HOST float distance_in_meters(const float fromlon,
                                                    const float fromlat,
                                                    const float tolon,
                                                    const float tolat);

template NEVER_INLINE HOST float distance_in_meters(const float fromlon,
                                                    const float fromlat,
                                                    const double tolon,
                                                    const double tolat);

template NEVER_INLINE HOST double distance_in_meters(const double fromlon,
                                                     const double fromlat,
                                                     const float tolon,
                                                     const float tolat);

template NEVER_INLINE HOST double distance_in_meters(const double fromlon,
                                                     const double fromlat,
                                                     const double tolon,
                                                     const double tolat);

bool DataBufferCache::isKeyCached(const std::string& key) const {
  std::shared_lock<std::shared_mutex> read_lock(cache_mutex_);
  return data_cache_.count(key) > 0;
}

bool DataBufferCache::isKeyCachedAndSameLength(const std::string& key,
                                               const size_t num_bytes) const {
  std::shared_lock<std::shared_mutex> read_lock(cache_mutex_);
  const auto& cached_data_itr = data_cache_.find(key);
  if (cached_data_itr == data_cache_.end()) {
    return false;
  }
  return num_bytes == cached_data_itr->second->num_bytes;
}

template <typename T>
void DataBufferCache::getDataForKey(const std::string& key, T* dest_buffer) const {
  auto timer = DEBUG_TIMER(__func__);
  std::shared_lock<std::shared_mutex> read_lock(cache_mutex_);
  const auto& cached_data_itr = data_cache_.find(key);
  if (cached_data_itr == data_cache_.end()) {
    const std::string error_msg = "Data for key " + key + " not found in cache.";
    throw std::runtime_error(error_msg);
  }
  copyData(reinterpret_cast<int8_t*>(dest_buffer),
           cached_data_itr->second->data_buffer,
           cached_data_itr->second->num_bytes);
}

template <typename T>
const T& DataBufferCache::getDataRefForKey(const std::string& key) const {
  std::shared_lock<std::shared_mutex> read_lock(cache_mutex_);
  const auto& cached_data_itr = data_cache_.find(key);
  if (cached_data_itr == data_cache_.end()) {
    const std::string error_msg{"Data for key " + key + " not found in cache."};
    throw std::runtime_error(error_msg);
  }
  return *reinterpret_cast<const T*>(cached_data_itr->second->data_buffer);
}

template <typename T>
const T* DataBufferCache::getDataPtrForKey(const std::string& key) const {
  std::shared_lock<std::shared_mutex> read_lock(cache_mutex_);
  const auto& cached_data_itr = data_cache_.find(key);
  if (cached_data_itr == data_cache_.end()) {
    return nullptr;
  }
  return reinterpret_cast<const T* const>(cached_data_itr->second->data_buffer);
}

template <typename T>
void DataBufferCache::putDataForKey(const std::string& key,
                                    T* const data_buffer,
                                    const size_t num_elements) {
  auto timer = DEBUG_TIMER(__func__);
  const size_t num_bytes(num_elements * sizeof(T));
  auto cache_data = std::make_shared<CacheDataTf>(num_bytes);
  copyData(cache_data->data_buffer, reinterpret_cast<int8_t*>(data_buffer), num_bytes);
  std::unique_lock<std::shared_mutex> write_lock(cache_mutex_);
  const auto& cached_data_itr = data_cache_.find(key);
  if (data_cache_.find(key) != data_cache_.end()) {
    const std::string warning_msg =
        "Data for key " + key + " already exists in cache. Replacing.";
    std::cout << warning_msg << std::endl;
    cached_data_itr->second.reset();
    cached_data_itr->second = cache_data;
    return;
  }
  data_cache_.insert(std::make_pair(key, cache_data));
}

void DataBufferCache::copyData(int8_t* dest,
                               const int8_t* source,
                               const size_t num_bytes) const {
  if (num_bytes < parallel_copy_min_bytes) {
    std::memcpy(dest, source, num_bytes);
    return;
  }
  const size_t max_bytes_per_thread = parallel_copy_min_bytes;
  const size_t num_threads =
      (num_bytes + max_bytes_per_thread - 1) / max_bytes_per_thread;
  tbb::parallel_for(
      tbb::blocked_range<size_t>(0, num_threads, 1),
      [&](const tbb::blocked_range<size_t>& r) {
        const size_t end_chunk_idx = r.end();
        for (size_t chunk_idx = r.begin(); chunk_idx != end_chunk_idx; ++chunk_idx) {
          const size_t start_byte = chunk_idx * max_bytes_per_thread;
          const size_t length =
              std::min(start_byte + max_bytes_per_thread, num_bytes) - start_byte;
          std::memcpy(dest + start_byte, source + start_byte, length);
        }
      });
}

/* Definitions for DataCache */

template <typename T>
bool DataCache<T>::isKeyCached(const std::string& key) const {
  std::shared_lock<std::shared_mutex> read_lock(cache_mutex_);
  return data_cache_.count(key) > 0;
}

template <typename T>
std::shared_ptr<T> DataCache<T>::getDataForKey(const std::string& key) const {
  std::shared_lock<std::shared_mutex> read_lock(cache_mutex_);
  const auto& cached_data_itr = data_cache_.find(key);
  if (cached_data_itr == data_cache_.end()) {
    const std::string error_msg{"Data for key " + key + " not found in cache."};
    throw std::runtime_error(error_msg);
  }
  return cached_data_itr->second;
}

template <typename T>
void DataCache<T>::putDataForKey(const std::string& key, std::shared_ptr<T> const data) {
  std::unique_lock<std::shared_mutex> write_lock(cache_mutex_);
  const auto& cached_data_itr = data_cache_.find(key);
  if (cached_data_itr != data_cache_.end()) {
    const std::string warning_msg =
        "Data for key " + key + " already exists in cache. Replacing.";
    std::cout << warning_msg << std::endl;
    cached_data_itr->second.reset();
    cached_data_itr->second = data;
  }
  data_cache_.insert(std::make_pair(key, data));
}

namespace FileUtilities {

// Following implementation taken from https://stackoverflow.com/a/65851545

std::regex glob_to_regex(const std::string& glob, bool case_sensitive = false) {
  // Note It is possible to automate checking if filesystem is case sensitive or not (e.g.
  // by performing a test first time this function is ran)
  std::string regex_string{glob};
  // Escape all regex special chars:
  regex_string = std::regex_replace(regex_string, std::regex("\\\\"), "\\\\");
  regex_string = std::regex_replace(regex_string, std::regex("\\^"), "\\^");
  regex_string = std::regex_replace(regex_string, std::regex("\\."), "\\.");
  regex_string = std::regex_replace(regex_string, std::regex("\\$"), "\\$");
  regex_string = std::regex_replace(regex_string, std::regex("\\|"), "\\|");
  regex_string = std::regex_replace(regex_string, std::regex("\\("), "\\(");
  regex_string = std::regex_replace(regex_string, std::regex("\\)"), "\\)");
  regex_string = std::regex_replace(regex_string, std::regex("\\{"), "\\{");
  regex_string = std::regex_replace(regex_string, std::regex("\\{"), "\\}");
  regex_string = std::regex_replace(regex_string, std::regex("\\["), "\\[");
  regex_string = std::regex_replace(regex_string, std::regex("\\]"), "\\]");
  regex_string = std::regex_replace(regex_string, std::regex("\\+"), "\\+");
  regex_string = std::regex_replace(regex_string, std::regex("\\/"), "\\/");
  // Convert wildcard specific chars '*?' to their regex equivalents:
  regex_string = std::regex_replace(regex_string, std::regex("\\?"), ".");
  regex_string = std::regex_replace(regex_string, std::regex("\\*"), ".*");

  return std::regex(
      regex_string,
      case_sensitive ? std::regex_constants::ECMAScript : std::regex_constants::icase);
}

std::vector<std::filesystem::path> get_fs_paths(const std::string& file_or_directory) {
  const std::filesystem::path file_or_directory_path(file_or_directory);
  const auto file_status = std::filesystem::status(file_or_directory_path);

  std::vector<std::filesystem::path> fs_paths;
  if (std::filesystem::is_regular_file(file_status)) {
    fs_paths.emplace_back(file_or_directory_path);
    return fs_paths;
  } else if (std::filesystem::is_directory(file_status)) {
    for (std::filesystem::directory_entry const& entry :
         std::filesystem::directory_iterator(file_or_directory_path)) {
      if (std::filesystem::is_regular_file(std::filesystem::status(entry))) {
        fs_paths.emplace_back(entry.path());
      }
    }
    return fs_paths;
  } else {
    const auto parent_path = file_or_directory_path.parent_path();
    const auto parent_status = std::filesystem::status(parent_path);
    if (std::filesystem::is_directory(parent_status)) {
      const auto file_glob = file_or_directory_path.filename();
      const std::regex glob_regex{glob_to_regex(file_glob.string(), false)};

      for (std::filesystem::directory_entry const& entry :
           std::filesystem::directory_iterator(parent_path)) {
        if (std::filesystem::is_regular_file(std::filesystem::status(entry))) {
          const auto entry_filename = entry.path().filename().string();
          if (std::regex_match(entry_filename, glob_regex)) {
            fs_paths.emplace_back(entry.path());
          }
        }
      }
      return fs_paths;
    }
  }
  return fs_paths;
}

}  // namespace FileUtilities

template <typename T>
NEVER_INLINE HOST bool is_valid_tf_input(const T input,
                                         const T bounds_val,
                                         const BoundsType bounds_type,
                                         const IntervalType interval_type) {
  switch (bounds_type) {
    case BoundsType::Min:
      switch (interval_type) {
        case IntervalType::Inclusive:
          return input >= bounds_val;
        case IntervalType::Exclusive:
          return input > bounds_val;
        default:
          UNREACHABLE();
      }
    case BoundsType::Max:
      switch (interval_type) {
        case IntervalType::Inclusive:
          return input <= bounds_val;
        case IntervalType::Exclusive:
          return input < bounds_val;
        default:
          UNREACHABLE();
      }
      break;
    default:
      UNREACHABLE();
  }
  UNREACHABLE();
  return false;  // To address compiler warning
}

template NEVER_INLINE HOST bool is_valid_tf_input(const int32_t input,
                                                  const int32_t bounds_val,
                                                  const BoundsType bounds_type,
                                                  const IntervalType interval_type);

template NEVER_INLINE HOST bool is_valid_tf_input(const int64_t input,
                                                  const int64_t bounds_val,
                                                  const BoundsType bounds_type,
                                                  const IntervalType interval_type);

template NEVER_INLINE HOST bool is_valid_tf_input(const float input,
                                                  const float bounds_val,
                                                  const BoundsType bounds_type,
                                                  const IntervalType interval_type);

template NEVER_INLINE HOST bool is_valid_tf_input(const double input,
                                                  const double bounds_val,
                                                  const BoundsType bounds_type,
                                                  const IntervalType interval_type);

#endif  // #ifndef __CUDACC__
