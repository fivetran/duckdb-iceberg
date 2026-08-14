#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/common/enums/joinref_type.hpp"
#include "duckdb/common/enums/tableref_type.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "common/iceberg_utils.hpp"
#include "planning/iceberg_multi_file_reader.hpp"
#include "function/iceberg_functions.hpp"
#include "catalog/rest/catalog_entry/table/iceberg_table_entry.hpp"

#include <string>
#include <numeric>

namespace duckdb {

static void AddNamedParameters(TableFunction &fun) {
	fun.named_parameters["allow_moved_paths"] = LogicalType::BOOLEAN;
	fun.named_parameters["mode"] = LogicalType::VARCHAR;
	fun.named_parameters["metadata_compression_codec"] = LogicalType::VARCHAR;
	fun.named_parameters["version"] = LogicalType::VARCHAR;
	fun.named_parameters["version_name_format"] = LogicalType::VARCHAR;
	fun.named_parameters["snapshot_from_timestamp"] = LogicalType::TIMESTAMP;
	fun.named_parameters["snapshot_from_id"] = LogicalType::UBIGINT;
}

virtual_column_map_t IcebergVirtualColumns(ClientContext &context, optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<MultiFileBindData>();
	auto result = IcebergTableEntry::VirtualColumns();
	bind_data.virtual_columns = result;
	return result;
}

static void IcebergScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                 const TableFunction &function) {
	throw NotImplementedException("IcebergScan serialization not implemented");
}
static unique_ptr<FunctionData> IcebergScanDeserialize(Deserializer &deserializer, TableFunction &function) {
	throw NotImplementedException("IcebergScan deserialization not implemented");
}

static Value BoundScanInfoValue(const IcebergBoundScanMetadataV1 &metadata) {
	child_list_t<LogicalType> property_types {{"key", LogicalType::VARCHAR}, {"value", LogicalType::VARCHAR}};
	child_list_t<LogicalType> blob_types {
	    {"statistics_path", LogicalType::VARCHAR},
	    {"file_size", LogicalType::BIGINT},
	    {"blob_type", LogicalType::VARCHAR},
	    {"blob_snapshot_id", LogicalType::BIGINT},
	    {"blob_sequence_number", LogicalType::BIGINT},
	    {"field_ids", LogicalType::LIST(LogicalType::INTEGER)},
	    {"field_names", LogicalType::LIST(LogicalType::VARCHAR)},
	    {"properties", LogicalType::LIST(LogicalType::STRUCT(property_types))},
	};
	vector<Value> blobs;
	for (auto &statistics : metadata.statistics) {
		for (auto &blob : statistics.blobs) {
			vector<Value> field_ids;
			vector<Value> field_names;
			for (auto field_id : blob.fields) {
				field_ids.push_back(Value::INTEGER(field_id));
				for (auto &field : metadata.fields) {
					if (field.field_id == field_id) {
						field_names.push_back(field.name);
						break;
					}
				}
			}
			vector<Value> properties;
			for (auto &property : blob.properties) {
				properties.push_back(Value::STRUCT({{"key", property.first}, {"value", property.second}}));
			}
			blobs.push_back(Value::STRUCT({
			    {"statistics_path", statistics.path},
			    {"file_size", Value::BIGINT(statistics.file_size)},
			    {"blob_type", blob.type},
			    {"blob_snapshot_id", Value::BIGINT(blob.snapshot_id)},
			    {"blob_sequence_number", Value::BIGINT(blob.sequence_number)},
			    {"field_ids", Value::LIST(LogicalType::INTEGER, std::move(field_ids))},
			    {"field_names", Value::LIST(LogicalType::VARCHAR, std::move(field_names))},
			    {"properties", Value::LIST(LogicalType::STRUCT(property_types), std::move(properties))},
			}));
		}
	}
	return Value::STRUCT({
	    {"contract_version", Value::UINTEGER(FIVETRAN_BOUND_SCAN_VERSION)},
	    {"has_snapshot", Value::BOOLEAN(metadata.has_snapshot)},
	    {"snapshot_id", Value::BIGINT(metadata.snapshot_id)},
	    {"sequence_number", Value::BIGINT(metadata.sequence_number)},
	    {"schema_id", Value::INTEGER(metadata.schema_id)},
	    {"blobs", Value::LIST(LogicalType::STRUCT(blob_types), std::move(blobs))},
	});
}

