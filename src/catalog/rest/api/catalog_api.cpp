#include "catalog/rest/api/catalog_api.hpp"

#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "yyjson.hpp"

#include "catalog/rest/api/catalog_utils.hpp"
#include "iceberg_logging.hpp"
#include "catalog/rest/iceberg_catalog.hpp"
#include "catalog/rest/catalog_entry/schema/iceberg_schema_entry.hpp"
#include "catalog/rest/catalog_entry/table/iceberg_table_entry.hpp"
#include "common/iceberg_utils.hpp"
#include "rest_catalog/objects/load_view_result.hpp"
#include "rest_catalog/objects/table_identifier.hpp"
#include "catalog/rest/api/api_utils.hpp"
#include "catalog/rest/storage/iceberg_authorization.hpp"

#include "catalog/rest/api/create_view/iceberg_create_view_request.hpp"
#include "catalog/rest/api/replace_view/iceberg_replace_view_request.hpp"

#include <sys/stat.h>

#include "rest_catalog/objects/list.hpp"
#include "rest_catalog/objects/iceberg_error_response.hpp"

using namespace duckdb_yyjson;
namespace duckdb {

vector<string> IRCAPI::ParseSchemaName(const string &namespace_name) {
	idx_t start = 0;
	idx_t end = namespace_name.find(".", start);
	vector<string> ret;
	while (end != std::string::npos) {
		auto nested_identifier = namespace_name.substr(start, end - start);
		ret.push_back(nested_identifier);
		start = end + 1;
		end = namespace_name.find(".", start);
	}
	auto last_identifier = namespace_name.substr(start, end - start);
	ret.push_back(last_identifier);
	return ret;
}

[[noreturn]] static void ThrowException(const string &url, const HTTPResponse &response, const string &method) {
	D_ASSERT(!response.Success());

	if (response.HasRequestError()) {
		//! Request error - this means something went wrong performing the request
		throw IOException("%s request to endpoint '%s' failed: (ERROR %s)", method, url, response.GetRequestError());
	}
	//! FIXME: the spec defines response objects for all failure conditions, we can deserialize the response and
	//! return a more descriptive error message based on that.
	if (!response.reason.empty()) {
		throw HTTPException(response, "%s request to endpoint '%s' returned an error response (HTTP %n). Reason: %s",
		                    method, url, int(response.status), response.reason);
	}

	//! If this was not a request error this means the server responded - report the response status and response
	throw HTTPException(response, "%s request to endpoint '%s' returned an error response (HTTP %n)", method, url,
	                    int(response.status));
}

static void LogPostBody(ClientContext &context, const IRCEndpointBuilder &url_builder, const string &body) {
	if (!Logger::Get(context).ShouldLog(IcebergLogType::NAME, IcebergLogType::LEVEL)) {
		return;
	}
	idx_t truncate_limit = 10000;
	Value limit_value;
	if (context.TryGetCurrentSetting("iceberg_logging_post_body_truncate_limit", limit_value)) {
		truncate_limit = limit_value.GetValue<idx_t>();
	}
	string body_to_log;
	if (truncate_limit == 0) {
		body_to_log = "<body omitted>";
	} else if (body.size() > truncate_limit) {
		body_to_log = body.substr(0, truncate_limit) + "... (truncated)";
	} else {
		body_to_log = body;
	}
	DUCKDB_LOG(context, IcebergLogType, "POST %s body=%s", url_builder.GetURLEncoded(), body_to_log);
}

static IRCEntryLookupStatus CheckVerificationResponse(ClientContext &context, HTTPStatusCode &status) {
	// The following response codes return "schema does not exist"
	// This list can change, some error codes we want to surface to the user (i.e PaymentRequired_402)
	// but others not (Forbidden_403).
	// We log 400, 401, and 500 just in case.
	switch (status) {
	case HTTPStatusCode::OK_200:
	case HTTPStatusCode::NoContent_204:
		return IRCEntryLookupStatus::EXISTS;
	case HTTPStatusCode::Forbidden_403:
	case HTTPStatusCode::NotFound_404:
		return IRCEntryLookupStatus::NOT_FOUND;
		break;
	case HTTPStatusCode::BadRequest_400:
	case HTTPStatusCode::Unauthorized_401:
#ifndef DEBUG
		// Our local docker IRC can return 500 randomly, in debug we want to throw the error
		// Glue returns 500 if the schema doesn't exist.
	case HTTPStatusCode::InternalServerError_500:
#endif
		DUCKDB_LOG(context, IcebergLogType, "VerifySchemaExistence returned status code %s",
		           EnumUtil::ToString(status));
		return IRCEntryLookupStatus::API_ERROR;
	default:
		break;
	}
	return IRCEntryLookupStatus::API_ERROR;
}

bool IRCAPI::VerifyResponse(ClientContext &context, IcebergCatalog &catalog, IRCEndpointBuilder &url_builder,
                            bool execute_head) {
	HTTPHeaders headers(*context.db);
	IRCEntryLookupStatus entry_status = IRCEntryLookupStatus::API_ERROR;
	unique_ptr<HTTPResponse> response;
	if (execute_head) {
		// First response of Head request
		response = catalog.auth_handler->Request(RequestType::HEAD_REQUEST, context, url_builder, headers);
		// the httputil currently only sets 200 and 304 response to success
		// for AWS all responses < 400 are successful
		entry_status = CheckVerificationResponse(context, response->status);
		switch (entry_status) {
		case IRCEntryLookupStatus::EXISTS:
			return true;
		case IRCEntryLookupStatus::NOT_FOUND:
			return false;
		default:
			break;
		}
	}
	D_ASSERT(entry_status == IRCEntryLookupStatus::API_ERROR);
	response = catalog.auth_handler->Request(RequestType::GET_REQUEST, context, url_builder, headers);
	// if execute head has a weird response, fall back to GET just in case.
	// check response of GET REQUEST
	entry_status = CheckVerificationResponse(context, response->status);
	switch (entry_status) {
	case IRCEntryLookupStatus::EXISTS:
		return true;
	case IRCEntryLookupStatus::NOT_FOUND:
		return false;
	default:
		// both head and get responses have returned a status that is an
		// error status
		ThrowException(url_builder.GetURLEncoded(), *response, response->reason);
	}
}

bool IRCAPI::VerifySchemaExistence(ClientContext &context, IcebergCatalog &catalog, const string &schema) {
	auto namespace_items = ParseSchemaName(schema);

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(namespace_items));
	bool execute_head =
	    catalog.supported_urls.find("HEAD /v1/{prefix}/namespaces/{namespace}") != catalog.supported_urls.end();
	return VerifyResponse(context, catalog, url_builder, execute_head);
}

