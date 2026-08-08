#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! Versioned payload returned in BindInfo::options by bound Iceberg table scans.
//! The Value is a struct containing snapshot identity and a list of matching index descriptors.
constexpr const char *FIVETRAN_BOUND_SCAN_OPTION_V1 = "fivetran.iceberg.bound_scan.v1";
constexpr uint32_t FIVETRAN_BOUND_SCAN_VERSION = 1;

struct IcebergBoundFieldV1 {
	int32_t field_id;
	string name;
};

struct IcebergBoundBlobV1 {
	string type;
	int64_t snapshot_id;
	int64_t sequence_number;
	vector<int32_t> fields;
	case_insensitive_map_t<string> properties;
};

struct IcebergBoundStatisticsFileV1 {
	int64_t snapshot_id;
	string path;
	int64_t file_size;
	int64_t footer_size;
	vector<IcebergBoundBlobV1> blobs;
};

struct IcebergBoundScanMetadataV1 {
	bool has_snapshot = false;
	int64_t snapshot_id = 0;
	int64_t sequence_number = 0;
	int32_t schema_id = 0;
	vector<IcebergBoundFieldV1> fields;
	vector<IcebergBoundStatisticsFileV1> statistics;
};

} // namespace duckdb
