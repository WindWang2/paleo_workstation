#include "Engine/SdkC.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "Engine/Sdk.h"

namespace {

sf3_status MakeStatus(seismic::engine::StatusCode code, const std::string& message) {
    sf3_status status{};
    status.code = static_cast<std::int32_t>(code);
    std::strncpy(status.message, message.c_str(), SF3_STATUS_MESSAGE_BYTES - 1);
    status.message[SF3_STATUS_MESSAGE_BYTES - 1] = '\0';
    return status;
}

sf3_status MakeStatus(const seismic::engine::Status& status) {
    return MakeStatus(status.code, status.message);
}

void FillSlice(const seismic::engine::Slice2D& slice, sf3_slice* out) {
    out->width = slice.width;
    out->height = slice.height;
    out->value_min = slice.valueMin;
    out->value_max = slice.valueMax;
    out->value_count = static_cast<std::int32_t>(slice.values.size());
    out->values = nullptr;
    out->rgba = nullptr;
    out->rgba_count = 0;
    if(!slice.values.empty()) {
        out->values = static_cast<float*>(std::malloc(slice.values.size() * sizeof(float)));
        if(out->values != nullptr) {
            std::memcpy(out->values, slice.values.data(), slice.values.size() * sizeof(float));
        }
    }
    if(!slice.rgba.empty()) {
        out->rgba = static_cast<std::uint32_t*>(std::malloc(slice.rgba.size()));
        if(out->rgba != nullptr) {
            std::memcpy(out->rgba, slice.rgba.data(), slice.rgba.size());
            out->rgba_count = static_cast<std::int32_t>(slice.rgba.size());
        }
    }
}

} // namespace

extern "C" {

uint32_t sf3_abi_version(void) {
    return seismic::sdk::kSdkAbiVersion;
}

sf3_dataset* sf3_open(const char* utf8_path, int32_t backend, sf3_status* status) {
    if(status != nullptr) {
        *status = MakeStatus(seismic::engine::StatusCode::Ok, "");
    }
    if(utf8_path == nullptr) {
        if(status != nullptr) {
            *status = MakeStatus(seismic::engine::StatusCode::InvalidArgument, "path is null");
        }
        return nullptr;
    }
    seismic::sdk::OpenOptions options;
    switch(backend) {
        case SF3_BACKEND_DIRECT: options.backend = seismic::sdk::Backend::Direct; break;
        case SF3_BACKEND_WORKSPACE: options.backend = seismic::sdk::Backend::Workspace; break;
        case SF3_BACKEND_PAGED: options.backend = seismic::sdk::Backend::Paged; break;
        default: options.backend = seismic::sdk::Backend::Auto; break;
    }
    seismic::engine::Status openStatus;
    std::shared_ptr<seismic::sdk::Dataset> dataset =
        seismic::sdk::Dataset::Open(std::filesystem::u8path(utf8_path), options, openStatus);
    if(!dataset || !openStatus.ok()) {
        if(status != nullptr) {
            *status = MakeStatus(openStatus);
        }
        return nullptr;
    }
    return reinterpret_cast<sf3_dataset*>(new (std::nothrow) std::shared_ptr<seismic::sdk::Dataset>(dataset));
}

void sf3_close(sf3_dataset* dataset) {
    auto* handle = reinterpret_cast<std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr) {
        return;
    }
    if(*handle) {
        (*handle)->Close();
    }
    delete handle;
}

sf3_status sf3_metadata(const sf3_dataset* dataset, struct sf3_metadata* out) {
    auto* handle = reinterpret_cast<const std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle || out == nullptr) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset or output");
    }
    const seismic::engine::DatasetMetadata& meta = (*handle)->Metadata();
    out->abi_version = sf3_abi_version();
    out->samples = meta.sampleCount;
    out->inline_min = meta.inlineMin;
    out->inline_max = meta.inlineMax;
    out->xline_min = meta.xlineMin;
    out->xline_max = meta.xlineMax;
    out->sample_interval_us = meta.sampleIntervalUs;
    out->trace_count = meta.traceCount;
    out->rule_based = meta.ruleBased ? 1 : 0;
    out->index_complete = meta.indexComplete ? 1 : 0;
    return MakeStatus(seismic::engine::StatusCode::Ok, "");
}

sf3_status sf3_read_inline(sf3_dataset* dataset, int32_t inline_no, int32_t max_columns, sf3_slice* out) {
    auto* handle = reinterpret_cast<std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle || out == nullptr) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset or output");
    }
    std::memset(out, 0, sizeof(*out));
    seismic::engine::Slice2D slice;
    const seismic::engine::Status status = (*handle)->ReadInline(inline_no, slice, nullptr, max_columns);
    if(!status.ok()) {
        return MakeStatus(status);
    }
    FillSlice(slice, out);
    return MakeStatus(seismic::engine::StatusCode::Ok, "");
}

sf3_status sf3_read_crossline(sf3_dataset* dataset, int32_t xline_no, int32_t max_columns, sf3_slice* out) {
    auto* handle = reinterpret_cast<std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle || out == nullptr) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset or output");
    }
    std::memset(out, 0, sizeof(*out));
    seismic::engine::Slice2D slice;
    const seismic::engine::Status status = (*handle)->ReadCrossline(xline_no, slice, nullptr, max_columns);
    if(!status.ok()) {
        return MakeStatus(status);
    }
    FillSlice(slice, out);
    return MakeStatus(seismic::engine::StatusCode::Ok, "");
}

