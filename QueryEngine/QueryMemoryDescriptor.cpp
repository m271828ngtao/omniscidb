/*
 * Copyright 2018 MapD Technologies, Inc.
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

#include "QueryMemoryDescriptor.h"

#include "Execute.h"
#include "GroupByAndAggregate.h"
#include "StreamingTopN.h"

namespace {

bool is_int_and_no_bigger_than(const SQLTypeInfo& ti, const size_t byte_width) {
  if (!ti.is_integer()) {
    return false;
  }
  return get_bit_width(ti) <= (byte_width * 8);
}

}  // namespace

QueryMemoryDescriptor::QueryMemoryDescriptor()
    : executor_(nullptr),
      allow_multifrag(false),
      hash_type(GroupByColRangeType::Projection),
      keyless_hash(false),
      interleaved_bins_on_gpu(false),
      idx_target_as_key(0),
      init_val(0),
#ifdef ENABLE_KEY_COMPACTION
      group_col_compact_width(0),
#endif
      entry_count(0),
      entry_count_small(0),
      min_val(0),
      max_val(0),
      bucket(0),
      has_nulls(false),
      sharing(GroupByMemSharing::Private),
      sort_on_gpu_(false),
      is_sort_plan(false),
      output_columnar(false),
      render_output(false),
      must_use_baseline_sort(false) {
}

QueryMemoryDescriptor::QueryMemoryDescriptor(const Executor* executor,
                                             const size_t entry_count,
                                             const GroupByColRangeType hash_type)
    : executor_(nullptr),
      allow_multifrag(false),
      hash_type(hash_type),
      keyless_hash(false),
      interleaved_bins_on_gpu(false),
      idx_target_as_key(0),
      init_val(0),
#ifdef ENABLE_KEY_COMPACTION
      group_col_compact_width(0),
#endif
      entry_count(entry_count),
      entry_count_small(0),
      min_val(0),
      max_val(0),
      bucket(0),
      has_nulls(false),
      sharing(GroupByMemSharing::Private),
      sort_on_gpu_(false),
      is_sort_plan(false),
      output_columnar(false),
      render_output(false),
      must_use_baseline_sort(false) {}

QueryMemoryDescriptor::QueryMemoryDescriptor(const Executor* executor,
                                             const bool allow_multifrag,
                                             const GroupByColRangeType hash_type,
                                             const bool keyless_hash,
                                             const bool interleaved_bins_on_gpu,
                                             const int32_t idx_target_as_key,
                                             const int64_t init_val,
                                             const std::vector<int8_t>& group_col_widths,
#ifdef ENABLE_KEY_COMPACTION
                                             const int8_t group_col_compact_width,
#endif
                                             const std::vector<ColWidths>& agg_col_widths,
                                             const std::vector<ssize_t>& target_groupby_indices,
                                             const size_t entry_count,
                                             const size_t entry_count_small,
                                             const int64_t min_val,
                                             const int64_t max_val,
                                             const int64_t bucket,
                                             const bool has_nulls,
                                             const GroupByMemSharing sharing,
                                             const CountDistinctDescriptors count_distinct_descriptors,
                                             const bool sort_on_gpu,
                                             const bool is_sort_plan,
                                             const bool output_columnar,
                                             const bool render_output,
                                             const std::vector<int8_t>& key_column_pad_bytes,
                                             const std::vector<int8_t>& target_column_pad_bytes,
                                             const bool must_use_baseline_sort)
    : executor_(executor),
      allow_multifrag(allow_multifrag),
      hash_type(hash_type),
      keyless_hash(keyless_hash),
      interleaved_bins_on_gpu(interleaved_bins_on_gpu),
      idx_target_as_key(idx_target_as_key),
      init_val(init_val),
      group_col_widths(group_col_widths),
#ifdef ENABLE_KEY_COMPACTION
      group_col_compact_width(group_col_compact_width),
#endif
      agg_col_widths(agg_col_widths),
      target_groupby_indices(target_groupby_indices),
      entry_count(entry_count),
      entry_count_small(entry_count_small),
      min_val(min_val),
      max_val(max_val),
      bucket(bucket),
      has_nulls(has_nulls),
      sharing(sharing),
      count_distinct_descriptors_(count_distinct_descriptors),
      sort_on_gpu_(sort_on_gpu),
      is_sort_plan(is_sort_plan),
      output_columnar(output_columnar),
      render_output(render_output),
      key_column_pad_bytes(key_column_pad_bytes),
      target_column_pad_bytes(target_column_pad_bytes),
      must_use_baseline_sort(must_use_baseline_sort) {
}

std::unique_ptr<QueryExecutionContext> QueryMemoryDescriptor::getQueryExecutionContext(
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<int64_t>& init_agg_vals,
    const Executor* executor,
    const ExecutorDeviceType device_type,
    const int device_id,
    const std::vector<std::vector<const int8_t*>>& col_buffers,
    const std::vector<std::vector<const int8_t*>>& iter_buffers,
    const std::vector<std::vector<uint64_t>>& frag_offsets,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    const bool output_columnar,
    const bool sort_on_gpu,
    RenderInfo* render_info) const {
  return std::unique_ptr<QueryExecutionContext>(new QueryExecutionContext(ra_exe_unit,
                                                                          *this,
                                                                          init_agg_vals,
                                                                          executor,
                                                                          device_type,
                                                                          device_id,
                                                                          col_buffers,
                                                                          iter_buffers,
                                                                          frag_offsets,
                                                                          row_set_mem_owner,
                                                                          output_columnar,
                                                                          sort_on_gpu,
                                                                          render_info));
}

int8_t QueryMemoryDescriptor::pick_target_compact_width(const RelAlgExecutionUnit& ra_exe_unit,
                                                        const std::vector<InputTableInfo>& query_infos,
                                                        const int8_t crt_min_byte_width) {
  if (g_bigint_count) {
    return sizeof(int64_t);
  }
  int8_t compact_width{0};
  auto col_it = ra_exe_unit.input_col_descs.begin();
  int unnest_array_col_id{std::numeric_limits<int>::min()};
  for (const auto groupby_expr : ra_exe_unit.groupby_exprs) {
    const auto uoper = dynamic_cast<Analyzer::UOper*>(groupby_expr.get());
    if (uoper && uoper->get_optype() == kUNNEST) {
      const auto& arg_ti = uoper->get_operand()->get_type_info();
      CHECK(arg_ti.is_array());
      const auto& elem_ti = arg_ti.get_elem_type();
      if (elem_ti.is_string() && elem_ti.get_compression() == kENCODING_DICT) {
        unnest_array_col_id = (*col_it)->getColId();
      } else {
        compact_width = crt_min_byte_width;
        break;
      }
    }
    ++col_it;
  }
  if (!compact_width && (ra_exe_unit.groupby_exprs.size() != 1 || !ra_exe_unit.groupby_exprs.front())) {
    compact_width = crt_min_byte_width;
  }
  if (!compact_width) {
    col_it = ra_exe_unit.input_col_descs.begin();
    std::advance(col_it, ra_exe_unit.groupby_exprs.size());
    for (const auto target : ra_exe_unit.target_exprs) {
      const auto& ti = target->get_type_info();
      const auto agg = dynamic_cast<const Analyzer::AggExpr*>(target);
      if (agg && agg->get_arg()) {
        compact_width = crt_min_byte_width;
        break;
      }

      if (agg) {
        CHECK_EQ(kCOUNT, agg->get_aggtype());
        CHECK(!agg->get_is_distinct());
        ++col_it;
        continue;
      }

      if (is_int_and_no_bigger_than(ti, 4) || (ti.is_string() && ti.get_compression() == kENCODING_DICT)) {
        ++col_it;
        continue;
      }

      const auto uoper = dynamic_cast<Analyzer::UOper*>(target);
      if (uoper && uoper->get_optype() == kUNNEST && (*col_it)->getColId() == unnest_array_col_id) {
        const auto arg_ti = uoper->get_operand()->get_type_info();
        CHECK(arg_ti.is_array());
        const auto& elem_ti = arg_ti.get_elem_type();
        if (elem_ti.is_string() && elem_ti.get_compression() == kENCODING_DICT) {
          ++col_it;
          continue;
        }
      }

      compact_width = crt_min_byte_width;
      break;
    }
  }
  if (!compact_width) {
    size_t total_tuples{0};
    for (const auto& qi : query_infos) {
      total_tuples += qi.info.getNumTuples();
    }
    return total_tuples <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()) ||
                   unnest_array_col_id != std::numeric_limits<int>::min()
               ? 4
               : crt_min_byte_width;
  } else {
    // TODO(miyu): relax this condition to allow more cases just w/o padding
    for (auto wid : get_col_byte_widths(ra_exe_unit.target_exprs, {})) {
      compact_width = std::max(compact_width, wid);
    }
    return compact_width;
  }
}

size_t QueryMemoryDescriptor::getColsSize() const {
  size_t total_bytes{0};
  for (size_t col_idx = 0; col_idx < agg_col_widths.size(); ++col_idx) {
    auto chosen_bytes = agg_col_widths[col_idx].compact;
    if (chosen_bytes == sizeof(int64_t)) {
      total_bytes = align_to_int64(total_bytes);
    }
    total_bytes += chosen_bytes;
  }
  return total_bytes;
}

size_t QueryMemoryDescriptor::getRowSize() const {
  CHECK(!output_columnar);
  size_t total_bytes{0};
  if (keyless_hash) {
    CHECK_EQ(size_t(1), group_col_widths.size());
  } else {
    total_bytes += group_col_widths.size() * getEffectiveKeyWidth();
    total_bytes = align_to_int64(total_bytes);
  }
  total_bytes += getColsSize();
  return align_to_int64(total_bytes);
}

size_t QueryMemoryDescriptor::getWarpCount() const {
  return (interleaved_bins_on_gpu ? executor_->warpSize() : 1);
}

size_t QueryMemoryDescriptor::getCompactByteWidth() const {
  if (agg_col_widths.empty()) {
    return 8;
  }
  size_t compact_width{0};
  for (const auto col_width : agg_col_widths) {
    if (col_width.compact != 0) {
      compact_width = col_width.compact;
      break;
    }
  }
  if (!compact_width) {
    return 0;
  }
  CHECK_GT(compact_width, size_t(0));
  for (const auto col_width : agg_col_widths) {
    if (col_width.compact == 0) {
      continue;
    }
    CHECK_EQ(col_width.compact, compact_width);
  }
  return compact_width;
}

// TODO(miyu): remove if unnecessary
bool QueryMemoryDescriptor::isCompactLayoutIsometric() const {
  if (agg_col_widths.empty()) {
    return true;
  }
  const auto compact_width = agg_col_widths.front().compact;
  for (const auto col_width : agg_col_widths) {
    if (col_width.compact != compact_width) {
      return false;
    }
  }
  return true;
}

size_t QueryMemoryDescriptor::getTotalBytesOfColumnarBuffers(const std::vector<ColWidths>& col_widths) const {
  CHECK(output_columnar);
  size_t total_bytes{0};
  const auto is_isometric = isCompactLayoutIsometric();
  for (size_t col_idx = 0; col_idx < col_widths.size(); ++col_idx) {
    total_bytes += col_widths[col_idx].compact * entry_count;
    if (!is_isometric) {
      total_bytes = align_to_int64(total_bytes);
    }
  }
  return align_to_int64(total_bytes);
}

size_t QueryMemoryDescriptor::getKeyOffInBytes(const size_t bin, const size_t key_idx) const {
  CHECK(!keyless_hash);
  if (output_columnar) {
    CHECK_EQ(size_t(0), key_idx);
    return bin * sizeof(int64_t);
  }

  CHECK_LT(key_idx, group_col_widths.size());
  auto offset = bin * getRowSize();
  CHECK_EQ(size_t(0), offset % sizeof(int64_t));
  offset += key_idx * getEffectiveKeyWidth();
  return offset;
}

size_t QueryMemoryDescriptor::getNextKeyOffInBytes(const size_t crt_idx) const {
  CHECK(!keyless_hash);
  CHECK_LT(crt_idx, group_col_widths.size());
  if (output_columnar) {
    CHECK_EQ(size_t(0), crt_idx);
  }
  return getEffectiveKeyWidth();
}

size_t QueryMemoryDescriptor::getColOnlyOffInBytes(const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths.size());
  size_t offset{0};
  for (size_t index = 0; index < col_idx; ++index) {
    const auto chosen_bytes = agg_col_widths[index].compact;
    if (chosen_bytes == sizeof(int64_t)) {
      offset = align_to_int64(offset);
    }
    offset += chosen_bytes;
  }

  if (sizeof(int64_t) == agg_col_widths[col_idx].compact) {
    offset = align_to_int64(offset);
  }

  return offset;
}

size_t QueryMemoryDescriptor::getColOffInBytes(const size_t bin, const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths.size());
  const auto warp_count = getWarpCount();
  if (output_columnar) {
    CHECK_LT(bin, entry_count);
    CHECK_EQ(size_t(1), group_col_widths.size());
    CHECK_EQ(size_t(1), warp_count);
    size_t offset{0};
    const auto is_isometric = isCompactLayoutIsometric();
    if (!keyless_hash) {
      offset = sizeof(int64_t) * entry_count;
    }
    for (size_t index = 0; index < col_idx; ++index) {
      offset += agg_col_widths[index].compact * entry_count;
      if (!is_isometric) {
        offset = align_to_int64(offset);
      }
    }
    offset += bin * agg_col_widths[col_idx].compact;
    return offset;
  }

  auto offset = bin * warp_count * getRowSize();
  if (keyless_hash) {
    CHECK_EQ(size_t(1), group_col_widths.size());
  } else {
    offset += group_col_widths.size() * getEffectiveKeyWidth();
    offset = align_to_int64(offset);
  }
  offset += getColOnlyOffInBytes(col_idx);
  return offset;
}

size_t QueryMemoryDescriptor::getConsistColOffInBytes(const size_t bin, const size_t col_idx) const {
  CHECK(output_columnar && !agg_col_widths.empty());
  return (keyless_hash ? 0 : sizeof(int64_t) * entry_count) + (col_idx * entry_count + bin) * agg_col_widths[0].compact;
}

size_t QueryMemoryDescriptor::getColOffInBytesInNextBin(const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths.size());
  auto warp_count = getWarpCount();
  if (output_columnar) {
    CHECK_EQ(size_t(1), group_col_widths.size());
    CHECK_EQ(size_t(1), warp_count);
    return agg_col_widths[col_idx].compact;
  }

  return warp_count * getRowSize();
}

size_t QueryMemoryDescriptor::getNextColOffInBytes(const int8_t* col_ptr,
                                                   const size_t bin,
                                                   const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths.size());
  CHECK(!output_columnar || bin < entry_count);
  size_t offset{0};
  auto warp_count = getWarpCount();
  const auto chosen_bytes = agg_col_widths[col_idx].compact;
  if (col_idx + 1 == agg_col_widths.size()) {
    if (output_columnar) {
      return (entry_count - bin) * chosen_bytes;
    } else {
      return static_cast<size_t>(align_to_int64(col_ptr + chosen_bytes) - col_ptr);
    }
  }

  const auto next_chosen_bytes = agg_col_widths[col_idx + 1].compact;
  if (output_columnar) {
    CHECK_EQ(size_t(1), group_col_widths.size());
    CHECK_EQ(size_t(1), warp_count);

    offset = entry_count * chosen_bytes;
    if (!isCompactLayoutIsometric()) {
      offset = align_to_int64(offset);
    }
    offset += bin * (next_chosen_bytes - chosen_bytes);
    return offset;
  }

  if (next_chosen_bytes == sizeof(int64_t)) {
    return static_cast<size_t>(align_to_int64(col_ptr + chosen_bytes) - col_ptr);
  } else {
    return chosen_bytes;
  }
}

size_t QueryMemoryDescriptor::getBufferSizeQuad(const ExecutorDeviceType device_type) const {
  const auto size_bytes = getBufferSizeBytes(device_type);
  CHECK_EQ(size_t(0), size_bytes % sizeof(int64_t));
  return getBufferSizeBytes(device_type) / sizeof(int64_t);
}

size_t QueryMemoryDescriptor::getSmallBufferSizeQuad() const {
  CHECK(!keyless_hash || entry_count_small == 0);
  return (group_col_widths.size() + agg_col_widths.size()) * entry_count_small;
}

size_t QueryMemoryDescriptor::getBufferSizeBytes(const RelAlgExecutionUnit& ra_exe_unit,
                                                 const unsigned thread_count,
                                                 const ExecutorDeviceType device_type) const {
  if (use_streaming_top_n(ra_exe_unit, *this)) {
    const size_t n = ra_exe_unit.sort_info.offset + ra_exe_unit.sort_info.limit;
    return streaming_top_n::get_heap_size(getRowSize(), n, thread_count);
  }
  return getBufferSizeBytes(device_type);
}

size_t QueryMemoryDescriptor::getBufferSizeBytes(const ExecutorDeviceType device_type) const {
  if (keyless_hash) {
    CHECK_GE(group_col_widths.size(), size_t(1));
    auto total_bytes = align_to_int64(getColsSize());

    return (interleavedBins(device_type) ? executor_->warpSize() : 1) * entry_count * total_bytes;
  }

  size_t total_bytes{0};
  if (output_columnar) {
    total_bytes =
        sizeof(int64_t) * group_col_widths.size() * entry_count + getTotalBytesOfColumnarBuffers(agg_col_widths);
  } else {
    total_bytes = getRowSize() * entry_count;
  }

  return total_bytes;
}

size_t QueryMemoryDescriptor::getSmallBufferSizeBytes() const {
  return getSmallBufferSizeQuad() * sizeof(int64_t);
}

bool QueryMemoryDescriptor::usesGetGroupValueFast() const {
  return (hash_type == GroupByColRangeType::OneColKnownRange && !getSmallBufferSizeBytes());
}

bool QueryMemoryDescriptor::usesCachedContext() const {
  return allow_multifrag && (usesGetGroupValueFast() || hash_type == GroupByColRangeType::MultiColPerfectHash);
}

bool QueryMemoryDescriptor::threadsShareMemory() const {
  return sharing == GroupByMemSharing::Shared || sharing == GroupByMemSharing::SharedForKeylessOneColumnKnownRange;
}

bool QueryMemoryDescriptor::blocksShareMemory() const {
  if (g_cluster) {
    return true;
  }
  if (!countDescriptorsLogicallyEmpty(count_distinct_descriptors_)) {
    return true;
  }
  if (executor_->isCPUOnly() || render_output || hash_type == GroupByColRangeType::MultiCol ||
      hash_type == GroupByColRangeType::Projection || hash_type == GroupByColRangeType::MultiColPerfectHash) {
    return true;
  }
  return usesCachedContext() && !sharedMemBytes(ExecutorDeviceType::GPU) && many_entries(max_val, min_val, bucket);
}

bool QueryMemoryDescriptor::lazyInitGroups(const ExecutorDeviceType device_type) const {
  return device_type == ExecutorDeviceType::GPU && !render_output && !getSmallBufferSizeQuad() &&
         countDescriptorsLogicallyEmpty(count_distinct_descriptors_);
}

bool QueryMemoryDescriptor::interleavedBins(const ExecutorDeviceType device_type) const {
  return interleaved_bins_on_gpu && device_type == ExecutorDeviceType::GPU;
}

size_t QueryMemoryDescriptor::sharedMemBytes(const ExecutorDeviceType device_type) const {
  CHECK(device_type == ExecutorDeviceType::CPU || device_type == ExecutorDeviceType::GPU);
  if (device_type == ExecutorDeviceType::CPU) {
    return 0;
  }
  // if performing keyless aggregate query with a single column group-by:
  if (sharing == GroupByMemSharing::SharedForKeylessOneColumnKnownRange) {
    CHECK_EQ(getRowSize(), sizeof(int64_t));  // Currently just designed for this scenario
    size_t shared_mem_size = (/*bin_count=*/entry_count + 1) * sizeof(int64_t);  // one extra for NULL values
    CHECK(shared_mem_size <= executor_->getCatalog()->get_dataMgr().cudaMgr_->maxSharedMemoryForAll);
    return shared_mem_size;
  }
  const size_t shared_mem_threshold{0};
  const size_t shared_mem_bytes{getBufferSizeBytes(ExecutorDeviceType::GPU)};
  if (!usesGetGroupValueFast() || shared_mem_bytes > shared_mem_threshold) {
    return 0;
  }
  return shared_mem_bytes;
}

bool QueryMemoryDescriptor::isWarpSyncRequired(const ExecutorDeviceType device_type) const {
  if (device_type != ExecutorDeviceType::GPU) {
    return false;
  } else {
    auto cuda_manager = executor_->getCatalog()->get_dataMgr().cudaMgr_;
    CHECK(cuda_manager);
    return cuda_manager->isArchVoltaForAll();
  }
}

bool QueryMemoryDescriptor::canOutputColumnar() const {
  return usesGetGroupValueFast() && threadsShareMemory() && blocksShareMemory() &&
         !interleavedBins(ExecutorDeviceType::GPU) && countDescriptorsLogicallyEmpty(count_distinct_descriptors_);
}

bool QueryMemoryDescriptor::sortOnGpu() const {
  return sort_on_gpu_;
}