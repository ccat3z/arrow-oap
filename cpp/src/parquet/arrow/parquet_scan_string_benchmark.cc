/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arrow/filesystem/filesystem.h>
#include <arrow/io/interfaces.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/testing/gtest_util.h>
#include <arrow/type.h>
#include <arrow/util/io_util.h>
#include <benchmark/benchmark.h>
#include <gtest/gtest.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>

#include <chrono>

#include "arrow/record_batch.h"
#include "parquet/arrow/utils/macros.h"
#include "parquet/arrow/test_utils.h"


// namespace parquet {
// namespace benchmark {

const int batch_buffer_size = 32768;
// const int batch_buffer_size = 2;

class GoogleBenchmarkParquetStringScan {
 public:
  GoogleBenchmarkParquetStringScan(std::string file_name) { GetRecordBatchReader(file_name); }

  void GetRecordBatchReader(const std::string& input_file) {
    std::unique_ptr<::parquet::arrow::FileReader> parquet_reader;
    std::shared_ptr<arrow::RecordBatchReader> record_batch_reader;

    std::shared_ptr<arrow::fs::FileSystem> fs;
    std::string file_name;
    ARROW_ASSIGN_OR_THROW(fs, arrow::fs::FileSystemFromUriOrPath(input_file, &file_name))

    ARROW_ASSIGN_OR_THROW(file, fs->OpenInputFile(file_name));

    properties.set_batch_size(batch_buffer_size);
    properties.set_pre_buffer(false);
    properties.set_use_threads(false);

    ASSERT_NOT_OK(::parquet::arrow::FileReader::Make(
        arrow::default_memory_pool(), ::parquet::ParquetFileReader::Open(file),
        properties, &parquet_reader));

    ASSERT_NOT_OK(parquet_reader->GetSchema(&schema));

    auto num_rowgroups = parquet_reader->num_row_groups();

    for (int i = 0; i < num_rowgroups; ++i) {
      row_group_indices.push_back(i);
    }

    auto num_columns = schema->num_fields();
    std::cout << "Enter Is_binary_like Check: " << std::endl;
    for (int i = 0; i < num_columns; ++i) {
      auto field = schema->field(i);
      auto type = field->type();
      if (arrow::is_binary_like(type->id())) {
        std::cout << "Is_binary_like colIndex: " << i << std::endl;
        column_indices.push_back(i);
      }
    }
  }

  virtual void operator()(benchmark::State& state) {}

