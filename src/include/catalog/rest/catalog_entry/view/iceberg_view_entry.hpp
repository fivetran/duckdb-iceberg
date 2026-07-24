#pragma once

#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "rest_catalog/objects/load_view_result.hpp"

namespace duckdb {

class IcebergCatalog;
class IcebergSchemaEntry;
class IcebergViewEntry;

struct IcebergViewInformation {
	IcebergViewInformation(IcebergCatalog &catalog, IcebergSchemaEntry &schema, const string &view_name)
	    : catalog(catalog), schema(schema), name(view_name) {
	}

	IcebergCatalog &catalog;
	IcebergSchemaEntry &schema;
	string name;
	rest_api_objects::LoadViewResult load_view_result;
	unique_ptr<IcebergViewEntry> view_entry;
};

class IcebergViewEntry : public ViewCatalogEntry {
public:
	IcebergViewEntry(IcebergViewInformation &view_info, Catalog &catalog, SchemaCatalogEntry &schema,
	                 CreateViewInfo &info);

public:
	IcebergViewInformation &view_info;
};

} // namespace duckdb