bool IRCAPI::VerifyTableExistence(ClientContext &context, IcebergCatalog &catalog, const IcebergSchemaEntry &schema,
                                  const string &table) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(schema.namespace_items));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("tables"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(table));
	bool execute_head = catalog.supported_urls.find("HEAD /v1/{prefix}/namespaces/{namespace}/tables/{table}") !=
	                    catalog.supported_urls.end();
	return VerifyResponse(context, catalog, url_builder, execute_head);
}

static unique_ptr<HTTPResponse> GetTableMetadata(ClientContext &context, IcebergCatalog &catalog,
                                                 const IcebergSchemaEntry &schema, const string &table) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(schema.namespace_items));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("tables"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(table));

	HTTPHeaders headers(*context.db);
	if (catalog.attach_options.access_mode == IRCAccessDelegationMode::VENDED_CREDENTIALS) {
		headers.Insert("X-Iceberg-Access-Delegation", "vended-credentials");
	}
	return catalog.auth_handler->Request(RequestType::GET_REQUEST, context, url_builder, headers);
}

APIResult<unique_ptr<const rest_api_objects::LoadTableResult>> IRCAPI::GetTable(ClientContext &context,
                                                                                IcebergCatalog &catalog,
                                                                                const IcebergSchemaEntry &schema,
                                                                                const string &table_name) {
	auto ret = APIResult<unique_ptr<const rest_api_objects::LoadTableResult>>();
	auto result = GetTableMetadata(context, catalog, schema, table_name);
	if (result->status != HTTPStatusCode::OK_200) {
		std::unique_ptr<yyjson_doc, YyjsonDocDeleter> out_doc;
		yyjson_val *error_obj = ICUtils::GetErrorMessage(result->body, out_doc);
		if (error_obj == nullptr) {
			throw InvalidConfigurationException(result->body);
		}
		ret.has_error = true;
		ret.status_ = result->status;
		ret.error_ = rest_api_objects::IcebergErrorResponse::FromJSON(error_obj);
		return ret;
	}
	ret.has_error = false;
	auto doc = ICUtils::APIResultToDoc(result->body);
	auto *metadata_root = yyjson_doc_get_root(doc.get());
	ret.result_ =
	    make_uniq<const rest_api_objects::LoadTableResult>(rest_api_objects::LoadTableResult::FromJSON(metadata_root));
	return ret;
}

