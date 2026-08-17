#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! Versioned, product-agnostic metadata exposed by bound Iceberg table scans.
constexpr const char *ICEBERG_BOUND_SCAN_OPTION = "duckdb.iceberg.bound_scan.v1";
constexpr uint32_t ICEBERG_BOUND_SCAN_VERSION = 1;

struct IcebergBoundField {
	int32_t field_id;
	string name;
};

struct IcebergBoundBlob {
	string type;
	int64_t snapshot_id;
	int64_t sequence_number;
	vector<int32_t> fields;
	case_insensitive_map_t<string> properties;
};

struct IcebergBoundStatisticsFile {
	int64_t snapshot_id;
	string path;
	int64_t file_size;
	int64_t footer_size;
	vector<IcebergBoundBlob> blobs;
};

struct IcebergBoundScanMetadata {
	bool has_snapshot = false;
	int64_t snapshot_id = 0;
	int64_t sequence_number = 0;
	int32_t schema_id = 0;
	string table_location;
	vector<IcebergBoundField> fields;
	vector<IcebergBoundStatisticsFile> statistics;
	case_insensitive_map_t<string> properties;
};

} // namespace duckdb
