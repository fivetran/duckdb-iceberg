#pragma once

#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class Binder;
class IcebergTableEntry;
class LogicalOperator;
struct FivetranAIBM25IndexDefinition;

unique_ptr<LogicalOperator> BindFivetranAIBM25Refresh(Binder &binder, IcebergTableEntry &table,
                                                      vector<FivetranAIBM25IndexDefinition> definitions,
                                                      bool persist_definitions);

} // namespace duckdb
