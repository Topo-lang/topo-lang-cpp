#include <cstdio>

namespace pipeline {

// Forward declaration for Schema (defined in types.cpp)
class Schema;

// -- Pipeline stage functions (protected) -------------------------

int ingest(int source_id) {
    std::printf("ingest: reading from source %d\n", source_id);
    return source_id * 100 + 42;
}

int validate(int raw_data) {
    std::printf("validate: checking data %d\n", raw_data);
    if (raw_data <= 0) return 0;
    return raw_data;
}

int transform(int validated) {
    std::printf("transform: processing %d\n", validated);
    return validated * 2 + 1;
}

int build_index(int validated) {
    std::printf("build_index: indexing %d\n", validated);
    return validated % 1000;
}

int store(int transformed, int indexed) {
    std::printf("store: saving transformed=%d indexed=%d\n", transformed, indexed);
    return transformed + indexed;
}

int notify(int store_result) {
    std::printf("notify: pipeline complete, result=%d\n", store_result);
    return store_result;
}

// -- Pipeline entry (public, critical priority) -------------------

int run_pipeline(int source_id) {
    int raw = ingest(source_id);
    int validated = validate(raw);
    int transformed = transform(validated);
    int indexed = build_index(validated);
    int stored = store(transformed, indexed);
    return notify(stored);
}

// -- Priority-differentiated operations (public) ------------------

void flush_buffer(int buffer_id) {
    std::printf("flush_buffer: flushing buffer %d [critical]\n", buffer_id);
}

void compact_storage() {
    std::printf("compact_storage: reclaiming space [low priority]\n");
}

void merge_indices() {
    std::printf("merge_indices: consolidating index segments [low priority]\n");
}

void collect_metrics() {
    std::printf("collect_metrics: gathering stats [background]\n");
}

void archive_processed(int batch_id) {
    std::printf("archive_processed: archiving batch %d [background]\n", batch_id);
}

// -- Private helpers ----------------------------------------------

int parse_raw(int data) {
    return data & 0x7FFFFFFF;
}

int apply_schema(int parsed, const Schema& /*schema*/) {
    // apply schema rules to parsed data
    return parsed;
}

int build_segment(int data, int segment_size) {
    return data / segment_size;
}

} // namespace pipeline
