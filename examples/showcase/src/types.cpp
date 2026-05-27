#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "pipeline/config.h"

namespace pipeline {

// -- Record -------------------------------------------------------

class Record {
private:
    int schema;
    int count;
    std::vector<int> fields;

public:
    explicit Record(int schema_id) : schema(schema_id), count(0), fields(16, 0) {}
    ~Record() = default;

    int field_count() const { return count; }

    int get_field(int index) const {
        if (index < 0 || index >= count) return 0;
        return fields[index];
    }

    void set_field(int index, int value) {
        if (index >= 0 && index < static_cast<int>(fields.size())) {
            fields[index] = value;
            if (index >= count) count = index + 1;
        }
    }

    int schema_id() const { return schema; }
    bool is_empty() const { return count == 0; }
};

// -- Schema -------------------------------------------------------

class Schema {
private:
    int schema_id;
    int field_count;

public:
    explicit Schema(int id) : schema_id(id), field_count(8) {}

    int field_type(int index) const {
        (void)index;
        return 1; // all fields are int type
    }

    bool accepts(const Record& record) const { return record.schema_id() == schema_id; }

    int id() const { return schema_id; }

    static Schema default_schema() { return Schema(0); }
};

// -- Batch --------------------------------------------------------

class Batch {
private:
    int count;
    int cap;
    std::vector<Record> records;

public:
    explicit Batch(int capacity) : count(0), cap(capacity) { records.reserve(capacity); }
    ~Batch() = default;

    void add(const Record& record) {
        if (count < cap) {
            records.push_back(record);
            ++count;
        }
    }

    const Record& get(int index) const { return records[index]; }

    int size() const { return count; }
    bool is_full() const { return count >= cap; }

    void clear() {
        records.clear();
        count = 0;
    }
};

// -- DataSource / FileSource (inheritance) ------------------------

class DataSource {
private:
    int64_t total_bytes;

public:
    DataSource() : total_bytes(0) {}

    int read(Batch& batch) {
        Record r(0);
        r.set_field(0, 42);
        batch.add(r);
        total_bytes += 64;
        return 1;
    }

    bool has_more() const { return true; }
    int64_t bytes_read() const { return total_bytes; }
};

class FileSource : public DataSource {
private:
    int fd;
    int64_t offset;
    int64_t total_size;

public:
    explicit FileSource(int path) : fd(path), offset(0), total_size(1024 * 1024) {}

    int64_t file_size() const { return total_size; }

    double progress() const {
        if (total_size == 0) return 1.0;
        return static_cast<double>(offset) / static_cast<double>(total_size);
    }
};

// -- Constraint adaptation: Validatable for Record ----------------

bool record_validate(Record& record) {
    return !record.is_empty() && record.schema_id() >= 0;
}

int record_error_count(const Record& record) {
    int errors = 0;
    for (int i = 0; i < record.field_count(); ++i) {
        if (record.get_field(i) < 0) ++errors;
    }
    return errors;
}

// -- Constraint adaptation: Serializable for Record ---------------

std::size_t record_serialized_size(const Record& record) {
    // header (8 bytes) + field_count * sizeof(int)
    return 8 + record.field_count() * sizeof(int);
}

void record_serialize(const Record& record, int dest) {
    std::printf("serialize Record(schema=%d, fields=%d) -> dest %d\n", record.schema_id(), record.field_count(), dest);
}

bool record_deserialize(Record& record, int src) {
    (void)src;
    record.set_field(0, 1);
    return true;
}

// -- Constraint adaptation: Indexable for Record ------------------

int64_t record_index_key(const Record& record) {
    // use schema_id and first field as composite key
    return static_cast<int64_t>(record.schema_id()) * 100000 + record.get_field(0);
}

int record_compare_key(const Record& a, const Record& b) {
    int64_t ka = record_index_key(a);
    int64_t kb = record_index_key(b);
    if (ka < kb) return -1;
    if (ka > kb) return 1;
    return 0;
}

// -- Cache<T> template --------------------------------------------

template <typename T>
class Cache {
private:
    int count;
    int max_size;

public:
    Cache() : count(0), max_size(1024) {}

    bool get(int64_t key, T& out) const {
        (void)key;
        (void)out;
        return count > 0;
    }

    void put(int64_t key, const T& value) {
        (void)key;
        (void)value;
        if (count < max_size) ++count;
    }

    void evict(int64_t key) {
        (void)key;
        if (count > 0) --count;
    }

    int size() const { return count; }
    void clear() { count = 0; }
};

// -- Index<T> template --------------------------------------------

template <typename T>
class Index {
private:
    int count;

public:
    Index() : count(0) {}

    void insert(const T& item) {
        (void)item;
        ++count;
    }

    bool lookup(int64_t key, T& out) const {
        (void)key;
        (void)out;
        return count > 0;
    }

    void remove(int64_t key) {
        (void)key;
        if (count > 0) --count;
    }

    int size() const { return count; }
};

// -- Template instantiations --------------------------------------

template class Cache<Record>;
template class Index<Record>;

// -- Compile-time conditional -------------------------------------

void use_compact_format() {
    std::printf("using compact record format (32-bit)\n");
}

void use_wide_format() {
    std::printf("using wide record format (64-bit)\n");
}

// -- Ownership functions ------------------------------------------

void submit_batch(Batch batch) {
    std::printf("submitted batch with %d records\n", batch.size());
}

int query_cache(std::shared_ptr<Cache<Record>> cache, int64_t key) {
    Record out(0);
    if (cache->get(key, out)) return out.schema_id();
    return -1;
}

bool check_source(std::weak_ptr<DataSource> source) {
    auto locked = source.lock();
    if (!locked) return false;
    return locked->has_more();
}

// -- Private helpers ----------------------------------------------

void compact_cache(int threshold) {
    std::printf("compacting cache entries below threshold %d\n", threshold);
}

void rebuild_index_segment(int segment_id) {
    std::printf("rebuilding index segment %d\n", segment_id);
}

} // namespace pipeline