APIResult<unique_ptr<const rest_api_objects::GetNamespaceResponse>>
IRCAPI::GetNamespace(ClientContext &context, IcebergCatalog &catalog, const IcebergSchemaEntry &schema) {
	if (catalog.supported_urls.find("GET /v1/{prefix}/namespaces/{namespace}") == catalog.supported_urls.end()) {
		throw NotImplementedException("This Iceberg REST catalog server does not support this operation");
	}

	auto ret = APIResult<unique_ptr<const rest_api_objects::GetNamespaceResponse>>();

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(schema.namespace_items));

	HTTPHeaders headers(*context.db);
	if (catalog.attach_options.access_mode == IRCAccessDelegationMode::VENDED_CREDENTIALS) {
		headers.Insert("X-Iceberg-Access-Delegation", "vended-credentials");
	}
	auto result = catalog.auth_handler->Request(RequestType::GET_REQUEST, context, url_builder, headers);

	if (result->status != HTTPStatusCode::OK_200) {
		std::unique_ptr<yyjson_doc, YyjsonDocDeleter> out_doc;
		yyjson_val *error_obj = ICUtils::GetErrorMessage(result->body, out_doc);
		if (error_obj == nullptr) {
			throw InvalidConfigurationException(result->body);
		}
		ret.has_error = true;
		ret.status_ = result->status;
		ret.error_ = rest_api_objects::IcebergErrorResponse::FromJSON(error_obj);
		return ret;
	}
	ret.has_error = false;
	auto doc = ICUtils::APIResultToDoc(result->body);
	auto *metadata_root = yyjson_doc_get_root(doc.get());
	ret.result_ = make_uniq<const rest_api_objects::GetNamespaceResponse>(
	    rest_api_objects::GetNamespaceResponse::FromJSON(metadata_root));
	return ret;
}

vector<rest_api_objects::TableIdentifier> IRCAPI::GetTables(ClientContext &context, IcebergCatalog &catalog,
                                                            const IcebergSchemaEntry &schema) {
	vector<rest_api_objects::TableIdentifier> all_identifiers;
	string page_token;

	do {
		auto url_builder = catalog.GetBaseUrl();
		url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
		url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
		url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(schema.namespace_items));
		url_builder.AddPathComponent(IRCPathComponent::RegularComponent("tables"));
		if (!page_token.empty()) {
			url_builder.SetParam("pageToken", IRCPathComponent::RegularComponent(page_token));
		}

		HTTPHeaders headers(*context.db);
		if (catalog.attach_options.access_mode == IRCAccessDelegationMode::VENDED_CREDENTIALS) {
			headers.Insert("X-Iceberg-Access-Delegation", "vended-credentials");
		}
		auto response = catalog.auth_handler->Request(RequestType::GET_REQUEST, context, url_builder, headers);
		if (!response->Success()) {
			if (response->status == HTTPStatusCode::Forbidden_403 ||
			    response->status == HTTPStatusCode::Unauthorized_401 ||
			    response->status == HTTPStatusCode::NotFound_404) {
				// when listing tables, if a user is not allowed to list a schema for one of the error reasons above
				// we log a warning to notify the user. We do not error, otherwise the user won't be able to see any
				// results.
				DUCKDB_LOG_WARNING(context, "GET %s returned status code %s", url_builder.GetURLEncoded(),
				                   EnumUtil::ToString(response->status));
				// return empty result if user cannot list tables for a schema.
				vector<rest_api_objects::TableIdentifier> ret;
				return ret;
			}
			auto url = url_builder.GetURLEncoded();
			ThrowException(url, *response, "GET");
		}

		auto doc = ICUtils::APIResultToDoc(response->body);
		auto *root = yyjson_doc_get_root(doc.get());
		auto list_tables_response = rest_api_objects::ListTablesResponse::FromJSON(root);

		if (!list_tables_response.has_identifiers) {
			throw NotImplementedException("List of 'identifiers' is missing, missing support for Iceberg V1");
		}

		all_identifiers.insert(all_identifiers.end(), std::make_move_iterator(list_tables_response.identifiers.begin()),
		                       std::make_move_iterator(list_tables_response.identifiers.end()));

		if (list_tables_response.has_next_page_token) {
			page_token = list_tables_response.next_page_token.value;
		} else {
			page_token.clear();
		}
	} while (!page_token.empty());

	return all_identifiers;
}