static Value BoundScanInfoValueV2(const IcebergBoundScanMetadataV2 &metadata) {
	child_list_t<LogicalType> definition_types {
	    {"name", LogicalType::VARCHAR},
	    {"stable_id_field_id", LogicalType::INTEGER},
	    {"content_field_id", LogicalType::INTEGER},
	};
	vector<Value> definitions;
	for (auto &definition : metadata.definitions) {
		definitions.push_back(Value::STRUCT({
		    {"name", definition.name},
		    {"stable_id_field_id", Value::INTEGER(definition.stable_id_field_id)},
		    {"content_field_id", Value::INTEGER(definition.content_field_id)},
		}));
	}
	auto v1 = BoundScanInfoValue(metadata);
	auto &fields = StructValue::GetChildren(v1);
	return Value::STRUCT({
	    {"contract_version", Value::UINTEGER(FIVETRAN_BOUND_SCAN_VERSION_V2)},
	    {"has_snapshot", fields[1]},
	    {"snapshot_id", fields[2]},
	    {"sequence_number", fields[3]},
	    {"schema_id", fields[4]},
	    {"blobs", fields[5]},
	    {"definitions", Value::LIST(LogicalType::STRUCT(definition_types), std::move(definitions))},
	});
}

BindInfo IcebergBindInfo(const optional_ptr<FunctionData> bind_data) {
	auto &multi_file_data = bind_data->Cast<MultiFileBindData>();
	auto &file_list = multi_file_data.file_list->Cast<IcebergMultiFileList>();
	auto table = file_list.GetTable();
	if (!table) {
		return BindInfo(ScanType::EXTERNAL);
	}
	BindInfo result(*table);
	result.InsertOption(FIVETRAN_BOUND_SCAN_OPTION_V1, BoundScanInfoValue(file_list.GetBoundScanMetadata()));
	result.InsertOption(FIVETRAN_BOUND_SCAN_OPTION_V2, BoundScanInfoValueV2(file_list.GetBoundScanMetadataV2()));
	return result;
}

//! FIXME: needs v1.5.1, causes a crash on v1.5.0
// static bool IcebergScanSupportsPushdownType(const FunctionData &bind_data_p, idx_t column_id) {
//	// Don't push down filters on the _row_id virtual column
//	if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
//		return false;
//	}

//	// Default behavior for other columns
//	return true;
//}

TableFunctionSet IcebergFunctions::GetIcebergScanFunction(ExtensionLoader &loader) {
	// The iceberg_scan function is constructed by grabbing the parquet scan from the Catalog, then injecting the
	// IcebergMultiFileReader into it to create a Iceberg-based multi file read

	auto &parquet_scan = loader.GetTableFunction("parquet_scan");
	auto parquet_scan_copy = parquet_scan.functions;

	for (auto &function : parquet_scan_copy.functions) {
		// Register the MultiFileReader as the driver for reads
		function.get_multi_file_reader = IcebergMultiFileReader::CreateInstance;
		function.late_materialization = false;

		// Unset all of these: they are either broken, very inefficient.
		// TODO: implement/fix these
		function.serialize = IcebergScanSerialize;
		function.deserialize = IcebergScanDeserialize;

		function.statistics = nullptr;
		function.table_scan_progress = nullptr;
		function.get_bind_info = IcebergBindInfo;
		function.get_virtual_columns = IcebergVirtualColumns;
		function.get_partition_stats = IcebergMultiFileReader::IcebergGetPartitionStats;
		// function.supports_pushdown_type = IcebergScanSupportsPushdownType;

		// Schema param is just confusing here
		function.named_parameters.erase("schema");
		AddNamedParameters(function);

		function.name = "iceberg_scan";
	}

	parquet_scan_copy.name = "iceberg_scan";
	return parquet_scan_copy;
}

} // namespace duckdb
