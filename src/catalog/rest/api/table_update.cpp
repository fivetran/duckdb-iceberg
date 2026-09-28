#include "catalog/rest/api/table_update.hpp"
#include "duckdb/common/exception.hpp"
#include "catalog/rest/iceberg_table_set.hpp"
#include "duckdb/common/file_system.hpp"
#include "core/metadata/puffin_file.hpp"

namespace duckdb {

static rest_api_objects::Schema CopySchema(const IcebergTableSchema &schema) {
	// the rest api objects are currently not copyable. Without having to modify generated code
	//  the easiest way to copy for now is to write the schema to string, then parse it again
	std::unique_ptr<yyjson_mut_doc, YyjsonDocDeleter> doc_p(yyjson_mut_doc_new(nullptr));
	yyjson_mut_doc *doc = doc_p.get();
	auto root_object = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root_object);
	IcebergCreateTableRequest::PopulateSchema(doc, root_object, schema);
	auto schema_str = ICUtils::JsonToString(std::move(doc_p));

	// Parse it back as immutable
	std::unique_ptr<yyjson_doc, YyjsonDocDeleter> new_doc(
	    yyjson_read(schema_str.c_str(), strlen(schema_str.c_str()), 0));
	yyjson_val *val = yyjson_doc_get_root(new_doc.get());
	return rest_api_objects::Schema::FromJSON(val);
}

SetPuffinStatistics::SetPuffinStatistics(const IcebergTableInformation &table_info, string statistics_path_p,
                                         vector<ExpectedPuffinBlob> expected_blobs_p, int64_t snapshot_id_p,
                                         int64_t sequence_number_p)
    : IcebergTableUpdate(TYPE, table_info), statistics_path(std::move(statistics_path_p)),
      expected_blobs(std::move(expected_blobs_p)), snapshot_id(snapshot_id_p), sequence_number(sequence_number_p) {
}

void SetPuffinStatistics::CreateUpdate(DatabaseInstance &, ClientContext &context,
                                       IcebergCommitState &commit_state) const {
	auto &file_system = FileSystem::GetFileSystem(context);
	auto file = file_system.OpenFile(statistics_path, FileOpenFlags(FileOpenFlags::FILE_FLAGS_READ));
	auto puffin = PuffinFile::Read(*file, "statistics file '" + statistics_path + "'");
	if (puffin.blobs.size() != expected_blobs.size()) {
		throw IOException("Puffin file '%s' does not contain every declared blob", statistics_path);
	}
	vector<bool> matched(expected_blobs.size(), false);
	vector<rest_api_objects::BlobMetadata> blob_metadata;
	for (auto &blob : puffin.blobs) {
		idx_t match = expected_blobs.size();
		for (idx_t index = 0; index < expected_blobs.size(); index++) {
			if (matched[index] || blob.type != expected_blobs[index].type) {
				continue;
			}
			auto property = blob.properties.find(expected_blobs[index].property_key);
			if (property != blob.properties.end() && property->second == expected_blobs[index].property_value) {
				match = index;
				break;
			}
		}
		if (blob.snapshot_id != snapshot_id || blob.sequence_number != sequence_number ||
		    match == expected_blobs.size()) {
			throw IOException("Puffin file '%s' does not match the declared publication", statistics_path);
		}
		matched[match] = true;
		blob_metadata.emplace_back();
		auto &result = blob_metadata.back();
		result.type = blob.type;
		result.snapshot_id = blob.snapshot_id;
		result.sequence_number = blob.sequence_number;
		result.fields = blob.fields;
		result.properties = blob.properties;
		result.has_properties = !blob.properties.empty();
	}
	for (auto value : matched) {
		if (!value) {
			throw IOException("Puffin file '%s' does not contain every declared blob", statistics_path);
		}
	}

	commit_state.table_change.updates.emplace_back();
	auto &table_update = commit_state.table_change.updates.back();
	table_update.has_set_statistics_update = true;
	auto &set_statistics = table_update.set_statistics_update;
	set_statistics.has_action = true;
	set_statistics.action = "set-statistics";
	set_statistics.has_snapshot_id = true;
	set_statistics.snapshot_id = snapshot_id;
	auto &statistics = set_statistics.statistics;
	statistics.snapshot_id = snapshot_id;
	statistics.statistics_path = statistics_path;
	statistics.file_size_in_bytes = NumericCast<int64_t>(puffin.file_size);
	statistics.file_footer_size_in_bytes = NumericCast<int64_t>(puffin.footer_size);
	statistics.blob_metadata = std::move(blob_metadata);

	commit_state.table_change.requirements.emplace_back();
	auto &requirement = commit_state.table_change.requirements.back();
	requirement.has_assert_ref_snapshot_id = true;
	requirement.assert_ref_snapshot_id.type.value = "assert-ref-snapshot-id";
	requirement.assert_ref_snapshot_id.ref = "main";
	requirement.assert_ref_snapshot_id.has_snapshot_id = true;
	requirement.assert_ref_snapshot_id.snapshot_id = snapshot_id;
}

