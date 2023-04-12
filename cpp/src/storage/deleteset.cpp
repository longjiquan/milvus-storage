#include "storage/deleteset.h"
#include "arrow/array/array_primitive.h"
#include "arrow/result.h"
#include "arrow/type_fwd.h"
#include "common/macro.h"
#include "reader/scan_record_reader.h"
#include "storage/default_space.h"
#include "storage/manifest.h"
#include <arrow/array/array_binary.h>
#include <arrow/type_fwd.h>
#include <arrow/type_traits.h>
#include <cstdint>
#include <memory>
#include "common/macro.h"
namespace milvus_storage {

arrow::Status DeleteSetVisitor::Visit(const arrow::Int64Array& array) {
  for (int i = 0; i < array.length(); ++i) {
    auto value = array.Value(i);
    if (delete_set_.contains(value)) {
      delete_set_.at(value).push_back(version_col_->Value(i));
    } else {
      delete_set_.emplace(value, std::vector<int64_t>{version_col_->Value(i)});
    }
  }
  return arrow::Status::OK();
}

arrow::Status DeleteSetVisitor::Visit(const arrow::StringArray& array) {
  for (int i = 0; i < array.length(); ++i) {
    auto value = array.Value(i);
    if (delete_set_.contains(value)) {
      delete_set_.at(value).push_back(version_col_->Value(i));
    } else {
      delete_set_.emplace(value, std::vector<int64_t>());
    }
  }
  return arrow::Status::OK();
}

DeleteSet::DeleteSet(const DefaultSpace& space) : space_(space) {}

Status DeleteSet::Build() {
  const auto& delete_files = space_.manifest_->delete_files();
  auto option = std::make_shared<ReadOptions>();
  ScanRecordReader rec_reader(option, delete_files, space_);

  for (const auto& batch_rec : rec_reader) {
    ASSIGN_OR_RETURN_ARROW_NOT_OK(auto batch, batch_rec);
    Add(batch);
  }
  RETURN_ARROW_NOT_OK(rec_reader.Close());
  return Status::OK();
}

Status DeleteSet::Add(std::shared_ptr<arrow::RecordBatch>& batch) {
  auto schema_options = space_.schema_->options();
  auto pk_col = batch->GetColumnByName(schema_options->primary_column);
  auto vec_col = batch->GetColumnByName(schema_options->version_column);

  auto int64_version_col = std::static_pointer_cast<arrow::Int64Array>(vec_col);
  DeleteSetVisitor visitor(data_, int64_version_col);
  RETURN_ARROW_NOT_OK(pk_col->Accept(&visitor));
  return Status::OK();
}

std::vector<int64_t> DeleteSet::GetVersionByPk(pk_type& pk) {
  if (data_.contains(pk)) {
    return data_.at(pk);
  }
  return {};
}
}  // namespace milvus_storage