#pragma once

#include "catalog/rest/api/catalog_utils.hpp"
#include "core/metadata/schema/iceberg_table_schema.hpp"
#include "rest_catalog/objects/view_metadata.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"

using namespace duckdb_yyjson;
namespace duckdb {

struct YyjsonDocDeleter;

struct IcebergReplaceViewRequest {
	// new_schema_id >= 0: add a new schema with this ID; -1: reuse existing schema from schema->schema_id
	IcebergReplaceViewRequest(const string &view_name, const string &view_uuid, const string &view_sql,
	                          const uint32_t version_id, shared_ptr<IcebergTableSchema> schema, int32_t new_schema_id,
	                          const vector<string> &namespace_items);

public:
	string ReplaceViewToJSON(std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p);

private:
	string view_name;
	string view_uuid;
	string view_sql;
	uint32_t version_id;
	shared_ptr<IcebergTableSchema> schema;
	int32_t new_schema_id;
	vector<string> namespace_items;
};

} // namespace duckdb
