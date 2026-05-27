#include <cstdio>

namespace pipeline {

// -- Lifetime boundary: connection ---------------------------------

void connect() {
    std::printf("connect: establishing connection\n");
}

void disconnect() {
    std::printf("disconnect: closing connection\n");
}

// -- Lifetime boundary: transaction --------------------------------

void begin_transaction() {
    std::printf("begin_transaction: starting transaction\n");
}

void end_transaction() {
    std::printf("end_transaction: committing transaction\n");
}

// -- Staged session operations (protected) -------------------------

int load_schemas(int conn) {
    std::printf("load_schemas: loading from connection %d\n", conn);
    return conn + 10;
}

int warm_cache(int conn) {
    std::printf("warm_cache: preloading cache from connection %d\n", conn);
    return conn + 20;
}

int process_batches(int schemas, int cache) {
    std::printf("process_batches: schemas=%d cache=%d\n", schemas, cache);
    int processed = schemas * 2 + cache;
    return processed;
}

void report_metrics(int count) {
    std::printf("report_metrics: processed %d records\n", count);
}

// -- Multi-return (public) -----------------------------------------

void get_status(int& processed, int& errors, int& pending) {
    processed = 1024;
    errors = 3;
    pending = 47;
}

// -- Nested namespace: metrics -------------------------------------

namespace metrics {

void record_latency(int stage_id, double ms) {
    std::printf("metrics: stage %d latency %.2f ms\n", stage_id, ms);
}

void record_throughput(int records_per_sec) {
    std::printf("metrics: throughput %d rec/s\n", records_per_sec);
}

double get_p99_latency() {
    return 12.5;
}

void reset_all() {
    std::printf("metrics: reset all counters\n");
}

} // namespace metrics

// -- Private -------------------------------------------------------

void rollback_on_error() {
    std::printf("rollback_on_error: reverting changes\n");
}

void flush_buffers() {
    std::printf("flush_buffers: flushing pending writes\n");
}

bool retry_connection(int max_attempts) {
    std::printf("retry_connection: up to %d attempts\n", max_attempts);
    return max_attempts > 0;
}

// -- Internal ------------------------------------------------------

void dump_session_state() {
    std::printf("dump_session_state: [debug]\n");
}

void trace_pipeline(int batch_id) {
    std::printf("trace_pipeline: batch %d [debug]\n", batch_id);
}

// -- Entry point (public) ------------------------------------------

void run() {
    connect();
    int conn = 1;
    int schemas = load_schemas(conn);
    int cache = warm_cache(conn);
    begin_transaction();
    int count = process_batches(schemas, cache);
    end_transaction();
    report_metrics(count);
    disconnect();
}

} // namespace pipeline

int main() {
    pipeline::run();
    return 0;
}
