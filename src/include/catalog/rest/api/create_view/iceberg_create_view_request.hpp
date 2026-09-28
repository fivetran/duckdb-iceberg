#pragma once

#include "catalog/rest/api/catalog_utils.hpp"
#include "core/metadata/schema/iceberg_table_schema.hpp"
#include "rest_catalog/objects/create_view_request.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"

using namespace duckdb_yyjson;
namespace duckdb {

struct YyjsonDocDeleter;

struct IcebergCreateViewRequest {
	IcebergCreateViewRequest(const string &view_name, const string &view_sql, shared_ptr<IcebergTableSchema> schema,
	                         const vector<string> &namespace_items);

public:
	string CreateViewToJSON(std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p);

private:
	string view_name;
	string view_sql;
	shared_ptr<IcebergTableSchema> schema;
	vector<string> namespace_items;
};

} // namespace duckdb