sf3_status sf3_read_time_slice(sf3_dataset* dataset, int32_t sample_index, sf3_slice* out) {
    auto* handle = reinterpret_cast<std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle || out == nullptr) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset or output");
    }
    std::memset(out, 0, sizeof(*out));
    seismic::engine::Slice2D slice;
    const seismic::engine::Status status = (*handle)->ReadTimeSlice(sample_index, slice, nullptr);
    if(!status.ok()) {
        return MakeStatus(status);
    }
    FillSlice(slice, out);
    return MakeStatus(seismic::engine::StatusCode::Ok, "");
}

sf3_status sf3_read_trace(sf3_dataset* dataset, int32_t inline_no, int32_t xline_no,
                          float* out, int32_t capacity, int32_t* out_count) {
    auto* handle = reinterpret_cast<std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle || out == nullptr || capacity <= 0) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset or buffer");
    }
    seismic::engine::TraceData trace;
    const seismic::engine::Status status = (*handle)->ReadTrace(inline_no, xline_no, trace, nullptr);
    if(!status.ok()) {
        return MakeStatus(status);
    }
    const int32_t count = static_cast<int32_t>(trace.samples.size());
    const int32_t copied = count < capacity ? count : capacity;
    std::memcpy(out, trace.samples.data(), static_cast<std::size_t>(copied) * sizeof(float));
    if(out_count != nullptr) {
        *out_count = copied;
    }
    return MakeStatus(seismic::engine::StatusCode::Ok, "");
}

sf3_status sf3_read_section(sf3_dataset* dataset, const int32_t* inline_xline_pairs, int32_t point_count,
                            int32_t max_columns, int32_t interpolate, sf3_slice* out) {
    auto* handle = reinterpret_cast<std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle || out == nullptr || inline_xline_pairs == nullptr || point_count < 2) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset or path");
    }
    seismic::engine::SectionRequest request;
    for(int32_t i = 0; i < point_count; ++i) {
        seismic::engine::PathPoint point;
        point.inlineNo = inline_xline_pairs[i * 2];
        point.xlineNo = inline_xline_pairs[i * 2 + 1];
        request.pathPoints.push_back(point);
    }
    request.maxColumns = max_columns;
    request.interpolate = interpolate != 0;
    std::memset(out, 0, sizeof(*out));
    seismic::engine::Slice2D slice;
    const seismic::engine::Status status = (*handle)->ReadSection(request, slice, nullptr);
    if(!status.ok()) {
        return MakeStatus(status);
    }
    FillSlice(slice, out);
    return MakeStatus(seismic::engine::StatusCode::Ok, "");
}

void sf3_slice_free(sf3_slice* slice) {
    if(slice == nullptr) {
        return;
    }
    std::free(slice->values);
    std::free(slice->rgba);
    slice->values = nullptr;
    slice->rgba = nullptr;
    slice->value_count = 0;
    slice->rgba_count = 0;
}

sf3_status sf3_statistics(const sf3_dataset* dataset, uint64_t* requests, uint64_t* traces_read,
                          uint64_t* bytes_read) {
    auto* handle = reinterpret_cast<const std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset");
    }
    const seismic::engine::SourceStatistics stats = (*handle)->Statistics();
    if(requests != nullptr) {
        *requests = stats.requests;
    }
    if(traces_read != nullptr) {
        *traces_read = stats.tracesRead;
    }
    if(bytes_read != nullptr) {
        *bytes_read = stats.bytesRead;
    }
    return MakeStatus(seismic::engine::StatusCode::Ok, "");
}

sf3_status sf3_cache_stats(const sf3_dataset* dataset, uint64_t* hits, uint64_t* misses,
                           uint64_t* entries, uint64_t* bytes) {
    auto* handle = reinterpret_cast<const std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset");
    }
    const seismic::engine::CacheStats stats = (*handle)->ChunkCacheStats();
    if(hits != nullptr) {
        *hits = stats.hits;
    }
    if(misses != nullptr) {
        *misses = stats.misses;
    }
    if(entries != nullptr) {
        *entries = stats.entries;
    }
    if(bytes != nullptr) {
        *bytes = stats.bytes;
    }
    return MakeStatus(seismic::engine::StatusCode::Ok, "");
}

sf3_status sf3_set_cache_budget(sf3_dataset* dataset, uint64_t chunk_bytes, uint64_t slice_bytes) {
    auto* handle = reinterpret_cast<std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle) {
        return MakeStatus(seismic::engine::StatusCode::InvalidArgument, "invalid dataset");
    }
    return MakeStatus((*handle)->SetCacheBudget(static_cast<std::size_t>(chunk_bytes),
                                                static_cast<std::size_t>(slice_bytes)));
}

void sf3_clear_caches(sf3_dataset* dataset) {
    auto* handle = reinterpret_cast<std::shared_ptr<seismic::sdk::Dataset>*>(dataset);
    if(handle == nullptr || !*handle) {
        return;
    }
    (*handle)->ClearCaches();
}

} // extern "C"