vector<IRCAPISchema> IRCAPI::GetSchemas(ClientContext &context, IcebergCatalog &catalog, const vector<string> &parent) {
	vector<IRCAPISchema> result;
	string page_token = "";
	do {
		auto url_builder = catalog.GetBaseUrl();
		url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
		url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
		if (!parent.empty()) {
			url_builder.SetParam("parent", IRCPathComponent::NamespaceComponent(parent));
		}
		if (!page_token.empty()) {
			url_builder.SetParam("pageToken", IRCPathComponent::RegularComponent(page_token));
		}
		HTTPHeaders headers(*context.db);
		auto response = catalog.auth_handler->Request(RequestType::GET_REQUEST, context, url_builder, headers);
		if (!response->Success()) {
			if (response->status == HTTPStatusCode::Forbidden_403 ||
			    response->status == HTTPStatusCode::Unauthorized_401 ||
			    response->status == HTTPStatusCode::NotFound_404) {
				// when listing tables, if a user is not allowed to list a schema for one of the error reasons above
				// we log a warning to notify the user. We do not error, otherwise the user won't be able to see any
				// results.
				DUCKDB_LOG_WARNING(context, "GET %s returned %s", url_builder.GetURLEncoded(),
				                   EnumUtil::ToString(response->status));
				// return empty result if user cannot list schemas.
				return result;
			}
			auto url = url_builder.GetURLEncoded();
			ThrowException(url, *response, "GET");
		}

		auto doc = ICUtils::APIResultToDoc(response->body);
		auto *root = yyjson_doc_get_root(doc.get());
		auto list_namespaces_response = rest_api_objects::ListNamespacesResponse::FromJSON(root);
		if (!list_namespaces_response.has_namespaces) {
			//! FIXME: old code expected 'namespaces' to always be present, but it's not a required property
			return result;
		}
		auto &schemas = list_namespaces_response.namespaces;
		for (auto &schema : schemas) {
			IRCAPISchema schema_result;
			schema_result.catalog_name = catalog.GetName();
			schema_result.items = std::move(schema.value);

			if (catalog.attach_options.support_nested_namespaces) {
				auto new_parent = parent;
				new_parent.push_back(schema_result.items.back());
				auto nested_namespaces = GetSchemas(context, catalog, new_parent);
				result.insert(result.end(), std::make_move_iterator(nested_namespaces.begin()),
				              std::make_move_iterator(nested_namespaces.end()));
			}
			result.push_back(schema_result);
		}

		if (list_namespaces_response.has_next_page_token) {
			page_token = list_namespaces_response.next_page_token.value;
		} else {
			page_token.clear();
		}
	} while (!page_token.empty());

	return result;
}

void IRCAPI::CommitMultiTableUpdate(ClientContext &context, IcebergCatalog &catalog, const string &body) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("transactions"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("commit"));
	HTTPHeaders headers(*context.db);
	headers.Insert("Content-Type", "application/json");
	LogPostBody(context, url_builder, body);
	auto response = catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, body);
	if (response->status != HTTPStatusCode::OK_200 && response->status != HTTPStatusCode::NoContent_204) {
		std::unique_ptr<yyjson_doc, YyjsonDocDeleter> out_doc;
		yyjson_val *error_obj = ICUtils::GetErrorMessage(response->body, out_doc);
		if (error_obj == nullptr) {
			throw InvalidConfigurationException(response->body);
		}
		auto error = rest_api_objects::IcebergErrorResponse::FromJSON(error_obj);
		string stack_trace;
		for (const auto &str : error._error.stack) {
			stack_trace.append(str + "\n");
		}
		DUCKDB_LOG(context, IcebergLogType, stack_trace);

		// Omit stack from error output
		error._error.stack = vector<string>();
		throw InvalidConfigurationException(
		    "Request to '%s' returned a non-200 status code (%s). \n message: %s\n type: %s\n reason: %s\n",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), error._error.message, error._error.type,
		    response->reason);
	}
}

