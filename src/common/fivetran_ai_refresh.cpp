#include "fivetran/fivetran_ai_refresh.hpp"

#include "catalog/rest/catalog_entry/table/iceberg_table_entry.hpp"
#include "catalog/rest/iceberg_catalog.hpp"
#include "catalog/rest/transaction/iceberg_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "fivetran/fivetran_ai_index_definition.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> BindFivetranAIBM25Refresh(Binder &binder, IcebergTableEntry &table,
                                                      vector<FivetranAIBM25IndexDefinition> definitions,
                                                      bool persist_definitions) {
	if (definitions.empty()) {
		throw InvalidInputException("Iceberg table %s has no FIVETRAN_AI BM25 indexes to refresh", table.name);
	}
	auto &table_info = table.table_info;
	auto snapshot = table_info.table_metadata.GetLatestSnapshot();
	if (!snapshot) {
		throw BinderException("cannot refresh BM25 indexes on an Iceberg table with no snapshot");
	}

	auto &file_system = FileSystem::GetFileSystem(binder.context);
	auto file_name =
	    std::to_string(snapshot->snapshot_id) + "-" + UUID::ToString(UUID::GenerateRandomUUID()) + ".bm25.puffin";
	auto statistics_path = file_system.JoinPath(file_system.JoinPath(table_info.BaseFilePath(), "metadata"), file_name);
	vector<string> index_names;
	case_insensitive_map_t<string> properties;
	for (auto &definition : definitions) {
		index_names.push_back(definition.name);
		if (persist_definitions) {
			properties[FivetranAIBM25PropertyName(definition.name)] =
			    FivetranAIBM25PropertyValue(definition.stable_id_field_id, definition.content_field_id);
		}
	}
	auto &catalog = table.catalog.Cast<IcebergCatalog>();
	auto &transaction = IcebergTransaction::Get(binder.context, catalog);
	ApplyTableUpdate(table_info, transaction, [&](IcebergTableInformation &updated_table) {
		auto &transaction_data = updated_table.GetOrCreateTransactionData(transaction);
		if (!properties.empty()) {
			transaction_data.TableSetProperties(properties);
			for (auto &property : properties) {
				updated_table.table_metadata.table_properties[property.first] = property.second;
			}
		}
		transaction_data.TableSetFivetranAIStatistics(statistics_path, index_names, snapshot->snapshot_id,
		                                              snapshot->sequence_number);
	});

	auto copy_statement = make_uniq<CopyStatement>();
	copy_statement->info = make_uniq<CopyInfo>();
	auto &copy = *copy_statement->info;
	copy.is_from = false;
	copy.is_format_auto_detected = false;
	copy.format = "fivetran_ai_puffin";
	copy.file_path = statistics_path;
	copy.options["snapshot_id"].push_back(Value::BIGINT(snapshot->snapshot_id));
	copy.options["sequence_number"].push_back(Value::BIGINT(snapshot->sequence_number));

	auto select = make_uniq<SelectNode>();
	for (auto &definition : definitions) {
		copy.options["index_name"].push_back(Value(definition.name));
		copy.options["stable_id_field_id"].push_back(Value::INTEGER(definition.stable_id_field_id));
		copy.options["content_field_id"].push_back(Value::INTEGER(definition.content_field_id));
		select->select_list.push_back(make_uniq<ColumnRefExpression>(definition.stable_id_column, table.name));
		select->select_list.push_back(make_uniq<ColumnRefExpression>(definition.content_column, table.name));
		select->select_list.push_back(make_uniq<ColumnRefExpression>("filename", table.name));
		select->select_list.push_back(make_uniq<ColumnRefExpression>("file_row_number", table.name));
	}
	auto source = make_uniq<BaseTableRef>();
	source->catalog_name = table.catalog.GetName();
	source->schema_name = table.schema.name;
	source->table_name = table.name;
	select->from_table = std::move(source);
	copy.select_statement = std::move(select);
	auto copy_binder = Binder::CreateBinder(binder.context);
	return copy_binder->Bind(*copy_statement).plan;
}

} // namespace duckdb
