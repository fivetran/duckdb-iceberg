#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class FileHandle;
struct CachingFileHandle;

struct PuffinBlobMetadata {
	string type;
	vector<int32_t> fields;
	int64_t snapshot_id;
	int64_t sequence_number;
	idx_t offset;
	idx_t length;
	case_insensitive_map_t<string> properties;
};

struct PuffinFileMetadata {
	idx_t file_size;
	idx_t footer_size;
	vector<PuffinBlobMetadata> blobs;
};

class PuffinFile {
public:
	static PuffinFileMetadata Read(FileHandle &file, const string &description);
	static PuffinFileMetadata Read(CachingFileHandle &file, const string &description);
};

} // namespace duckdb
