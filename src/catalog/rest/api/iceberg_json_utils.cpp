#include "catalog/rest/api/iceberg_json_utils.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "core/metadata/schema/iceberg_table_schema.hpp"
#include "core/metadata/schema/iceberg_column_definition.hpp"
#include "catalog/rest/api/iceberg_type.hpp"

namespace duckdb {

// Forward declarations for internal helper functions

void IcebergJSONUtils::PopulateViewVersion(yyjson_mut_doc *doc, yyjson_mut_val *view_version_obj, int32_t version_id,
                                           int32_t schema_id, const string &view_sql,
                                           const vector<string> &namespace_items) {
	// version-id
	yyjson_mut_obj_add_int(doc, view_version_obj, "version-id", version_id);

	// timestamp-ms: current timestamp in milliseconds
	auto timestamp_ms = Timestamp::GetEpochMs(Timestamp::GetCurrentTimestamp());
	yyjson_mut_obj_add_uint(doc, view_version_obj, "timestamp-ms", timestamp_ms);

	// schema-id
	yyjson_mut_obj_add_int(doc, view_version_obj, "schema-id", schema_id);

	// summary: metadata about the view version
	auto summary_obj = yyjson_mut_obj_add_obj(doc, view_version_obj, "summary");
	yyjson_mut_obj_add_strcpy(doc, summary_obj, "engine-name", "duckdb");

	// representations: array of view representations (SQL representation)
	auto representations_arr = yyjson_mut_obj_add_arr(doc, view_version_obj, "representations");
	auto repr_obj = yyjson_mut_arr_add_obj(doc, representations_arr);

	// SQL view representation
	yyjson_mut_obj_add_strcpy(doc, repr_obj, "type", "sql");
	yyjson_mut_obj_add_strcpy(doc, repr_obj, "sql", view_sql.c_str());
	yyjson_mut_obj_add_strcpy(doc, repr_obj, "dialect", "duckdb");

	// default-namespace: the namespace where this view is created
	auto default_ns_arr = yyjson_mut_obj_add_arr(doc, view_version_obj, "default-namespace");
	for (const auto &ns : namespace_items) {
		yyjson_mut_arr_add_strcpy(doc, default_ns_arr, ns.c_str());
	}
}

} // namespace duckdb
