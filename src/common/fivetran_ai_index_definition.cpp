#include "fivetran/fivetran_ai_index_definition.hpp"

#include "core/metadata/schema/iceberg_column_definition.hpp"
#include "core/metadata/schema/iceberg_table_schema.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <cstring>

namespace duckdb {
namespace {

optional_ptr<const IcebergColumnDefinition> FindField(const vector<unique_ptr<IcebergColumnDefinition>> &columns,
                                                      int32_t field_id) {
	for (auto &column : columns) {
		if (column->id == field_id) {
			return column.get();
		}
		auto child = FindField(column->children, field_id);
		if (child) {
			return child;
		}
	}
	return nullptr;
}

int32_t ParseFieldId(const string &value, const string &property_name) {
	if (value.empty()) {
		throw InvalidInputException("Iceberg BM25 index property '%s' is malformed", property_name);
	}
	int64_t result = 0;
	for (auto character : value) {
		if (character < '0' || character > '9') {
			throw InvalidInputException("Iceberg BM25 index property '%s' is malformed", property_name);
		}
		result = result * 10 + character - '0';
		if (result > NumericLimits<int32_t>::Maximum()) {
			throw InvalidInputException("Iceberg BM25 index property '%s' is malformed", property_name);
		}
	}
	return NumericCast<int32_t>(result);
}

} // namespace

string FivetranAIBM25PropertyName(const string &index_name) {
	return string(FIVETRAN_AI_BM25_PROPERTY_PREFIX) + StringUtil::URLEncode(index_name);
}

string FivetranAIBM25PropertyValue(int32_t stable_id_field_id, int32_t content_field_id) {
	return std::to_string(stable_id_field_id) + "," + std::to_string(content_field_id);
}

vector<FivetranAIBM25IndexDefinition>
GetFivetranAIBM25IndexDefinitions(const case_insensitive_map_t<string> &properties, const IcebergTableSchema &schema) {
	vector<FivetranAIBM25IndexDefinition> result;
	for (auto &property : properties) {
		if (!StringUtil::CIStartsWith(property.first, FIVETRAN_AI_BM25_PROPERTY_PREFIX)) {
			continue;
		}
		auto separator = property.second.find(',');
		if (separator == string::npos || property.second.find(',', separator + 1) != string::npos) {
			throw InvalidInputException("Iceberg BM25 index property '%s' is malformed", property.first);
		}
		auto stable_id_field_id = ParseFieldId(property.second.substr(0, separator), property.first);
		auto content_field_id = ParseFieldId(property.second.substr(separator + 1), property.first);
		auto stable_id = FindField(schema.columns, stable_id_field_id);
		auto content = FindField(schema.columns, content_field_id);
		if (!stable_id || !content || stable_id->type.id() != LogicalTypeId::VARCHAR ||
		    content->type.id() != LogicalTypeId::VARCHAR) {
			throw InvalidInputException("Iceberg BM25 index property '%s' refers to missing or non-VARCHAR fields",
			                            property.first);
		}
		result.push_back({StringUtil::URLDecode(property.first.substr(strlen(FIVETRAN_AI_BM25_PROPERTY_PREFIX))),
		                  stable_id_field_id, content_field_id, stable_id->name, content->name});
	}
	std::sort(result.begin(), result.end(),
	          [](const FivetranAIBM25IndexDefinition &left, const FivetranAIBM25IndexDefinition &right) {
		          return left.name < right.name;
	          });
	return result;
}

} // namespace duckdb
