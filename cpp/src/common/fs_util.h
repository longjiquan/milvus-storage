#pragma once
#include <arrow/filesystem/filesystem.h>
#include <string>
#include "result.h"
namespace milvus_storage {

Result<std::shared_ptr<arrow::fs::FileSystem>> BuildFileSystem(const std::string& uri);

}