#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/function/table_function.hpp"

#include "core/metadata/iceberg_table_metadata.hpp"
#include "core/metadata/schema/iceberg_table_schema.hpp"
#include "core/metadata/snapshot/iceberg_snapshot.hpp"
#include "catalog/rest/transaction/iceberg_transaction_data.hpp"
#include "planning/snapshot/iceberg_snapshot_scan_info.hpp"
#include "fivetran/iceberg_bound_scan_metadata.hpp"
#include "fivetran/fivetran_ai_index_definition.hpp"

namespace duckdb {

struct IcebergTransactionData;

//! Used when we are not scanning from a REST Catalog
struct IcebergScanTemporaryData {
	IcebergTableMetadata metadata;
};

inline IcebergBoundScanMetadataV1 GetBoundScanMetadata(const IcebergTableMetadata &metadata,
                                                       const IcebergSnapshotScanInfo &snapshot_info,
                                                       const IcebergTableSchema &schema) {
	IcebergBoundScanMetadataV1 result;
	result.schema_id = snapshot_info.schema_id;
	for (auto &column : schema.columns) {
		result.fields.push_back({column->id, column->name});
	}
	if (!snapshot_info.snapshot) {
		return result;
	}
	result.has_snapshot = true;
	result.snapshot_id = snapshot_info.snapshot->snapshot_id;
	result.sequence_number = snapshot_info.snapshot->sequence_number;
	for (auto &statistics : metadata.statistics) {
		if (statistics.snapshot_id != result.snapshot_id) {
			continue;
		}
		IcebergBoundStatisticsFileV1 bound_statistics;
		bound_statistics.snapshot_id = statistics.snapshot_id;
		bound_statistics.path = statistics.path;
		bound_statistics.file_size = statistics.file_size;
		bound_statistics.footer_size = statistics.footer_size;
		for (auto &blob : statistics.blobs) {
			if (blob.snapshot_id == result.snapshot_id && blob.sequence_number == result.sequence_number) {
				bound_statistics.blobs.push_back(blob);
			}
		}
		if (!bound_statistics.blobs.empty()) {
			result.statistics.push_back(std::move(bound_statistics));
		}
	}
	return result;
}

inline IcebergBoundScanMetadataV2 GetBoundScanMetadataV2(const IcebergTableMetadata &metadata,
                                                         const IcebergSnapshotScanInfo &snapshot_info,
                                                         const IcebergTableSchema &schema) {
	IcebergBoundScanMetadataV2 result;
	static_cast<IcebergBoundScanMetadataV1 &>(result) = GetBoundScanMetadata(metadata, snapshot_info, schema);
	for (auto &definition : GetFivetranAIBM25IndexDefinitions(metadata.table_properties, schema)) {
		result.definitions.push_back({definition.name, definition.stable_id_field_id, definition.content_field_id});
	}
	return result;
}

struct IcebergScanInfo : public TableFunctionInfo {
public:
	IcebergScanInfo(const string &metadata_path, const IcebergTableMetadata &metadata,
	                IcebergSnapshotScanInfo snapshot_info, const IcebergTableSchema &schema)
	    : metadata_path(metadata_path), metadata(metadata), snapshot_info(snapshot_info), schema(schema) {
		bound_scan = GetBoundScanMetadata(metadata, this->snapshot_info, schema);
		bound_scan_v2 = GetBoundScanMetadataV2(metadata, this->snapshot_info, schema);
	}
	IcebergScanInfo(const string &metadata_path, unique_ptr<IcebergScanTemporaryData> owned_temp_data_p,
	                IcebergSnapshotScanInfo snapshot_info, const IcebergTableSchema &schema)
	    : metadata_path(metadata_path), owned_temp_data(std::move(owned_temp_data_p)),
	      metadata(owned_temp_data->metadata), snapshot_info(snapshot_info), schema(schema) {
		bound_scan = GetBoundScanMetadata(metadata, this->snapshot_info, schema);
		bound_scan_v2 = GetBoundScanMetadataV2(metadata, this->snapshot_info, schema);
	}

public:
	string metadata_path;
	unique_ptr<IcebergScanTemporaryData> owned_temp_data;
	const IcebergTableMetadata &metadata;
	optional_ptr<IcebergTransactionData> transaction_data;

	IcebergSnapshotScanInfo snapshot_info;
	const IcebergTableSchema &schema;
	IcebergBoundScanMetadataV1 bound_scan;
	IcebergBoundScanMetadataV2 bound_scan_v2;
};

} // namespace duckdb
