#include "catalog/rest/iceberg_view_set.hpp"
#include "catalog/rest/iceberg_catalog.hpp"
#include "catalog/rest/catalog_entry/schema/iceberg_schema_entry.hpp"
#include "catalog/rest/api/catalog_api.hpp"
#include "iceberg_logging.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "catalog/rest/api/iceberg_type.hpp"
#include "core/metadata/schema/iceberg_column_definition.hpp"

namespace duckdb {

// Helper function to compare Iceberg types
static bool IcebergTypesMatch(const LogicalType &new_type, const rest_api_objects::Type *curr_type) {
	bool types_match = false;

	if (curr_type) {
		// Check primitive types
		if (curr_type->has_primitive_type && !new_type.IsNested()) {
			auto new_type_str = IcebergTypeHelper::LogicalTypeToIcebergType(new_type);
			types_match = (new_type_str == curr_type->primitive_type.value);
		}
		// Check complex types
		else if (!curr_type->has_primitive_type && new_type.IsNested()) {
			// List type
			if (curr_type->has_list_type && new_type.id() == LogicalTypeId::LIST) {
				auto &child_type = ListType::GetChildType(new_type);
				types_match = IcebergTypesMatch(child_type, curr_type->list_type.element.get());
			}
			// Map type
			else if (curr_type->has_map_type && new_type.id() == LogicalTypeId::MAP) {
				auto &key_type = MapType::KeyType(new_type);
				auto &value_type = MapType::ValueType(new_type);
				types_match = IcebergTypesMatch(key_type, curr_type->map_type.key.get()) &&
				              IcebergTypesMatch(value_type, curr_type->map_type.value.get());
			}
			// Struct type
			else if (curr_type->has_struct_type && new_type.id() == LogicalTypeId::STRUCT) {
				auto &struct_children = StructType::GetChildTypes(new_type);
				types_match = (struct_children.size() == curr_type->struct_type.fields.size());

				for (size_t i = 0; types_match && i < struct_children.size(); i++) {
					auto &new_field = struct_children[i];
					auto &curr_field = curr_type->struct_type.fields[i];

					types_match = (new_field.first == curr_field->name) &&
					              IcebergTypesMatch(new_field.second, curr_field->type.get());
				}
			}
		}
	}

	return types_match;
}

// Helper function to check if an IcebergTableSchema matches a REST API schema
static bool TableSchemasMatch(const IcebergTableSchema &iceberg_schema, const rest_api_objects::Schema &rest_schema) {
	const auto &fields = rest_schema.struct_type.fields;

	if (iceberg_schema.columns.size() != fields.size()) {
		return false;
	}

	for (size_t i = 0; i < iceberg_schema.columns.size(); i++) {
		const auto &iceberg_col = iceberg_schema.columns[i];
		const auto &rest_field = fields[i];

		// Compare names
		if (iceberg_col->name != rest_field->name || !IcebergTypesMatch(iceberg_col->type, rest_field->type.get())) {
			return false;
		}
	}

	return true;
}

// Helper function to create an IcebergTableSchema from column names and types
static shared_ptr<IcebergTableSchema> CreateIcebergTableSchema(const vector<string> &column_names,
                                                               const vector<LogicalType> &types) {
	D_ASSERT(column_names.size() == types.size());

	auto schema = make_shared_ptr<IcebergTableSchema>();

	idx_t field_id = 1;
	auto next_field_id = [&field_id]() -> idx_t {
		return field_id++;
	};

	for (idx_t i = 0; i < types.size(); i++) {
		const string &column_name = column_names[i];
		const auto &column_type = types[i];

		rest_api_objects::Type type;
		if (column_type.IsNested()) {
			type = IcebergTypeHelper::CreateIcebergRestType(column_type, next_field_id);
		} else {
			type.has_primitive_type = true;
			type.primitive_type = rest_api_objects::PrimitiveType();
			type.primitive_type.value = IcebergTypeHelper::LogicalTypeToIcebergType(column_type);
		}

		auto column_def = IcebergColumnDefinition::ParseType(column_name, field_id++, false, type, "");
		schema->columns.push_back(std::move(column_def));
	}

	return schema;
}

IcebergViewSet::IcebergViewSet(IcebergSchemaEntry &schema) : schema(schema), catalog(schema.ParentCatalog()) {
}

bool IcebergViewSet::CreateNewEntry(ClientContext &context, IcebergCatalog &catalog, IcebergSchemaEntry &schema,
                                    CreateViewInfo &info) {
	lock_guard<mutex> l(entry_lock);

	auto view_name = info.view_name;
	string view_sql = info.query->ToString();

	auto view_entry_it = entries.find(view_name);
	if (view_entry_it == entries.end()) {
		switch (info.on_conflict) {
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			if (IRCAPI::VerifyViewExistence(context, catalog, schema, view_name)) {
				return false;
			}
			break;
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			if (IRCAPI::VerifyViewExistence(context, catalog, schema, view_name)) {
				if (!LoadViewFromServer(context, catalog, view_name)) {
					throw CatalogException("Could not load existing Iceberg view '%s' for replacement", view_name);
				}
				view_entry_it = entries.find(view_name);
			}
			break;
		default:
			break;
		}
	}

	if (view_entry_it != entries.end()) {
		switch (info.on_conflict) {
		case OnCreateConflict::ERROR_ON_CONFLICT:
			throw CatalogException("View with name '%s' already exists", view_name.c_str());
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return false;
		case OnCreateConflict::ALTER_ON_CONFLICT:
			throw NotImplementedException("Alter on conflict");
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			return ReplaceExistingView(context, catalog, schema, info, view_name);
		default:
			throw InternalException("Unknown conflict state when creating a view");
		}
	}

	// Create schema using the helper function
	auto &column_names = info.aliases.empty() ? info.names : info.aliases;
	auto view_schema = CreateIcebergTableSchema(column_names, info.types);
	view_schema->schema_id = 0;

	entries.emplace(view_name, IcebergViewInformation(catalog, schema, view_name));
	auto &view_info = entries.find(view_name)->second;

	rest_api_objects::LoadViewResult load_view_result =
	    IRCAPI::CreateView(context, catalog, schema, view_name, view_sql, view_schema);
	view_info.load_view_result = std::move(load_view_result);

	auto view_entry = make_uniq<IcebergViewEntry>(view_info, catalog, schema, info);
	view_info.view_entry = std::move(view_entry);

	return true;
}

optional_ptr<CatalogEntry> IcebergViewSet::GetEntry(ClientContext &context, const EntryLookupInfo &lookup) {
	lock_guard<mutex> l(entry_lock);

	auto view_name = lookup.GetEntryName();
	auto view_set_entry = entries.find(view_name);
	if (view_set_entry != entries.end()) {
		return view_set_entry->second.view_entry.get();
	}

	// View not found in local entries, check if it exists on server
	auto &ic_catalog = catalog.Cast<IcebergCatalog>();
	if (!IRCAPI::VerifyViewExistence(context, ic_catalog, schema, view_name)) {
		return nullptr;
	}
	return LoadViewFromServer(context, ic_catalog, view_name);
}

void IcebergViewSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	lock_guard<mutex> l(entry_lock);

