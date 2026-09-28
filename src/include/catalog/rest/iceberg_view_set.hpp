#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "catalog/rest/catalog_entry/view/iceberg_view_entry.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {

class IcebergSchemaEntry;
class IcebergCatalog;
struct CreateViewInfo;
struct DropInfo;

class IcebergViewSet {
public:
	explicit IcebergViewSet(IcebergSchemaEntry &schema);

public:
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const EntryLookupInfo &lookup);
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);
	bool CreateNewEntry(ClientContext &context, IcebergCatalog &catalog, IcebergSchemaEntry &schema,
	                    CreateViewInfo &info);
	void DropEntry(ClientContext &context, IcebergCatalog &catalog, IcebergSchemaEntry &schema, DropInfo &info);

public:
	IcebergSchemaEntry &schema;
	Catalog &catalog;
	case_insensitive_map_t<IcebergViewInformation> entries;
	bool listed = false;

private:
	mutex entry_lock;

	// Helper function to load view from REST catalog and create a view entry
	// Returns nullptr if the view cannot be loaded or parsed
	optional_ptr<CatalogEntry> LoadViewFromServer(ClientContext &context, IcebergCatalog &catalog,
	                                              const string &view_name);

	// Helper function to replace an existing view
	// Handles metadata loading, schema building, and API call
	bool ReplaceExistingView(ClientContext &context, IcebergCatalog &catalog, IcebergSchemaEntry &schema,
	                         CreateViewInfo &info, const string &view_name);
};

} // namespace duckdb
