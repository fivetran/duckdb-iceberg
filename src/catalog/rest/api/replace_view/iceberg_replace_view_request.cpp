#include "catalog/rest/api/replace_view/iceberg_replace_view_request.hpp"
#include "catalog/rest/api/iceberg_json_utils.hpp"
#include "catalog/rest/api/iceberg_create_table_request.hpp"

namespace duckdb {

IcebergReplaceViewRequest::IcebergReplaceViewRequest(const string &view_name, const string &view_uuid,
                                                     const string &view_sql, const uint32_t version_id,
                                                     shared_ptr<IcebergTableSchema> schema, int32_t new_schema_id,
                                                     const vector<string> &namespace_items)
    : view_name(view_name), view_uuid(view_uuid), view_sql(view_sql), version_id(version_id), schema(schema),
      new_schema_id(new_schema_id), namespace_items(namespace_items) {
}

string IcebergReplaceViewRequest::ReplaceViewToJSON(std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p) {
	auto doc = doc_p.get();
	auto root_object = yyjson_mut_doc_get_root(doc);

	// Add requirements array
	auto requirements_arr = yyjson_mut_obj_add_arr(doc, root_object, "requirements");
	auto req_obj = yyjson_mut_arr_add_obj(doc, requirements_arr);
	yyjson_mut_obj_add_strcpy(doc, req_obj, "type", "assert-view-uuid");
	yyjson_mut_obj_add_strcpy(doc, req_obj, "uuid", view_uuid.c_str());

	// Add updates array
	auto updates_arr = yyjson_mut_obj_add_arr(doc, root_object, "updates");

	// new_schema_id >= 0 means we need to add a new schema; set it on the schema before serializing.
	// Otherwise the schema already exists and schema->schema_id is the existing schema's ID.
	if (new_schema_id >= 0) {
		schema->schema_id = new_schema_id;
	}
	int32_t view_version_schema_id = schema->schema_id;

	// 1. Add schema update only when adding a new schema
	if (new_schema_id >= 0) {
		auto add_schema_update = yyjson_mut_arr_add_obj(doc, updates_arr);
		yyjson_mut_obj_add_strcpy(doc, add_schema_update, "action", "add-schema");
		auto schema_obj = yyjson_mut_obj_add_obj(doc, add_schema_update, "schema");
		IcebergCreateTableRequest::PopulateSchema(doc, schema_obj, *schema.get());
	}

	// 2. Add view version update
	auto add_version_update = yyjson_mut_arr_add_obj(doc, updates_arr);
	yyjson_mut_obj_add_strcpy(doc, add_version_update, "action", "add-view-version");
	auto view_version_obj = yyjson_mut_obj_add_obj(doc, add_version_update, "view-version");
	IcebergJSONUtils::PopulateViewVersion(doc, view_version_obj, version_id, view_version_schema_id, view_sql,
	                                      namespace_items);

	// 3. Set current view version update
	auto set_current_update = yyjson_mut_arr_add_obj(doc, updates_arr);
	yyjson_mut_obj_add_strcpy(doc, set_current_update, "action", "set-current-view-version");
	yyjson_mut_obj_add_int(doc, set_current_update, "view-version-id", version_id);

	return ICUtils::JsonToString(std::move(doc_p));
}

} // namespace duckdb