AddSchemaUpdate::AddSchemaUpdate(const IcebergTableInformation &table_info, int32_t schema_id)
    : IcebergTableUpdate(IcebergTableUpdateType::ADD_SCHEMA, table_info), schema_id(schema_id) {
	if (table_info.table_metadata.HasLastColumnId()) {
		last_column_id = table_info.table_metadata.GetLastColumnId();
	}
}

void AddSchemaUpdate::CreateUpdate(DatabaseInstance &db, ClientContext &context,
                                   IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &update = commit_state.table_change.updates.back();
	update.has_add_schema_update = true;
	update.add_schema_update.has_action = true;
	update.add_schema_update.action = "add-schema";

	auto &schemas = table_info.table_metadata.GetSchemas();
	auto it = schemas.find(schema_id);
	if (it == schemas.end()) {
		throw InternalException("(AddSchemaUpdate) Couldn't find schema with id: %d", schema_id);
	}
	auto &schema = it->second;
	update.add_schema_update.schema = CopySchema(*schema.get());
	// last-column-id is technically deprecated in AddSchemaUpdate, but some catalogs still use it (nessie).
	if (last_column_id.IsValid()) {
		update.add_schema_update.has_last_column_id = true;
		update.add_schema_update.last_column_id = last_column_id.GetIndex();
	}
}

AssignUUIDUpdate::AssignUUIDUpdate(const IcebergTableInformation &table_info)
    : IcebergTableUpdate(IcebergTableUpdateType::ADD_SCHEMA, table_info) {
}

void AssignUUIDUpdate::CreateUpdate(DatabaseInstance &db, ClientContext &context,
                                    IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &update = commit_state.table_change.updates.back();
	update.has_assign_uuidupdate = true;
	update.assign_uuidupdate.action = "assign-uuid";
	update.assign_uuidupdate.has_action = true;
	// uuid most likely created by the rest catalog?
	update.assign_uuidupdate.uuid = table_info.table_metadata.table_uuid;
}

AssertCreateRequirement::AssertCreateRequirement(const IcebergTableInformation &table_info)
    : IcebergTableRequirement(IcebergTableRequirementType::ASSERT_CREATE, table_info) {
}

void AssertCreateRequirement::CreateRequirement(DatabaseInstance &db, ClientContext &context,
                                                IcebergCommitState &commit_state) {
	commit_state.table_change.requirements.push_back(rest_api_objects::TableRequirement());
	auto &req = commit_state.table_change.requirements.back();
	req.assert_create.type.value = "assert-create";
	req.has_assert_create = true;
}

AssertTableUUIDRequirement::AssertTableUUIDRequirement(const IcebergTableInformation &table_info)
    : IcebergTableRequirement(IcebergTableRequirementType::ASSERT_TABLE_UUID, table_info) {
}

void AssertTableUUIDRequirement::CreateRequirement(DatabaseInstance &db, ClientContext &context,
                                                   IcebergCommitState &commit_state) {
	commit_state.table_change.requirements.push_back(rest_api_objects::TableRequirement());
	auto &req = commit_state.table_change.requirements.back();
	req.assert_table_uuid.type.value = "assert-table-uuid";
	req.assert_table_uuid.uuid = commit_state.table_info.table_metadata.table_uuid;
	req.has_assert_table_uuid = true;
}

AssertCurrentSchemaIdRequirement::AssertCurrentSchemaIdRequirement(const IcebergTableInformation &table_info)
    : IcebergTableRequirement(IcebergTableRequirementType::ASSERT_CURRENT_SCHEMA_ID, table_info) {
	current_schema_id = table_info.table_metadata.GetCurrentSchemaId();
}

void AssertCurrentSchemaIdRequirement::CreateRequirement(DatabaseInstance &db, ClientContext &context,
                                                         IcebergCommitState &commit_state) {
	commit_state.table_change.requirements.push_back(rest_api_objects::TableRequirement());
	auto &req = commit_state.table_change.requirements.back();
	req.has_assert_current_schema_id = true;
	req.assert_current_schema_id.type.value = "assert-current-schema-id";
	req.assert_current_schema_id.current_schema_id = current_schema_id;
}

AssertLastAssignedFieldIdRequirement::AssertLastAssignedFieldIdRequirement(const IcebergTableInformation &table_info)
    : IcebergTableRequirement(IcebergTableRequirementType::ASSERT_LAST_ASSIGNED_FIELD_ID, table_info) {
	D_ASSERT(table_info.table_metadata.HasLastColumnId());
	last_assigned_field_id = static_cast<int32_t>(table_info.table_metadata.GetLastColumnId());
}

