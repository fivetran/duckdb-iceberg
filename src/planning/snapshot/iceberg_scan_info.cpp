#include "planning/snapshot/iceberg_scan_info.hpp"

namespace duckdb {

IcebergBoundScanMetadata GetBoundScanMetadata(const IcebergTableMetadata &metadata,
                                              const IcebergSnapshotScanInfo &snapshot_info,
                                              const IcebergTableSchema &schema) {
	IcebergBoundScanMetadata result;
	result.schema_id = snapshot_info.schema_id;
	result.table_location = metadata.location;
	result.properties = metadata.table_properties;
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
		IcebergBoundStatisticsFile bound_statistics;
		bound_statistics.snapshot_id = statistics.snapshot_id;
		bound_statistics.path = statistics.path;
		bound_statistics.file_size = statistics.file_size;
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

} // namespace duckdb
