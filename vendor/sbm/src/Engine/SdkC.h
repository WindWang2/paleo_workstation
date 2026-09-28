/* Stable C ABI over the seismic engine SDK (Stage I, optional layer).
 *
 * Contracts:
 *   - opaque handles, no C++ types in this header;
 *   - UTF-8 paths;
 *   - every function returns sf3_status (code 0 = ok, message is a fixed buffer);
 *   - result buffers are allocated by the engine and released with sf3_slice_free
 *     (or sf3_buffer_free for traces);
 *   - a dataset is used by one thread at a time;
 *   - sf3_close is idempotent; calling other functions after close is an error.
 */
#ifndef SEISMIC_SDK_C_H
#define SEISMIC_SDK_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF3_STATUS_MESSAGE_BYTES 256

typedef struct sf3_dataset sf3_dataset;

typedef struct sf3_status {
    int32_t code; /* 0 ok, 1 invalid argument, 2 not found, 3 not indexed, 4 cancelled,
                     5 io error, 6 unsupported, 7 internal */
    char message[SF3_STATUS_MESSAGE_BYTES];
} sf3_status;

enum {
    SF3_BACKEND_AUTO = 0,
    SF3_BACKEND_DIRECT = 1,
    SF3_BACKEND_WORKSPACE = 2,
    SF3_BACKEND_PAGED = 3
};

struct sf3_metadata {
    uint32_t abi_version;
    int32_t samples;
    int32_t inline_min;
    int32_t inline_max;
    int32_t xline_min;
    int32_t xline_max;
    int32_t sample_interval_us;
    int64_t trace_count;
    int32_t rule_based;
    int32_t index_complete;
};

typedef struct sf3_slice {
    int32_t width;
    int32_t height;
    float value_min;
    float value_max;
    float* values; /* width*height, row-major; NaN = missing data */
    int32_t value_count;
    uint32_t* rgba; /* width*height packed RGBA, may be null */
    int32_t rgba_count;
} sf3_slice;

uint32_t sf3_abi_version(void);

sf3_dataset* sf3_open(const char* utf8_path, int32_t backend, sf3_status* status);
void sf3_close(sf3_dataset* dataset);

sf3_status sf3_metadata(const sf3_dataset* dataset, struct sf3_metadata* out);
sf3_status sf3_read_inline(sf3_dataset* dataset, int32_t inline_no, int32_t max_columns, sf3_slice* out);
sf3_status sf3_read_crossline(sf3_dataset* dataset, int32_t xline_no, int32_t max_columns, sf3_slice* out);
sf3_status sf3_read_time_slice(sf3_dataset* dataset, int32_t sample_index, sf3_slice* out);
/* out receives capacity floats; out_count is set to the number of samples. */
sf3_status sf3_read_trace(sf3_dataset* dataset, int32_t inline_no, int32_t xline_no,
                          float* out, int32_t capacity, int32_t* out_count);
/* inline_xline_pairs: point_count pairs (inline, xline). */
sf3_status sf3_read_section(sf3_dataset* dataset, const int32_t* inline_xline_pairs, int32_t point_count,
                            int32_t max_columns, int32_t interpolate, sf3_slice* out);
void sf3_slice_free(sf3_slice* slice);

sf3_status sf3_statistics(const sf3_dataset* dataset, uint64_t* requests, uint64_t* traces_read,
                          uint64_t* bytes_read);
sf3_status sf3_cache_stats(const sf3_dataset* dataset, uint64_t* hits, uint64_t* misses,
                           uint64_t* entries, uint64_t* bytes);
sf3_status sf3_set_cache_budget(sf3_dataset* dataset, uint64_t chunk_bytes, uint64_t slice_bytes);
void sf3_clear_caches(sf3_dataset* dataset);

#ifdef __cplusplus
}
#endif

#endif /* SEISMIC_SDK_C_H */