	// Load views from REST catalog if not already listed
	if (!listed) {
		auto &ic_catalog = catalog.Cast<IcebergCatalog>();
		auto view_identifiers = IRCAPI::GetViews(context, ic_catalog, schema);

		for (auto &view_id : view_identifiers) {
			auto view_name = view_id.name;
			// Only add if not already in local cache
			if (entries.find(view_name) == entries.end()) {
				entries.emplace(view_name, IcebergViewInformation(ic_catalog, schema, view_name));
			}
		}
		listed = true;
	}

	// Iterate over all views and call callback
	for (auto &entry : entries) {
		auto &view_info = entry.second;
		auto view_name = entry.first;

		// If view entry doesn't exist, we need to load it from the server
		if (!view_info.view_entry) {
			auto &ic_catalog = catalog.Cast<IcebergCatalog>();
			LoadViewFromServer(context, ic_catalog, view_name);
			// After loading, the view_entry should be populated in view_info
		}

		// Call callback if view entry exists
		if (view_info.view_entry) {
			callback(*view_info.view_entry);
		}
	}
}

void IcebergViewSet::DropEntry(ClientContext &context, IcebergCatalog &catalog, IcebergSchemaEntry &schema,
                               DropInfo &info) {
	lock_guard<mutex> l(entry_lock);

	auto view_name = info.name;

	auto view_info_it = entries.find(view_name);
	if (view_info_it == entries.end()) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("View '%s' does not exist", view_name);
	}

	IRCAPI::DropView(context, catalog, schema.namespace_items, view_name);
	entries.erase(view_info_it);
}