void AssertLastAssignedFieldIdRequirement::CreateRequirement(DatabaseInstance &db, ClientContext &context,
                                                             IcebergCommitState &commit_state) {
	commit_state.table_change.requirements.push_back(rest_api_objects::TableRequirement());
	auto &req = commit_state.table_change.requirements.back();
	req.has_assert_last_assigned_field_id = true;
	req.assert_last_assigned_field_id.type.value = "assert-last-assigned-field-id";
	req.assert_last_assigned_field_id.last_assigned_field_id = last_assigned_field_id;
}

AssertLastAssignedPartitionIdRequirement::AssertLastAssignedPartitionIdRequirement(
    const IcebergTableInformation &table_info)
    : IcebergTableRequirement(IcebergTableRequirementType::ASSERT_LAST_ASSIGNED_PARTITION_ID, table_info) {
	if (table_info.table_metadata.HasLastPartitionId()) {
		last_assigned_partition_id = table_info.table_metadata.GetLastPartitionFieldId();
	} else {
		// If no partition field IDs have been assigned, use 999 as the last assigned so 1000 becomes the
		// next partition id. Based on assignments in v1 in https://iceberg.apache.org/spec/#partition-evolution
		last_assigned_partition_id = 999;
	}
}

void AssertLastAssignedPartitionIdRequirement::CreateRequirement(DatabaseInstance &db, ClientContext &context,
                                                                 IcebergCommitState &commit_state) {
	commit_state.table_change.requirements.push_back(rest_api_objects::TableRequirement());
	auto &req = commit_state.table_change.requirements.back();
	req.has_assert_last_assigned_partition_id = true;
	req.assert_last_assigned_partition_id.type.value = "assert-last-assigned-partition-id";
	req.assert_last_assigned_partition_id.last_assigned_partition_id = last_assigned_partition_id;
}

AssertDefaultSpecIdRequirement::AssertDefaultSpecIdRequirement(const IcebergTableInformation &table_info)
    : IcebergTableRequirement(IcebergTableRequirementType::ASSERT_DEFAULT_SPEC_ID, table_info) {
	default_spec_id = table_info.table_metadata.default_spec_id;
}

void AssertDefaultSpecIdRequirement::CreateRequirement(DatabaseInstance &db, ClientContext &context,
                                                       IcebergCommitState &commit_state) {
	commit_state.table_change.requirements.push_back(rest_api_objects::TableRequirement());
	auto &req = commit_state.table_change.requirements.back();
	req.has_assert_default_spec_id = true;
	req.assert_default_spec_id.type.value = "assert-default-spec-id";
	req.assert_default_spec_id.default_spec_id = default_spec_id;
}

UpgradeFormatVersion::UpgradeFormatVersion(const IcebergTableInformation &table_info)
    : IcebergTableUpdate(IcebergTableUpdateType::UPGRADE_FORMAT_VERSION, table_info) {
}

void UpgradeFormatVersion::CreateUpdate(DatabaseInstance &db, ClientContext &context,
                                        IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_upgrade_format_version_update = true;
	req.upgrade_format_version_update.action = "upgrade-format-version";
	req.upgrade_format_version_update.has_action = true;
	req.upgrade_format_version_update.format_version = table_info.table_metadata.iceberg_version;
}

SetCurrentSchema::SetCurrentSchema(const IcebergTableInformation &table_info)
    : IcebergTableUpdate(IcebergTableUpdateType::SET_CURRENT_SCHEMA, table_info) {
}

void SetCurrentSchema::CreateUpdate(DatabaseInstance &db, ClientContext &context,
                                    IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_set_current_schema_update = true;
	req.set_current_schema_update.action = "set-current-schema";
	// TODO: should this be a different value? or is the rest catalog setting this again?
	req.set_current_schema_update.schema_id = table_info.table_metadata.GetCurrentSchemaId();
}

AddPartitionSpec::AddPartitionSpec(const IcebergTableInformation &table_info)
    : IcebergTableUpdate(IcebergTableUpdateType::ADD_PARTITION_SPEC, table_info) {
}

