#pragma once

struct PipelineConfig {
    int max_batch_size;
    int num_workers;
    int buffer_size_kb;
    bool enable_compression;
};
