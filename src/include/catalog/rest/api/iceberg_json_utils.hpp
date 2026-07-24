#pragma once

#include "yyjson.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

using namespace duckdb_yyjson;

namespace duckdb {

class IcebergTableSchema;

class IcebergJSONUtils {
public:
	// Populates a view-version JSON object with common fields
	// Used by both CreateViewRequest and ReplaceViewRequest
	static void PopulateViewVersion(yyjson_mut_doc *doc, yyjson_mut_val *view_version_obj, int32_t version_id,
	                                int32_t schema_id, const string &view_sql, const vector<string> &namespace_items);

	// Populates a schema JSON object with fields from IcebergTableSchema
	// Used by CreateTableRequest, CreateViewRequest, and ReplaceViewRequest
	// schema_id of -1 means new schema (server will assign ID)
};
} // namespace duckdb