optional_ptr<CatalogEntry> IcebergViewSet::LoadViewFromServer(ClientContext &context, IcebergCatalog &catalog,
                                                              const string &view_name) {
	auto load_view_result = IRCAPI::GetView(context, catalog, schema, view_name);

	auto current_version_id = load_view_result.metadata.current_version_id;
	auto version_it = std::find_if(
	    load_view_result.metadata.versions.begin(), load_view_result.metadata.versions.end(),
	    [current_version_id](const rest_api_objects::ViewVersion &v) { return v.version_id == current_version_id; });
	if (version_it == load_view_result.metadata.versions.end()) {
		return nullptr;
	}
	auto &current_version = *version_it;

	auto duckdb_repr = std::find_if(current_version.representations.begin(), current_version.representations.end(),
	                                [](const rest_api_objects::ViewRepresentation &repr) {
		                                return repr.has_sqlview_representation &&
		                                       StringUtil::Lower(repr.sqlview_representation.dialect) == "duckdb";
	                                });
	if (duckdb_repr == current_version.representations.end()) {
		return nullptr;
	}
	auto view_sql = duckdb_repr->sqlview_representation.sql;

	CreateViewInfo create_info(schema, view_name);
	create_info.sql = view_sql;

	Parser parser;
	try {
		parser.ParseQuery(view_sql);
	} catch (std::exception &e) {
		DUCKDB_LOG(context, IcebergLogType, "Failed to parse view SQL for view '%s': %s", view_name.c_str(), e.what());
		return nullptr;
	}
	if (parser.statements.size() != 1) {
		DUCKDB_LOG(context, IcebergLogType, "Unexpected number of statements (%zu) in view SQL for view '%s'",
		           parser.statements.size(), view_name.c_str());
		return nullptr;
	}
	auto &statement = parser.statements[0];
	if (statement->type != StatementType::SELECT_STATEMENT) {
		DUCKDB_LOG(context, IcebergLogType, "View SQL for view '%s' is not a SELECT statement", view_name.c_str());
		return nullptr;
	}
	create_info.query = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(statement));

	auto schema_id = current_version.schema_id;
	auto view_schema_it =
	    std::find_if(load_view_result.metadata.schemas.begin(), load_view_result.metadata.schemas.end(),
	                 [schema_id](const rest_api_objects::Schema &s) { return s.object_1.schema_id == schema_id; });
	if (view_schema_it == load_view_result.metadata.schemas.end()) {
		return nullptr;
	}
	auto &view_schema_obj = *view_schema_it;
	for (auto &field : view_schema_obj.struct_type.fields) {
		create_info.names.push_back(field->name);
		auto column_def =
		    IcebergColumnDefinition::ParseType(field->name, field->id, field->required, *field->type, field->doc);
		create_info.types.push_back(column_def->type);
	}

	// Find or create the entry
	auto entry_it = entries.find(view_name);
	if (entry_it == entries.end()) {
		entries.emplace(view_name, IcebergViewInformation(catalog, schema, view_name));
		entry_it = entries.find(view_name);
	}
	auto &view_info = entry_it->second;
	view_info.load_view_result = std::move(load_view_result);
	auto view_entry = make_uniq<IcebergViewEntry>(view_info, this->catalog, schema, create_info);
	auto view_entry_ptr = view_entry.get();
	view_info.view_entry = std::move(view_entry);

	return view_entry_ptr;
}

bool IcebergViewSet::ReplaceExistingView(ClientContext &context, IcebergCatalog &catalog, IcebergSchemaEntry &schema,
                                         CreateViewInfo &info, const string &view_name) {
	auto &view_info = entries.find(view_name)->second;
	string new_sql = info.query->ToString();

	auto &column_names = info.aliases.empty() ? info.names : info.aliases;
	auto table_schema = CreateIcebergTableSchema(column_names, info.types);

	// First, check if any existing version has the exact same SQL
	int32_t matching_version_id = -1;
	for (const auto &version : view_info.load_view_result.metadata.versions) {
		for (const auto &repr : version.representations) {
			if (repr.has_sqlview_representation && repr.sqlview_representation.dialect == "duckdb" &&
			    repr.sqlview_representation.type == "sql" && repr.sqlview_representation.sql == new_sql) {
				auto version_schema_id = version.schema_id;
				auto schema_it = std::find_if(view_info.load_view_result.metadata.schemas.begin(),
				                              view_info.load_view_result.metadata.schemas.end(),
				                              [version_schema_id](const rest_api_objects::Schema &s) {
					                              return s.object_1.schema_id == version_schema_id;
				                              });
				if (schema_it != view_info.load_view_result.metadata.schemas.end()) {
					if (TableSchemasMatch(*table_schema, *schema_it)) {
						matching_version_id = version.version_id;
						break;
					}
				}
			}
		}

		if (matching_version_id != -1) {
			break;
		}
	}

	rest_api_objects::LoadViewResult new_view_result;
	if (matching_version_id != -1) {
		new_view_result = IRCAPI::SetCurrentViewVersion(context, catalog, schema, view_name, matching_version_id,
		                                                view_info.load_view_result);
	} else {
		// Check if the schema matches an existing one, reuse it if so;
		// -1 signals "new schema" — IRCAPI::ReplaceView assigns the actual next ID.
		table_schema->schema_id = -1;
		for (auto &existing_schema : view_info.load_view_result.metadata.schemas) {
			if (TableSchemasMatch(*table_schema, existing_schema)) {
				table_schema->schema_id = existing_schema.object_1.schema_id;
				break;
			}
		}

		// Create a new version with the new SQL and schema
		new_view_result =
		    IRCAPI::ReplaceView(context, catalog, schema, view_name, new_sql, table_schema, view_info.load_view_result);
	}

	// Recreate view entry with updated metadata
	view_info.load_view_result = std::move(new_view_result);
	auto view_entry = make_uniq<IcebergViewEntry>(view_info, catalog, schema, info);
	view_info.view_entry = std::move(view_entry);

	return true;
}

} // namespace duckdb