void IRCAPI::CommitTableUpdate(ClientContext &context, IcebergCatalog &catalog, const vector<string> &schema,
                               const string &table, const string &body) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(schema));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("tables"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(table));
	HTTPHeaders headers(*context.db);
	headers.Insert("Content-Type", "application/json");
	LogPostBody(context, url_builder, body);
	auto response = catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, body);
	if (response->status != HTTPStatusCode::OK_200 && response->status != HTTPStatusCode::NoContent_204) {
		throw InvalidConfigurationException(
		    "Request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}
}

void IRCAPI::CommitTableDelete(ClientContext &context, IcebergCatalog &catalog, const vector<string> &schema,
                               const string &table) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(schema));

	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("tables"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(table));
	url_builder.SetParam("purgeRequested", IRCPathComponent::RegularComponent(
	                                           Value::BOOLEAN(catalog.attach_options.purge_requested).ToString()));

	HTTPHeaders headers(*context.db);
	auto response = catalog.auth_handler->Request(RequestType::DELETE_REQUEST, context, url_builder, headers);
	// Glue/S3Tables follow spec and return 204, apache/iceberg-rest-fixture docker image returns 200
	if (response->status != HTTPStatusCode::NoContent_204 && response->status != HTTPStatusCode::OK_200) {
		throw InvalidConfigurationException(
		    "Request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}
}

void IRCAPI::CommitTableRename(ClientContext &context, IcebergCatalog &catalog, const string &body) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("tables"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("rename"));

	HTTPHeaders headers(*context.db);
	headers.Insert("Content-Type", "application/json");
	LogPostBody(context, url_builder, body);
	auto response = catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, body);
	// Glue/S3Tables follow spec and return 204, apache/iceberg-rest-fixture docker image returns 200
	if (response->status != HTTPStatusCode::NoContent_204 && response->status != HTTPStatusCode::OK_200) {
		throw InvalidConfigurationException(
		    "Request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}
}

void IRCAPI::CommitNamespaceCreate(ClientContext &context, IcebergCatalog &catalog, string body) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	HTTPHeaders headers(*context.db);
	headers.Insert("Content-Type", "application/json");
	LogPostBody(context, url_builder, body);
	auto response = catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, body);
	if (response->status != HTTPStatusCode::OK_200) {
		throw InvalidConfigurationException(
		    "Request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}
}

void IRCAPI::CommitNamespaceDrop(ClientContext &context, IcebergCatalog &catalog,
                                 const vector<string> &namespace_items) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(namespace_items));

	HTTPHeaders headers(*context.db);
	string body = "";
	auto response = catalog.auth_handler->Request(RequestType::DELETE_REQUEST, context, url_builder, headers, body);
	// Glue/S3Tables follow spec and return 204, apache/iceberg-rest-fixture docker image returns 200
	if (response->status != HTTPStatusCode::NoContent_204 && response->status != HTTPStatusCode::OK_200) {
		throw InvalidConfigurationException(
		    "Request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}
}

void IRCAPI::CommitNamespacePropertiesUpdate(ClientContext &context, IcebergCatalog &catalog, string body,
                                             const vector<string> &namespace_items) {
	if (catalog.supported_urls.find("POST /v1/{prefix}/namespaces/{namespace}/properties") ==
	    catalog.supported_urls.end()) {
		throw NotImplementedException("This Iceberg REST catalog server does not support this operation");
	}
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(namespace_items));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("properties"));

	HTTPHeaders headers(*context.db);
	headers.Insert("Content-Type", "application/json");
	auto response = catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, body);
	if (response->status != HTTPStatusCode::OK_200) {
		throw InvalidConfigurationException(
		    "Request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}
}

rest_api_objects::LoadTableResult IRCAPI::CommitNewTable(ClientContext &context, IcebergCatalog &catalog,
                                                         const IcebergTableEntry &table) {
	auto &ic_schema = table.schema.Cast<IcebergSchemaEntry>();
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(IRCPathComponent::NamespaceComponent(ic_schema.namespace_items));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("tables"));

	std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p(yyjson_mut_doc_new(nullptr));
	yyjson_mut_doc *doc = doc_p.get();
	auto root_object = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root_object);

	auto create_transaction = make_uniq<IcebergCreateTableRequest>(table.table_info);
	// if stage create is supported, create the table with stage_create = true and the table update will
	// commit the table.
	auto stage_create_tables = catalog.attach_options.stage_create_tables;
	yyjson_mut_obj_add_bool(doc, root_object, "stage-create", stage_create_tables);
	auto create_table_json = create_transaction->CreateTableToJSON(std::move(doc_p));

	try {
		HTTPHeaders headers(*context.db);
		headers.Insert("Content-Type", "application/json");
		// if you are creating a table with stage create, you need vended credentials
		if (catalog.attach_options.access_mode == IRCAccessDelegationMode::VENDED_CREDENTIALS) {
			headers.Insert("X-Iceberg-Access-Delegation", "vended-credentials");
		}
		LogPostBody(context, url_builder, create_table_json);
		auto response =
		    catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, create_table_json);
		if (response->status != HTTPStatusCode::OK_200) {
			throw InvalidConfigurationException(
			    "Request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
			    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
		}
		auto doc = ICUtils::APIResultToDoc(response->body);
		auto *root = yyjson_doc_get_root(doc.get());
		auto load_table_result = rest_api_objects::LoadTableResult::FromJSON(root);
		return load_table_result;
	} catch (const std::exception &e) {
		throw InvalidConfigurationException("Request to '%s' returned a non-200 status code body: %s",
		                                    url_builder.GetURLEncoded(), e.what());
	}
}

rest_api_objects::CatalogConfig IRCAPI::GetCatalogConfig(ClientContext &context, IcebergCatalog &catalog,
                                                         const string &warehouse) {
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("config"));
	if (!warehouse.empty()) {
		url_builder.SetParam("warehouse", IRCPathComponent::RegularComponent(warehouse));
	}
	string body = "";
	HTTPHeaders headers(*context.db);
	auto response = catalog.auth_handler->Request(RequestType::GET_REQUEST, context, url_builder, headers, body);
	if (response->status != HTTPStatusCode::OK_200) {
		throw InvalidConfigurationException("Request to '%s' returned a non-200 status code (%s), with reason: %s",
		                                    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status),
		                                    response->reason);
	}
	auto doc = ICUtils::APIResultToDoc(response->body);
	auto *root = yyjson_doc_get_root(doc.get());
	return rest_api_objects::CatalogConfig::FromJSON(root);
}

