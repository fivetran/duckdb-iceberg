#include "core/metadata/puffin_file.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/storage/caching_file_system.hpp"
#include "yyjson.hpp"

#include <array>
#include <functional>

using namespace duckdb_yyjson;

namespace duckdb {
namespace {

constexpr idx_t MAGIC_SIZE = 4;
constexpr idx_t TRAILER_SIZE = 12;

using ReadAt = std::function<void(data_ptr_t, idx_t, idx_t)>;

struct PuffinJsonDocDeleter {
	void operator()(yyjson_doc *document) const {
		yyjson_doc_free(document);
	}
};

int64_t RequiredInteger(yyjson_val *object, const char *name, const string &description) {
	auto value = yyjson_obj_get(object, name);
	if (!value || !yyjson_is_int(value)) {
		throw IOException("%s Puffin footer has invalid '%s'", description, name);
	}
	return yyjson_get_sint(value);
}

PuffinFileMetadata ReadPuffin(idx_t file_size, const ReadAt &read_at, const string &description) {
	if (file_size < MAGIC_SIZE * 2 + TRAILER_SIZE) {
		throw IOException("%s is too small to be a Puffin file", description);
	}
	std::array<char, MAGIC_SIZE> magic;
	read_at(data_ptr_cast(magic.data()), magic.size(), 0);
	if (string(magic.data(), magic.size()) != "PFA1") {
		throw IOException("%s has an invalid Puffin header", description);
	}
	std::array<uint8_t, TRAILER_SIZE> trailer;
	read_at(data_ptr_cast(trailer.data()), trailer.size(), file_size - trailer.size());
	if (string(char_ptr_cast(trailer.data() + 8), MAGIC_SIZE) != "PFA1") {
		throw IOException("%s has an invalid Puffin trailer", description);
	}
	auto flags = Load<uint32_t>(trailer.data() + 4);
	if (flags & 0x1) {
		throw IOException("%s has a compressed Puffin footer, which is not supported", description);
	}
	auto footer_length_signed = Load<int32_t>(trailer.data());
	if (footer_length_signed <= 0) {
		throw IOException("%s has an invalid Puffin footer size", description);
	}
	auto footer_length = NumericCast<idx_t>(footer_length_signed);
	if (footer_length > file_size - MAGIC_SIZE * 2 - TRAILER_SIZE) {
		throw IOException("%s has an invalid Puffin footer size", description);
	}
	auto footer_offset = file_size - TRAILER_SIZE - footer_length;
	read_at(data_ptr_cast(magic.data()), magic.size(), footer_offset - MAGIC_SIZE);
	if (string(magic.data(), magic.size()) != "PFA1") {
		throw IOException("%s has an invalid Puffin footer", description);
	}
	string footer(footer_length, '\0');
	read_at(data_ptr_cast(&footer[0]), footer.size(), footer_offset);
	std::unique_ptr<yyjson_doc, PuffinJsonDocDeleter> document(yyjson_read(footer.data(), footer.size(), 0));
	if (!document) {
		throw IOException("%s has invalid Puffin footer JSON", description);
	}
	auto blobs = yyjson_obj_get(yyjson_doc_get_root(document.get()), "blobs");
	if (!blobs || !yyjson_is_arr(blobs)) {
		throw IOException("%s Puffin footer has no blob metadata", description);
	}

	PuffinFileMetadata result {file_size, footer_length + MAGIC_SIZE + TRAILER_SIZE, {}};
	size_t blob_index, blob_count;
	yyjson_val *blob_value;
	yyjson_arr_foreach(blobs, blob_index, blob_count, blob_value) {
		PuffinBlobMetadata blob;
		auto type = yyjson_obj_get(blob_value, "type");
		if (!type || !yyjson_is_str(type)) {
			throw IOException("%s Puffin footer has invalid blob type", description);
		}
		blob.type = yyjson_get_str(type);
		blob.snapshot_id = RequiredInteger(blob_value, "snapshot-id", description);
		blob.sequence_number = RequiredInteger(blob_value, "sequence-number", description);
		auto offset = RequiredInteger(blob_value, "offset", description);
		auto length = RequiredInteger(blob_value, "length", description);
		if (offset < NumericCast<int64_t>(MAGIC_SIZE) || length < 0 ||
		    NumericCast<idx_t>(offset) > footer_offset - MAGIC_SIZE ||
		    NumericCast<idx_t>(length) > footer_offset - MAGIC_SIZE - NumericCast<idx_t>(offset)) {
			throw IOException("%s Puffin footer has invalid blob bounds", description);
		}
		blob.offset = NumericCast<idx_t>(offset);
		blob.length = NumericCast<idx_t>(length);
		auto fields = yyjson_obj_get(blob_value, "fields");
		if (!fields || !yyjson_is_arr(fields)) {
			throw IOException("%s Puffin footer has invalid blob fields", description);
		}
		size_t field_index, field_count;
		yyjson_val *field_value;
		yyjson_arr_foreach(fields, field_index, field_count, field_value) {
			if (!yyjson_is_int(field_value)) {
				throw IOException("%s Puffin footer has invalid blob field ID", description);
			}
			blob.fields.push_back(NumericCast<int32_t>(yyjson_get_sint(field_value)));
		}
		auto properties = yyjson_obj_get(blob_value, "properties");
		if (properties && !yyjson_is_null(properties)) {
			if (!yyjson_is_obj(properties)) {
				throw IOException("%s Puffin footer has invalid blob properties", description);
			}
			size_t property_index, property_count;
			yyjson_val *key, *value;
			yyjson_obj_foreach(properties, property_index, property_count, key, value) {
				if (!yyjson_is_str(value)) {
					throw IOException("%s Puffin footer has a non-string blob property", description);
				}
				blob.properties.emplace(yyjson_get_str(key), yyjson_get_str(value));
			}
		}
		result.blobs.push_back(std::move(blob));
	}
	return result;
}

} // namespace

PuffinFileMetadata PuffinFile::Read(FileHandle &file, const string &description) {
	return ReadPuffin(file.GetFileSize(),
	                  [&](data_ptr_t output, idx_t size, idx_t offset) { file.Read(output, size, offset); },
	                  description);
}

PuffinFileMetadata PuffinFile::Read(CachingFileHandle &file, const string &description) {
	return ReadPuffin(
	    file.GetFileSize(),
	    [&](data_ptr_t output, idx_t size, idx_t offset) {
		    data_ptr_t ignored = nullptr;
		    auto buffer = file.Read(ignored, size, offset);
		    memcpy(output, buffer.Ptr(), size);
	    },
	    description);
}

} // namespace duckdb
