#include "catalog/rest/api/create_view/iceberg_create_view_request.hpp"
#include "catalog/rest/api/iceberg_json_utils.hpp"
#include "catalog/rest/api/iceberg_create_table_request.hpp"
#include "core/metadata/schema/iceberg_table_schema.hpp"

using namespace duckdb_yyjson;
namespace duckdb {

IcebergCreateViewRequest::IcebergCreateViewRequest(const string &view_name, const string &view_sql,
                                                   shared_ptr<IcebergTableSchema> schema,
                                                   const vector<string> &namespace_items)
    : view_name(view_name), view_sql(view_sql), schema(schema), namespace_items(namespace_items) {
}

string IcebergCreateViewRequest::CreateViewToJSON(std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p) {
	auto doc = doc_p.get();
	auto root_object = yyjson_mut_doc_get_root(doc);

	// Add view name
	yyjson_mut_obj_add_strcpy(doc, root_object, "name", view_name.c_str());

	// Add schema - use PopulateSchema like CreateTable does
	auto schema_obj = yyjson_mut_obj_add_obj(doc, root_object, "schema");
	IcebergCreateTableRequest::PopulateSchema(doc, schema_obj, *schema.get());

	// Add view-version object (version 1 for new views)
	auto view_version_obj = yyjson_mut_obj_add_obj(doc, root_object, "view-version");
	IcebergJSONUtils::PopulateViewVersion(doc, view_version_obj, 1, schema->schema_id, view_sql, namespace_items);

	// properties: empty object for now (can be extended with custom properties)
	yyjson_mut_obj_add_obj(doc, root_object, "properties");

	// Convert JSON to string
	return ICUtils::JsonToString(std::move(doc_p));
}

} // namespace duckdb