bool IRCAPI::VerifyViewExistence(ClientContext &context, IcebergCatalog &catalog, const IcebergSchemaEntry &schema,
                                 const string &view) {
	auto schema_name = IRCPathComponent::NamespaceComponent(schema.namespace_items);

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(std::move(schema_name));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("views"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(view));
	bool execute_head = catalog.supported_urls.find("HEAD /v1/{prefix}/namespaces/{namespace}/views/{view}") !=
	                    catalog.supported_urls.end();
	return VerifyResponse(context, catalog, url_builder, execute_head);
}

rest_api_objects::LoadViewResult IRCAPI::CreateView(ClientContext &context, IcebergCatalog &catalog,
                                                    IcebergSchemaEntry &schema, const string &view_name,
                                                    const string &view_sql,
                                                    shared_ptr<IcebergTableSchema> view_schema) {
	// Build URL: POST /v1/{prefix}/namespaces/{namespace}/views
	auto schema_namespace = IRCPathComponent::NamespaceComponent(schema.namespace_items);

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(std::move(schema_namespace));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("views"));

	// Build CreateViewRequest JSON
	std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p(yyjson_mut_doc_new(nullptr));
	yyjson_mut_doc *doc = doc_p.get();
	auto root_object = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root_object);
	auto create_view_request =
	    make_uniq<IcebergCreateViewRequest>(view_name, view_sql, view_schema, schema.namespace_items);
	auto create_view_json = create_view_request->CreateViewToJSON(std::move(doc_p));

	try {
		HTTPHeaders headers(*context.db);
		headers.Insert("Content-Type", "application/json");
		auto response =
		    catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, create_view_json);

		if (response->status != HTTPStatusCode::OK_200) {
			throw HTTPException(
			    *response,
			    "CreateView request to '%s' returned a non-200 status code (%s), with reason: %s. Response body: %s",
			    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
		}
		std::unique_ptr<yyjson_doc, YyjsonDocDeleter> doc(ICUtils::APIResultToDoc(response->body));
		auto *root = yyjson_doc_get_root(doc.get());
		auto load_view_result = rest_api_objects::LoadViewResult::FromJSON(root);
		return load_view_result;
	} catch (const HTTPException &) {
		throw; // Re-throw HTTP exceptions as-is
	} catch (const std::exception &e) {
		throw HTTPException(StringUtil::Format("CreateView request to '%s' failed with error: %s",
		                                       url_builder.GetURLEncoded(), e.what()));
	}
}

rest_api_objects::LoadViewResult IRCAPI::ReplaceView(ClientContext &context, IcebergCatalog &catalog,
                                                     IcebergSchemaEntry &schema, const string &view_name,
                                                     const string &view_sql, shared_ptr<IcebergTableSchema> view_schema,
                                                     const rest_api_objects::LoadViewResult &existing_view) {
	// Build URL: POST /v1/{prefix}/namespaces/{namespace}/views/{view}
	auto schema_namespace = IRCPathComponent::NamespaceComponent(schema.namespace_items);

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(std::move(schema_namespace));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("views"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(view_name));

	// Build CommitViewRequest JSON
	std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p(yyjson_mut_doc_new(nullptr));
	yyjson_mut_doc *doc = doc_p.get();
	auto root_object_replace = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root_object_replace);

	// Find the maximum version-id in the existing view
	int32_t max_version_id = 0;
	for (const auto &version : existing_view.metadata.versions) {
		if (version.version_id > max_version_id) {
			max_version_id = version.version_id;
		}
	}
	int32_t new_version_id = max_version_id + 1;

	// view_schema->schema_id == -1 signals a new schema; compute the next available ID to pass explicitly.
	int32_t new_schema_id = -1;
	if (view_schema->schema_id == -1) {
		int32_t max_schema_id = 0;
		for (const auto &s : existing_view.metadata.schemas) {
			if (s.object_1.schema_id > max_schema_id) {
				max_schema_id = s.object_1.schema_id;
			}
		}
		new_schema_id = max_schema_id + 1;
	}

	auto replace_view_request =
	    make_uniq<IcebergReplaceViewRequest>(view_name, existing_view.metadata.view_uuid, view_sql, new_version_id,
	                                         view_schema, new_schema_id, schema.namespace_items);
	auto replace_view_json = replace_view_request->ReplaceViewToJSON(std::move(doc_p));

	try {
		HTTPHeaders headers(*context.db);
		headers.Insert("Content-Type", "application/json");
		auto response =
		    catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, replace_view_json);

		if (response->status != HTTPStatusCode::OK_200) {
			throw HTTPException(
			    *response,
			    "ReplaceView request to '%s' returned a non-200 status code (%s), with reason: %s. Response body: %s",
			    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
		}

		std::unique_ptr<yyjson_doc, YyjsonDocDeleter> response_doc(ICUtils::APIResultToDoc(response->body));
		auto *root = yyjson_doc_get_root(response_doc.get());
		return rest_api_objects::LoadViewResult::FromJSON(root);
	} catch (const HTTPException &) {
		throw; // Re-throw HTTP exceptions as-is
	} catch (const std::exception &e) {
		throw HTTPException(StringUtil::Format("ReplaceView request to '%s' failed with error: %s",
		                                       url_builder.GetURLEncoded(), e.what()));
	}
}

