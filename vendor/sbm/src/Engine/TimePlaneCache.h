#pragma once
// Disposable, exact, local time-plane cache. One bounded batch per dataset.
// This is NOT a replacement workspace format; a miss/corruption always falls back.
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>
#include <limits>
#include <cstring>
#include <cmath>
#include "Engine/Types.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace seismic { namespace engine {
class TimePlaneCache {
public:
    static constexpr std::size_t Budget = 256ull << 20;
    static std::uint64_t Hash(const void* ptr, std::size_t n) {
        auto p=static_cast<const unsigned char*>(ptr);
        std::uint64_t h=14695981039346656037ull;
        for(std::size_t i=0;i<n;++i) h=(h^p[i])*1099511628211ull;
        return h;
    }
    static bool Read(const std::filesystem::path& path, std::uint64_t key,
                     int width,int height,int sample,Slice2D& out,CancelToken* cancel) {
        out={};
        if(path.empty() || width<=0 || height<=0 || sample<0 || (cancel && cancel->IsCancelled())) return false;
        const std::uint64_t pixels=static_cast<std::uint64_t>(width)*height;
        if(pixels>Budget/4) return false;
        std::ifstream in(path,std::ios::binary);
        std::array<std::uint64_t,16> h{};
        if(!in.read(reinterpret_cast<char*>(h.data()),sizeof(h))) return false;
        const std::uint64_t bytes=pixels*4;
        std::error_code ec;
        if(h[0]!=0x31504c504d495453ull || h[1]!=key || h[2]!=static_cast<unsigned>(width) ||
           h[3]!=static_cast<unsigned>(height) || h[5]==0 || h[5]>4 ||
           bytes*h[5]>Budget || h[4]>static_cast<unsigned>(sample) ||
           static_cast<std::uint64_t>(sample)-h[4]>=h[5] ||
           h[15]!=Hash(h.data(),15*8) ||
           std::filesystem::file_size(path,ec)!=sizeof(h)+bytes*h[5] || ec) return false;
        Slice2D result; result.width=width; result.height=height;
        result.values.resize(static_cast<std::size_t>(pixels));
        in.seekg(sizeof(h)+(sample-h[4])*bytes);
        auto* dest=reinterpret_cast<char*>(result.values.data());
        for(std::size_t pos=0;pos<bytes;pos+=(1u<<20)) {
            if(cancel && cancel->IsCancelled()) return false;
            if(!in.read(dest+pos,static_cast<std::streamsize>(std::min<std::uint64_t>(1u<<20,bytes-pos)))) return false;
        }
        if(Hash(dest,bytes)!=h[6+sample-h[4]] || (cancel && cancel->IsCancelled())) return false;
        result.valueMin=std::numeric_limits<float>::max();
        result.valueMax=std::numeric_limits<float>::lowest();
        for(float v:result.values) if(std::isfinite(v)) {
            result.valueMin=std::min(result.valueMin,v); result.valueMax=std::max(result.valueMax,v);
        }
        if(result.valueMin>result.valueMax) {result.valueMin=0;result.valueMax=1;}
        result.columnsRead=result.totalColumns=width;
        out=std::move(result);
        return true;
    }
    static bool Write(const std::filesystem::path& path,std::uint64_t key,int width,int height,
                      int begin,const std::vector<std::vector<float>>& planes,CancelToken* cancel) {
        if(path.empty() || width<=0 || height<=0 || begin<0 || planes.empty() || planes.size()>4) return false;
        const std::uint64_t pixels=static_cast<std::uint64_t>(width)*height;
        if(pixels>Budget/4/planes.size()) return false;
        for(const auto& p:planes) if(p.size()!=pixels) return false;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(),ec);
        if(ec) return false;
        const auto space=std::filesystem::space(path.parent_path(),ec);
        if(ec || space.available<pixels*4*planes.size()+(64ull<<20)) return false;
        std::array<std::uint64_t,16> h{};
        h[0]=0x31504c504d495453ull;h[1]=key;h[2]=width;h[3]=height;h[4]=begin;h[5]=planes.size();
        for(std::size_t k=0;k<planes.size();++k) {
            if(cancel && cancel->IsCancelled()) return false;
            h[6+k]=Hash(planes[k].data(),pixels*4);
        }
        h[15]=Hash(h.data(),15*8);
        auto temp=path;
#ifdef _WIN32
        static std::atomic<std::uint64_t> serial{0};
        temp += L".partial."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++serial);
#else
        temp += ".partial."+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
        struct Cleanup { std::filesystem::path p; ~Cleanup(){std::error_code e;std::filesystem::remove(p,e);} } cleanup{temp};
        std::ofstream file(temp,std::ios::binary|std::ios::trunc);
        if(!file.write(reinterpret_cast<const char*>(h.data()),sizeof(h))) return false;
        for(const auto& plane:planes) {
            const auto* data=reinterpret_cast<const char*>(plane.data());
            const std::size_t bytes=plane.size()*4;
            for(std::size_t pos=0;pos<bytes;pos+=(1u<<20)) {
                if(cancel && cancel->IsCancelled()) return false;
                if(!file.write(data+pos,std::min<std::size_t>(1u<<20,bytes-pos))) return false;
            }
        }
        file.flush(); if(!file) return false; file.close(); if(!file) return false;
        if(cancel && cancel->IsCancelled()) return false;
#ifdef _WIN32
        return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
        std::filesystem::rename(temp,path,ec); return !ec;
#endif
    }
};
} }
