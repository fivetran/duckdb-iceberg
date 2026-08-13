#include "function/iceberg_functions.hpp"

#include "catalog/rest/catalog_entry/table/iceberg_table_entry.hpp"
#include "catalog/rest/catalog_entry/table/iceberg_table_information.hpp"
#include "common/iceberg_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "fivetran/fivetran_ai_index_definition.hpp"
#include "fivetran/fivetran_ai_refresh.hpp"

namespace duckdb {

static unique_ptr<LogicalOperator> BindFivetranAIRefresh(ClientContext &context, TableFunctionBindInput &input,
                                                         idx_t bind_index, vector<string> &return_names) {
	if (!input.binder) {
		throw InternalException("fivetran_ai_refresh_indexes requires a binder");
	}
	auto table_name = input.inputs[0].GetValue<string>();
	auto entry = IcebergUtils::GetTableEntry(context, table_name);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY || entry->ParentCatalog().GetCatalogType() != "iceberg") {
		throw InvalidInputException("ai.refresh_indexes currently requires an Iceberg table");
	}
	auto &table = entry->Cast<IcebergTableEntry>();
	auto &metadata = table.table_info.table_metadata;
	auto &schemas = metadata.GetSchemas();
	auto schema = schemas.find(metadata.GetCurrentSchemaId());
	if (schema == schemas.end()) {
		throw InternalException("Iceberg table has no current schema");
	}
	auto definitions = GetFivetranAIBM25IndexDefinitions(metadata.table_properties, *schema->second);

	DatabaseModificationType modification;
	modification |= DatabaseModificationType::ALTER_TABLE;
	input.binder->GetStatementProperties().RegisterDBModify(entry->ParentCatalog(), context, modification);
	auto plan = BindFivetranAIBM25Refresh(*input.binder, table, std::move(definitions), false);
	auto bindings = plan->GetColumnBindings();
	if (bindings.size() != 1) {
		throw InternalException("FIVETRAN_AI Puffin COPY returned an unexpected shape");
	}
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, bindings[0]));
	auto projection = make_uniq<LogicalProjection>(bind_index, std::move(expressions));
	projection->children.push_back(std::move(plan));
	return_names.push_back("Count");
	return std::move(projection);
}

TableFunctionSet IcebergFunctions::GetFivetranAIRefreshFunction() {
	TableFunctionSet result("fivetran_ai_refresh_indexes");
	TableFunction function({LogicalType::VARCHAR}, nullptr);
	function.bind_operator = BindFivetranAIRefresh;
	result.AddFunction(std::move(function));
	return result;
}

} // namespace duckdb
