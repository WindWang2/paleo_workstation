// 层：数据
#pragma once
#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

#include "cachecore.h"
#include "lrucache.h"

#include <memory>

// io/ — 栅格瓦片金字塔服务（wave/io-perf-cache D3）。
//
// GeoTIFF（及 png/jpg 等任何 GDAL 栅格，D3.5）→ 内部瓦片金字塔派生存储：
//   <cacheRoot>/<sha256(canonical)[0:24]>/meta.json     身份 + 层级目录
//   <cacheRoot>/<…>/z<N>.bin                            层级瓦片包（见下）
// 瓦片 256×256 Float32（带 1 个 nodata 语义）；层级 z 把源栅格按 2^z 降采样。
//
// z<N>.bin 布局（cacheio 小端）：
//   [u32 magic PYRL][u32 level][u32 tilesX][u32 tilesY][u32 tileW][u32 tileH]
//   [state bytes：每瓦片 1 字节 0=未建 1=已存 2=全nodata]
//   [offset table：每瓦片 u64 绝对偏移（0=无）]
//   [payload 追加区：w*h f32 × 已建瓦片]
// 追加式更新：建瓦片 → payload append → 回填 offset/state。崩溃残片只会
// 让该瓦片保持「未建」，不损整包（D3.6 全 nodata 瓦片不落地——state 2 无
// payload，磁盘占用骤降）。
//
// D3.3 懒建/预建：Lazy=ensure() 只铺目录与状态表，首次 tile() 逐瓦片生成；
// Eager=ensure() 全层级全瓦片一次生成（离线批处理用）。
// D3.4 两级缓存：解码瓦片进 LRU（默认 128MB，预算治理注册），磁盘层就是
// 各 z<N>.bin。
// D3.8 线程安全：任意线程可并发 tile()；同源构建互斥（QMutex per source），
// 不同源并行；GDAL dataset 每次构建独立打开。
class RasterPyramidService
{
  public:
    explicit RasterPyramidService(QString cacheRoot);

    struct PyramidMeta
    {
        QString sourcePath;      // 规范化
        qint64 sourceMtimeMs = 0;
        qint64 sourceSize = 0;
        int width = 0, height = 0;
        int bandCount = 1;
        float nodata = -9999.0f;
        bool hasNodata = false;
        int levels = 0;          // 层级数（z = 0..levels-1）
        double gt[6] = {0, 1, 0, 0, 0, 1};
        QString crsWkt;
    };

    enum class BuildStrategy { Lazy, Eager };

    // 建立（或校验后复用）金字塔。源文件 mtime/size 变化 → 重建。Eager 生成
    // 全部瓦片（可能秒级）；Lazy 只铺目录。返回 meta。
    bool ensure(const QString &rasterPath, PyramidMeta *out = nullptr,
                QString *error = nullptr, BuildStrategy strategy = BuildStrategy::Lazy);
    bool isBuilt(const QString &rasterPath) const;

    // D3.2 瓦片请求 API：z/x/y → 栅格块（Float32 行主序）。全 nodata 瓦片回
    // allNodata=true（samples 空）。越界瓦片 → false + error。
    struct Tile
    {
        int z = 0, x = 0, y = 0;
        int width = 0, height = 0;
        QVector<float> samples;
        bool allNodata = false;
    };
    bool tile(const QString &rasterPath, int z, int x, int y, Tile *out,
              QString *error = nullptr);

    // D3.7 统计。
    struct Stats
    {
        int levels = 0;
        qint64 tilesStored = 0;
        qint64 tilesNodata = 0;
        qint64 tilesUnbuilt = 0;
        qint64 bytesOnDisk = 0;
    };
    Stats stats(const QString &rasterPath) const;

    // 失效：删源目录 + 清瓦片 LRU 相关条目。
    void invalidate(const QString &rasterPath);

    // ---- LRU 面（D3.4/D6.3）----
    void setTileCacheBudget(qint64 bytes);
    CacheStats cacheStats() const;
    void pinTile(const QString &rasterPath, int z, int x, int y);
    void unpinTile(const QString &rasterPath, int z, int x, int y);

    // 层级几何：z 层的像素尺寸 / 瓦片网格。
    static void levelGrid(const PyramidMeta &meta, int z, int *lw, int *lh,
                          int *tilesX, int *tilesY);

  private:
    struct SourceState
    {
        QMutex buildMutex; // 同源构建串行（D3.8）
    };

    QString keyFor(const QString &rasterPath) const;
    QString dirFor(const QString &rasterPath) const;
    bool loadMeta(const QString &rasterPath, PyramidMeta *out) const;
    bool writeMeta(const QString &rasterPath, const PyramidMeta &meta) const;
    bool ensureLevelFile(const QString &rasterPath, const PyramidMeta &meta, int z) const;
    // 生成瓦片并持久化；pure=true 只算不落盘。
    bool buildTile(const QString &rasterPath, const PyramidMeta &meta, int z, int x,
                   int y, Tile *out, QString *error) const;

    SourceState *stateFor(const QString &rasterPath);

    const QString m_root;
    LruCache<QString, std::shared_ptr<QVector<float>>> m_tiles; // key = srcKey|z/x/y
    mutable QMutex m_stateMutex;
    QHash<QString, std::shared_ptr<SourceState>> m_states;
};
