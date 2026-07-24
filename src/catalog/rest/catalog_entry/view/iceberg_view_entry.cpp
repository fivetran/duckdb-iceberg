#include "catalog/rest/catalog_entry/view/iceberg_view_entry.hpp"
#include "catalog/rest/iceberg_catalog.hpp"
#include "catalog/rest/catalog_entry/schema/iceberg_schema_entry.hpp"

namespace duckdb {

IcebergViewEntry::IcebergViewEntry(IcebergViewInformation &view_info, Catalog &catalog, SchemaCatalogEntry &schema,
                                   CreateViewInfo &info)
    : ViewCatalogEntry(catalog, schema, info), view_info(view_info) {
}

} // namespace duckdb