rest_api_objects::LoadViewResult IRCAPI::SetCurrentViewVersion(ClientContext &context, IcebergCatalog &catalog,
                                                               IcebergSchemaEntry &schema, const string &view_name,
                                                               int32_t version_id,
                                                               const rest_api_objects::LoadViewResult &existing_view) {
	// Build URL: POST /v1/{prefix}/namespaces/{namespace}/views/{view}
	auto schema_namespace = IRCPathComponent::NamespaceComponent(schema.namespace_items);

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(std::move(schema_namespace));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("views"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(view_name));

	// Build CommitViewRequest JSON for setting current version
	std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p(yyjson_mut_doc_new(nullptr));
	yyjson_mut_doc *doc = doc_p.get();
	auto root_object = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root_object);

	auto requirements_arr = yyjson_mut_obj_add_arr(doc, root_object, "requirements");
	auto updates_arr = yyjson_mut_obj_add_arr(doc, root_object, "updates");

	auto req_obj = yyjson_mut_arr_add_obj(doc, requirements_arr);
	yyjson_mut_obj_add_strcpy(doc, req_obj, "type", "assert-view-uuid");
	yyjson_mut_obj_add_strcpy(doc, req_obj, "uuid", existing_view.metadata.view_uuid.c_str());

	auto update_obj = yyjson_mut_arr_add_obj(doc, updates_arr);
	yyjson_mut_obj_add_strcpy(doc, update_obj, "action", "set-current-view-version");
	yyjson_mut_obj_add_int(doc, update_obj, "view-version-id", version_id);

	auto data = yyjson_mut_val_write_opts(root_object, YYJSON_WRITE_ALLOW_INF_AND_NAN, nullptr, nullptr, nullptr);
	if (!data) {
		throw InternalException("Could not create JSON representation of view commit request");
	}
	string request_json(data);
	free(data);

	try {
		HTTPHeaders headers(*context.db);
		headers.Insert("Content-Type", "application/json");
		auto response =
		    catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, request_json);

		if (response->status != HTTPStatusCode::OK_200) {
			throw HTTPException(*response,
			                    "SetCurrentViewVersion request to '%s' returned a non-200 status code (%s), with "
			                    "reason: %s. Response body: %s",
			                    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason,
			                    response->body);
		}

		std::unique_ptr<yyjson_doc, YyjsonDocDeleter> response_doc(ICUtils::APIResultToDoc(response->body));
		auto *root = yyjson_doc_get_root(response_doc.get());
		return rest_api_objects::LoadViewResult::FromJSON(root);
	} catch (const HTTPException &) {
		throw; // Re-throw HTTP exceptions as-is
	} catch (const std::exception &e) {
		throw HTTPException(StringUtil::Format("SetCurrentViewVersion request to '%s' failed with error: %s",
		                                       url_builder.GetURLEncoded(), e.what()));
	}
}