 protected:
  long SetCPU(uint32_t cpuindex) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpuindex, &cs);
    return sched_setaffinity(0, sizeof(cs), &cs);
  }

 protected:
  std::string file_name;
  std::shared_ptr<arrow::io::RandomAccessFile> file;
  std::vector<int> row_group_indices;
  std::vector<int> column_indices;
  std::shared_ptr<arrow::Schema> schema;
  parquet::ArrowReaderProperties properties;
};
class GoogleBenchmarkParquetStringScan_IteratorScan_Benchmark
    : public GoogleBenchmarkParquetStringScan {
 public:
  GoogleBenchmarkParquetStringScan_IteratorScan_Benchmark(std::string filename)
      : GoogleBenchmarkParquetStringScan(filename) {}
  void operator()(benchmark::State& state) {
    if (state.range(0) == 0xffffffff) {
      SetCPU(state.thread_index());
    } else {
      SetCPU(state.range(0));
    }

    arrow::Compression::type compression_type = (arrow::Compression::type)1;

    std::shared_ptr<arrow::RecordBatch> record_batch;
    int64_t elapse_read = 0;
    int64_t num_batches = 0;
    int64_t num_rows = 0;
    int64_t init_time = 0;
    int64_t write_time = 0;


    std::vector<int> local_column_indices = column_indices;

    for (auto val : local_column_indices){
      std::cout << "local_column_indices: is_binary_like colIndex: " << val << std::endl;
    }

    std::shared_ptr<arrow::Schema> local_schema;
    local_schema = std::make_shared<arrow::Schema>(*schema.get());

    if (state.thread_index() == 0) std::cout << local_schema->ToString() << std::endl;

    for (auto _ : state) {
      std::unique_ptr<::parquet::arrow::FileReader> parquet_reader;
      std::shared_ptr<arrow::RecordBatchReader> record_batch_reader;
      ASSERT_NOT_OK(::parquet::arrow::FileReader::Make(
          ::arrow::default_memory_pool(), ::parquet::ParquetFileReader::Open(file),
          properties, &parquet_reader));

      std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
      ASSERT_NOT_OK(parquet_reader->GetRecordBatchReader(
          row_group_indices, local_column_indices, &record_batch_reader));
      do {
        TIME_NANO_OR_THROW(elapse_read, record_batch_reader->ReadNext(&record_batch));

        if (record_batch) {
          // batches.push_back(record_batch);
          num_batches += 1;
          num_rows += record_batch->num_rows();
        }
      } while (record_batch);

      std::cout << " parquet parse done elapsed time = " << elapse_read / 1000000
              << " rows = " << num_rows << std::endl;
    }

    state.counters["rowgroups"] =
        benchmark::Counter(row_group_indices.size(), benchmark::Counter::kAvgThreads,
                           benchmark::Counter::OneK::kIs1000);
    state.counters["columns"] =
        benchmark::Counter(column_indices.size(), benchmark::Counter::kAvgThreads,
                           benchmark::Counter::OneK::kIs1000);
    state.counters["batches"] = benchmark::Counter(
        num_batches, benchmark::Counter::kAvgThreads, benchmark::Counter::OneK::kIs1000);
    state.counters["num_rows"] = benchmark::Counter(
        num_rows, benchmark::Counter::kAvgThreads, benchmark::Counter::OneK::kIs1000);
    state.counters["batch_buffer_size"] =
        benchmark::Counter(batch_buffer_size, benchmark::Counter::kAvgThreads,
                           benchmark::Counter::OneK::kIs1024);

    state.counters["parquet_parse"] = benchmark::Counter(
        elapse_read, benchmark::Counter::kAvgThreads, benchmark::Counter::OneK::kIs1000);
    state.counters["init_time"] = benchmark::Counter(
        init_time, benchmark::Counter::kAvgThreads, benchmark::Counter::OneK::kIs1000);
    state.counters["write_time"] = benchmark::Counter(
        write_time, benchmark::Counter::kAvgThreads, benchmark::Counter::OneK::kIs1000);
  }
};

// }  // namespace ParquetStringScan
// }  // namespace sparkcolumnarplugin

int main(int argc, char** argv) {
  uint32_t iterations = 1;
  uint32_t threads = 1;
  std::string datafile;
  uint32_t cpu = 0xffffffff;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--iterations") == 0) {
      iterations = atol(argv[i + 1]);
    } else if (strcmp(argv[i], "--threads") == 0) {
      threads = atol(argv[i + 1]);
    } else if (strcmp(argv[i], "--file") == 0) {
      datafile = argv[i + 1];
    } else if (strcmp(argv[i], "--cpu") == 0) {
      cpu = atol(argv[i + 1]);
    }
  }
  std::cout << "iterations = " << iterations << std::endl;
  std::cout << "threads = " << threads << std::endl;
  std::cout << "datafile = " << datafile << std::endl;
  std::cout << "cpu = " << cpu << std::endl;

  GoogleBenchmarkParquetStringScan_IteratorScan_Benchmark
      bck(datafile);

  benchmark::RegisterBenchmark("GoogleBenchmarkParquetStringScan::IteratorScan", bck)
      ->Args({
          cpu,
      })
      ->Iterations(iterations)
      ->Threads(threads)
      ->ReportAggregatesOnly(false)
      ->MeasureProcessCPUTime()
      ->Unit(benchmark::kSecond);

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
}