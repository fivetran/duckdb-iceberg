#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class IcebergTableSchema;

constexpr const char *FIVETRAN_AI_BM25_PROPERTY_PREFIX = "fivetran.ai.bm25-index.v1.";

struct FivetranAIBM25IndexDefinition {
	string name;
	int32_t stable_id_field_id;
	int32_t content_field_id;
	string stable_id_column;
	string content_column;
};

string FivetranAIBM25PropertyName(const string &index_name);
string FivetranAIBM25PropertyValue(int32_t stable_id_field_id, int32_t content_field_id);
vector<FivetranAIBM25IndexDefinition>
GetFivetranAIBM25IndexDefinitions(const case_insensitive_map_t<string> &properties, const IcebergTableSchema &schema);

} // namespace duckdb