vector<rest_api_objects::TableIdentifier> IRCAPI::GetViews(ClientContext &context, IcebergCatalog &catalog,
                                                           const IcebergSchemaEntry &schema) {
	auto schema_name = IRCPathComponent::NamespaceComponent(schema.namespace_items);

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(std::move(schema_name));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("views"));

	HTTPHeaders headers;
	auto response = catalog.auth_handler->Request(RequestType::GET_REQUEST, context, url_builder, headers);
	if (response->status != HTTPStatusCode::OK_200) {
		throw HTTPException(
		    *response, "GetViews request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}

	std::unique_ptr<yyjson_doc, YyjsonDocDeleter> doc(ICUtils::APIResultToDoc(response->body));
	auto *root = yyjson_doc_get_root(doc.get());
	auto identifiers_array = yyjson_obj_get(root, "identifiers");
	if (!identifiers_array || !yyjson_is_arr(identifiers_array)) {
		throw InvalidInputException("GetViews endpoint returned an invalid response, expected 'identifiers' array");
	}

	vector<rest_api_objects::TableIdentifier> result;
	size_t idx, max;
	yyjson_val *val;
	yyjson_arr_foreach(identifiers_array, idx, max, val) {
		result.push_back(rest_api_objects::TableIdentifier::FromJSON(val));
	}
	return result;
}

rest_api_objects::LoadViewResult IRCAPI::GetView(ClientContext &context, IcebergCatalog &catalog,
                                                 const IcebergSchemaEntry &schema, const string &view_name) {
	auto schema_namespace = IRCPathComponent::NamespaceComponent(schema.namespace_items);

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(std::move(schema_namespace));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("views"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(view_name));

	HTTPHeaders headers;
	auto response = catalog.auth_handler->Request(RequestType::GET_REQUEST, context, url_builder, headers);
	if (response->status != HTTPStatusCode::OK_200) {
		throw HTTPException(
		    *response, "GetView request to '%s' returned a non-200 status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}

	std::unique_ptr<yyjson_doc, YyjsonDocDeleter> doc(ICUtils::APIResultToDoc(response->body));
	auto *root = yyjson_doc_get_root(doc.get());
	auto load_view_result = rest_api_objects::LoadViewResult::FromJSON(root);

	return load_view_result;
}

void IRCAPI::DropView(ClientContext &context, IcebergCatalog &catalog, const vector<string> &schema,
                      const string &view_name) {
	auto schema_name = IRCPathComponent::NamespaceComponent(schema);

	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("namespaces"));
	url_builder.AddPathComponent(std::move(schema_name));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("views"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent(view_name));

	HTTPHeaders headers;
	auto response = catalog.auth_handler->Request(RequestType::DELETE_REQUEST, context, url_builder, headers);
	if (response->status != HTTPStatusCode::NoContent_204 && response->status != HTTPStatusCode::OK_200) {
		throw HTTPException(
		    *response, "DropView request to '%s' returned a non-success status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}
}

void IRCAPI::RenameView(ClientContext &context, IcebergCatalog &catalog, const vector<string> &schema,
                        const string &source_view, const string &dest_view) {
	// Build the URL: POST /v1/{prefix}/views/rename
	auto url_builder = catalog.GetBaseUrl();
	url_builder.AddPrefixComponent(catalog.prefix, catalog.prefix_is_one_component);
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("views"));
	url_builder.AddPathComponent(IRCPathComponent::RegularComponent("rename"));

	// Build the JSON payload
	std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p(yyjson_mut_doc_new(nullptr));
	yyjson_mut_doc *doc = doc_p.get();
	auto root_object = yyjson_mut_doc_get_root(doc);

	// Source identifier
	auto source_obj = yyjson_mut_obj_add_obj(doc, root_object, "source");
	auto source_namespace_arr = yyjson_mut_obj_add_arr(doc, source_obj, "namespace");
	for (const auto &item : schema) {
		yyjson_mut_arr_add_strcpy(doc, source_namespace_arr, item.c_str());
	}
	yyjson_mut_obj_add_strcpy(doc, source_obj, "name", source_view.c_str());

	// Destination identifier
	auto dest_obj = yyjson_mut_obj_add_obj(doc, root_object, "destination");
	auto dest_namespace_arr = yyjson_mut_obj_add_arr(doc, dest_obj, "namespace");
	for (const auto &item : schema) {
		yyjson_mut_arr_add_strcpy(doc, dest_namespace_arr, item.c_str());
	}
	yyjson_mut_obj_add_strcpy(doc, dest_obj, "name", dest_view.c_str());

	// Convert JSON to string
	auto data = yyjson_mut_val_write_opts(root_object, YYJSON_WRITE_ALLOW_INF_AND_NAN, nullptr, nullptr, nullptr);
	if (!data) {
		throw InternalException("Could not create JSON representation of RenameView request");
	}
	string request_json(data);
	free(data);

	HTTPHeaders headers(*context.db);
	headers.Insert("Content-Type", "application/json");
	auto response =
	    catalog.auth_handler->Request(RequestType::POST_REQUEST, context, url_builder, headers, request_json);

	if (response->status != HTTPStatusCode::NoContent_204 && response->status != HTTPStatusCode::OK_200) {
		throw InvalidConfigurationException(
		    "RenameView request to '%s' returned a non-success status code (%s), with reason: %s, body: %s",
		    url_builder.GetURLEncoded(), EnumUtil::ToString(response->status), response->reason, response->body);
	}
}
} // namespace duckdb
