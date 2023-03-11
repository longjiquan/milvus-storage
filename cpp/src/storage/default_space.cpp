#include "default_space.h"

#include <arrow/type.h>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cassert>
#include <memory>
#include <numeric>
#include <utility>

#include "../exception.h"
#include "../format/parquet/file_writer.h"
#include "../reader/record_reader.h"
#include "arrow/array/builder_primitive.h"
#include "arrow/array/util.h"
#include "arrow/filesystem/mockfs.h"
#include "arrow/record_batch.h"

void WriteManifestFile(const Manifest *manifest);

DefaultSpace::DefaultSpace(std::shared_ptr<arrow::Schema> schema,
                           std::shared_ptr<SpaceOption> &options)
    : Space(options) {
  if (!schema->GetFieldByName(options->primary_column) ||
      !options->version_column.empty() &&
          !schema->GetFieldByName(options->version_column)) {
    throw StorageException("version column not found");
  }

  arrow::SchemaBuilder scalar_schema_builder;
  arrow::SchemaBuilder vector_schema_builder;

  for (const auto &field : schema->fields()) {
    if (field->name() == options->primary_column ||
        field->name() == options->version_column) {
      vector_schema_builder.AddField(field);
      scalar_schema_builder.AddField(field);
    } else if (field->name() == options->vector_column) {
      vector_schema_builder.AddField(field);
    } else {
      scalar_schema_builder.AddField(field);
    }
  }

  scalar_schema_builder.AddField(
      std::make_shared<arrow::Field>(kOffsetFieldName, arrow::int64()));

  manifest_ = std::make_unique<Manifest>(
      schema, scalar_schema_builder.Finish().ValueOrDie(),
      vector_schema_builder.Finish().ValueOrDie());
  fs_ =
      std::make_unique<arrow::fs::internal::MockFileSystem>(arrow::fs::kNoTime);
}

void DefaultSpace::Write(arrow::RecordBatchReader *reader,
                         WriteOption *option) {
  if (!reader->schema()->Equals(*this->manifest_->get_schema())) {
    throw StorageException("schema not match");
  }

  // remove duplicated codes
  auto scalar_schema = this->manifest_->get_scalar_schema(),
       vector_schema = this->manifest_->get_vector_schema();

  std::vector<std::shared_ptr<arrow::Array>> scalar_cols;
  std::vector<std::shared_ptr<arrow::Array>> vector_cols;

  FileWriter *scalar_writer = nullptr;
  FileWriter *vector_writer = nullptr;

  std::vector<std::string> scalar_files;
  std::vector<std::string> vector_files;

  int64_t offset = 1;

  for (auto rec = reader->Next(); rec.ok(); rec = reader->Next()) {
    auto batch = rec.ValueOrDie();
    if (batch->num_rows() == 0) {
      continue;
    }
    auto cols = batch->columns();
    for (int i = 0; i < cols.size(); ++i) {
      if (scalar_schema->GetFieldByName(batch->column_name(i))) {
        scalar_cols.emplace_back(cols[i]);
      }
      if (vector_schema->GetFieldByName(batch->column_name(i))) {
        vector_cols.emplace_back(cols[i]);
      }
    }

    // add offset column
    std::vector<int64_t> offset_values(batch->num_rows());
    std::iota(offset_values.begin(), offset_values.end(), offset);
    arrow::NumericBuilder<arrow::Int64Type> builder;
    auto offset_col = builder.AppendValues(offset_values);
    scalar_cols.emplace_back(builder.Finish());

    auto scalar_record =
        arrow::RecordBatch::Make(scalar_schema, batch->num_rows(), scalar_cols);
    auto vector_record =
        arrow::RecordBatch::Make(vector_schema, batch->num_rows(), vector_cols);

    if (!scalar_writer) {
      auto scalar_file_id = boost::uuids::random_generator()();
      auto scalar_file_path =
          boost::uuids::to_string(scalar_file_id) + ".parquet";
      ParquetFileWriter new_scalar_writer(scalar_schema.get(), fs_.get(),
                                          scalar_file_path);
      scalar_writer = &new_scalar_writer;

      auto vector_file_id = boost::uuids::random_generator()();
      auto vector_file_path =
          boost::uuids::to_string(vector_file_id) + ".parquet";
      ParquetFileWriter new_vector_writer(vector_schema.get(), fs_.get(),
                                          vector_file_path);
      vector_writer = &new_vector_writer;

      scalar_files.emplace_back(scalar_file_path);
      vector_files.emplace_back(vector_file_path);
    }

    scalar_writer->Write(scalar_record.get());
    vector_writer->Write(vector_record.get());

    if (scalar_writer->count() >= option->max_record_per_file) {
      scalar_writer->Close();
      vector_writer->Close();
      scalar_writer = nullptr;
      vector_writer = nullptr;
      offset = 1;
    }
  }

  if (scalar_writer) {
    scalar_writer->Close();
    vector_writer->Close();
    scalar_writer = nullptr;
    vector_writer = nullptr;
  }

  manifest_->AddDataFiles(scalar_files, vector_files);
  WriteManifestFile(manifest_.get());
}

std::unique_ptr<arrow::RecordBatchReader> DefaultSpace::Read(
    std::shared_ptr<ReadOption> option) {
  return RecordReader::GetRecordReader(this, options_.get());
}

void WriteManifestFile(const Manifest *manifest) {}