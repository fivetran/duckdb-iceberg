#include "function/iceberg_functions.hpp"

#include "catalog/rest/catalog_entry/table/iceberg_table_entry.hpp"
#include "catalog/rest/iceberg_catalog.hpp"
#include "catalog/rest/transaction/iceberg_transaction.hpp"
#include "common/iceberg_utils.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {
namespace {

vector<string> StringList(const Value &value, const string &name) {
	if (value.type().id() != LogicalTypeId::LIST) {
		throw InvalidInputException("iceberg_prepare_puffin_statistics requires %s to be VARCHAR[]", name);
	}
	vector<string> result;
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull() || child.type().id() != LogicalTypeId::VARCHAR) {
			throw InvalidInputException("iceberg_prepare_puffin_statistics requires %s to contain VARCHAR values",
			                            name);
		}
		result.push_back(child.GetValue<string>());
	}
	return result;
}

unique_ptr<FunctionData> BindPreparePuffinStatistics(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	if (!input.binder) {
		throw InternalException("iceberg_prepare_puffin_statistics requires a binder");
	}
	auto table_name = input.inputs[0].GetValue<string>();
	auto entry = IcebergUtils::GetTableEntry(context, table_name);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY || entry->ParentCatalog().GetCatalogType() != "iceberg") {
		throw InvalidInputException("iceberg_prepare_puffin_statistics requires an Iceberg table");
	}
	auto &table = entry->Cast<IcebergTableEntry>();
	auto snapshot_id = input.inputs[2].GetValue<int64_t>();
	auto sequence_number = input.inputs[3].GetValue<int64_t>();
	auto snapshot = table.table_info.table_metadata.GetLatestSnapshot();
	if (!snapshot || snapshot->snapshot_id != snapshot_id || snapshot->sequence_number != sequence_number) {
		throw InvalidInputException("Iceberg table %s changed while preparing Puffin statistics", table_name);
	}

	auto property_keys = StringList(input.inputs[4], "property_keys");
	auto property_values = StringList(input.inputs[5], "property_values");
	auto blob_types = StringList(input.inputs[6], "blob_types");
	auto blob_property_keys = StringList(input.inputs[7], "blob_property_keys");
	auto blob_property_values = StringList(input.inputs[8], "blob_property_values");
	if (property_keys.size() != property_values.size()) {
		throw InvalidInputException("Puffin publication property keys and values must have the same length");
	}
	if (blob_types.empty() || blob_types.size() != blob_property_keys.size() ||
	    blob_types.size() != blob_property_values.size()) {
		throw InvalidInputException("Puffin publication blob descriptors must be non-empty and have the same length");
	}

	case_insensitive_map_t<string> properties;
	for (idx_t index = 0; index < property_keys.size(); index++) {
		properties[property_keys[index]] = property_values[index];
	}
	vector<ExpectedPuffinBlob> expected_blobs;
	for (idx_t index = 0; index < blob_types.size(); index++) {
		expected_blobs.push_back({blob_types[index], blob_property_keys[index], blob_property_values[index]});
	}

	DatabaseModificationType modification;
	modification |= DatabaseModificationType::ALTER_TABLE;
	input.binder->GetStatementProperties().RegisterDBModify(entry->ParentCatalog(), context, modification);
	auto &catalog = table.catalog.Cast<IcebergCatalog>();
	auto &transaction = IcebergTransaction::Get(context, catalog);
	ApplyTableUpdate(table.table_info, transaction, [&](IcebergTableInformation &updated_table) {
		auto &transaction_data = updated_table.GetOrCreateTransactionData(transaction);
		if (!properties.empty()) {
			transaction_data.TableSetProperties(properties);
			for (auto &property : properties) {
				updated_table.table_metadata.table_properties[property.first] = property.second;
			}
		}
		transaction_data.TableSetPuffinStatistics(input.inputs[1].GetValue<string>(), std::move(expected_blobs),
		                                          snapshot_id, sequence_number);
	});

	return_types.push_back(LogicalType::BIGINT);
	names.push_back("Count");
	return make_uniq<TableFunctionData>();
}

void PreparePuffinStatistics(ClientContext &, TableFunctionInput &, DataChunk &output) {
	output.SetCardinality(0);
}

} // namespace

TableFunctionSet IcebergFunctions::GetPreparePuffinStatisticsFunction() {
	TableFunctionSet result("iceberg_prepare_puffin_statistics");
	TableFunction function({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT,
	                        LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::VARCHAR)},
	                       PreparePuffinStatistics, BindPreparePuffinStatistics);
	result.AddFunction(std::move(function));
	return result;
}

} // namespace duckdb
