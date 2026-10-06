// 层：数据
#pragma once

#include "services/seismictaskservice.h"
#include "Engine/Sdk.h"
#include "Engine/Types.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <memory>

namespace seismic {

// 条目：一个已打开的 sdk::Dataset 及其并发互斥锁与 LRU 计数
struct SeismicDatasetEntry {
  std::shared_ptr<sdk::Dataset> dataset;
  QMutex mutex;          // engine 契约：一次一个线程使用 Dataset
  quint64 lastUse = 0;
};

// 条目注册表：以 shared_ptr 持有（服务与各 worker 各持引用）。worker 在
// 服务析构后仍可能排队在 registry->mutex 上——注册表必须活到最后一个
// 使用者退出，否则出现「锁在等待者手中被销毁」的析构竞态（挂死/UAF）。
struct SeismicDatasetRegistry {
  QMutex mutex;
  QHash<QString, std::shared_ptr<SeismicDatasetEntry>> entries;
  quint64 clock = 0;
};

// 条目键：路径 + 后端枚举（同一 .sgy 的 Auto 直读与显式 .sf3p 并存）。
QString entryKey(const QString &path, sdk::Backend backend);

// Slice2D → SgySliceImage 的字段平移（values/rgba 保有 NaN 语义）。
std::shared_ptr<SgySliceImage> toSliceImage(const engine::Slice2D &slice);

// 从已打开的 Dataset 提取后端状态
SeismicBackendStatus statusFromDataset(const sdk::Dataset &dataset);

// 按需打开并缓存条目。registry 以值参 shared_ptr 传入：worker 与服务共用，
// 生命周期自动延伸过任何在途 worker。
std::shared_ptr<SeismicDatasetEntry> datasetEntryFor(
    const std::shared_ptr<SeismicDatasetRegistry> &registry,
    const QString &path, sdk::Backend backend);

// 计算磁盘文件的 SHA-256 十六进制摘要
QString sha256OfFile(const QString &path);

// 去重缓存产物名：<attrId>_<scope>_<hash12>.<ext>
QString attrCacheFilePath(const QString &outputDir, const QString &attrId,
                          const QString &scope, const QString &paramHash,
                          const QString &ext);

} // namespace seismic