void AddPartitionSpec::CreateUpdate(DatabaseInstance &db, ClientContext &context,
                                    IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_add_partition_spec_update = true;
	req.add_partition_spec_update.has_action = true;
	req.add_partition_spec_update.action = "add-spec";
	req.add_partition_spec_update.spec.has_spec_id = true;
	// need to get the spec id from table_info() so we can also check updated tables.
	req.add_partition_spec_update.spec.spec_id = table_info.table_metadata.default_spec_id;
	if (table_info.table_metadata.HasPartitionSpec()) {
		auto &current_partition_spec = table_info.table_metadata.GetLatestPartitionSpec();
		for (auto &field : current_partition_spec.fields) {
			req.add_partition_spec_update.spec.fields.push_back(rest_api_objects::PartitionField());
			auto &updated_field = req.add_partition_spec_update.spec.fields.back();
			updated_field.name = field.GetPartitionSpecFieldName();
			updated_field.transform.value = field.transform.RawType();
			updated_field.field_id = field.partition_field_id;
			updated_field.source_id = field.source_id;
			updated_field.has_field_id = true;
		}
	}
}

AddSortOrder::AddSortOrder(const IcebergTableInformation &table_info)
    : IcebergTableUpdate(IcebergTableUpdateType::ADD_SORT_ORDER, table_info) {
}

void AddSortOrder::CreateUpdate(DatabaseInstance &db, ClientContext &context, IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_add_sort_order_update = true;
	req.add_sort_order_update.has_action = true;
	req.add_sort_order_update.action = "add-sort-order";
	if (table_info.table_metadata.HasSortOrder()) {
		req.add_sort_order_update.sort_order.order_id = table_info.table_metadata.default_sort_order_id.GetIndex();
	}

	if (table_info.table_metadata.HasSortOrder()) {
		// FIXME: is it correct to just get the latest sort order?
		auto &current_sort_order = table_info.table_metadata.GetLatestSortOrder();
		for (auto &field : current_sort_order.fields) {
			req.add_sort_order_update.sort_order.fields.push_back(rest_api_objects::SortField());
			auto &updated_field = req.add_sort_order_update.sort_order.fields.back();
			updated_field.direction.value = field.direction;
			updated_field.transform.value = field.transform.RawType();
			updated_field.null_order.value = field.null_order;
			updated_field.source_id = field.source_id;
		}
	}
}

SetDefaultSortOrder::SetDefaultSortOrder(const IcebergTableInformation &table_info)
    : IcebergTableUpdate(IcebergTableUpdateType::SET_DEFAULT_SORT_ORDER, table_info) {
}

void SetDefaultSortOrder::CreateUpdate(DatabaseInstance &db, ClientContext &context,
                                       IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_set_default_sort_order_update = true;
	req.set_default_sort_order_update.has_action = true;
	req.set_default_sort_order_update.action = "set-default-sort-order";
	D_ASSERT(table_info.table_metadata.HasSortOrder());
	req.set_default_sort_order_update.sort_order_id = table_info.table_metadata.GetLatestSortOrder().sort_order_id;
}

SetDefaultSpec::SetDefaultSpec(const IcebergTableInformation &table_info)
    : IcebergTableUpdate(IcebergTableUpdateType::SET_DEFAULT_SPEC, table_info) {
}

void SetDefaultSpec::CreateUpdate(DatabaseInstance &db, ClientContext &context,
                                  IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_set_default_spec_update = true;
	req.set_default_spec_update.has_action = true;
	req.set_default_spec_update.action = "set-default-spec";
	req.set_default_spec_update.spec_id = table_info.table_metadata.default_spec_id;
}

SetProperties::SetProperties(const IcebergTableInformation &table_info,
                             const case_insensitive_map_t<string> &properties)
    : IcebergTableUpdate(IcebergTableUpdateType::SET_PROPERTIES, table_info), properties(properties) {
}

void SetProperties::CreateUpdate(DatabaseInstance &db, ClientContext &context, IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_set_properties_update = true;
	req.set_properties_update.action = "set-properties";
	req.set_properties_update.updates = properties;
}

RemoveProperties::RemoveProperties(const IcebergTableInformation &table_info, const vector<string> &properties)
    : IcebergTableUpdate(IcebergTableUpdateType::SET_PROPERTIES, table_info), properties(properties) {
}

void RemoveProperties::CreateUpdate(DatabaseInstance &db, ClientContext &context,
                                    IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_remove_properties_update = true;
	req.remove_properties_update.action = "remove-properties";
	req.remove_properties_update.removals = properties;
}

SetLocation::SetLocation(const IcebergTableInformation &table_info)
    : IcebergTableUpdate(IcebergTableUpdateType::SET_LOCATION, table_info) {
}

void SetLocation::CreateUpdate(DatabaseInstance &db, ClientContext &context, IcebergCommitState &commit_state) const {
	commit_state.table_change.updates.push_back(rest_api_objects::TableUpdate());
	auto &req = commit_state.table_change.updates.back();
	req.has_set_location_update = true;
	req.set_location_update.action = "set-location";
	req.set_location_update.location = table_info.table_metadata.location;
}

} // namespace duckdb
