#define DUCKDB_EXTENSION_MAIN

#include "iceberg_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "iceberg_functions.hpp"
#include "yyjson.hpp"
#include "duckdb/main/extension_util.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

static void LoadInternal(DatabaseInstance &instance) {
	auto &config = DBConfig::GetConfig(instance);

	config.AddExtensionOption(
		"unsafe_enable_version_guessing",
		"Enable globbing the filesystem (if possible) to find the latest version metadata. This could result in reading an uncommitted version.",
		LogicalType::BOOLEAN,
		Value::BOOLEAN(false)
	);

	// Iceberg Table Functions
	for (auto &fun : IcebergFunctions::GetTableFunctions()) {
		ExtensionUtil::RegisterFunction(instance, fun);
	}

	// Iceberg Scalar Functions
	for (auto &fun : IcebergFunctions::GetScalarFunctions()) {
		ExtensionUtil::RegisterFunction(instance, fun);
	}
}

void IcebergExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string IcebergExtension::Name() {
	return "iceberg";
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void iceberg_init(duckdb::DatabaseInstance &db) {
	LoadInternal(db);
}

DUCKDB_EXTENSION_API const char *iceberg_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
