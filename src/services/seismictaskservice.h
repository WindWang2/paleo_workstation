#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "domain/seismic/sgyindex.h"
#include "domain/seismic/sgyvolume.h"
#include "domain/seismic/sgydatacache.h"
#include "domain/seismic/sgysectionbuilder.h"

class PaleoTask;
class PaleoTaskService;

namespace seismic {

// services/ — SeismicTaskService: 地震数据异步任务协调服务
//
// 1. 将全卷扫描与几何构建接入 PaleoTaskService 线程池，UI 线程永不阻塞；
// 2. 自动协同 SgyIndexCache 实现 .sgyidx 磁盘缓存自动加载与落盘；
// 3. 内存层维护有界 LRU 缓存（SgyDataCache），切片与剖面提取自动去重复用；
// 4. 支持秒级协作取消（CancelToken），取消后不发布不完整索引与切片。
class SeismicTaskService : public QObject
{
  Q_OBJECT

public:
  explicit SeismicTaskService(PaleoTaskService *taskService = nullptr,
                              std::size_t dataCacheBudgetMb = 256,
                              QObject *parent = nullptr);
  ~SeismicTaskService() override = default;

  SgyDataCache &dataCache() { return dataCache_; }
  const SgyDataCache &dataCache() const { return dataCache_; }

  PaleoTaskService *taskService() const { return taskService_; }
  void setTaskService(PaleoTaskService *taskService) { taskService_ = taskService; }

  // 1. 异步索引 SEG-Y：优先读取磁盘缓存；未命中或强制刷新时提交至 PaleoTaskService 异步扫描
  PaleoTask *startIndexing(
      const QString &sgyPath,
      bool forceReindex,
      std::function<void(bool success, SgyIndexPtr index, const QString &error)> onFinished,
      const QString &layerId = QString());

  // 2. 异步切片提取：优先查询内存 LRU 缓存；未命中则提交后台任务提取
  PaleoTask *startSliceExtraction(
      std::shared_ptr<SgyVolume> volume,
      SgySliceType type,
      int sliceIndex,
      std::function<void(bool success, std::shared_ptr<const SgySliceImage> image, const QString &error)> onFinished);

  // 3. 异步任意剖面提取：任意折线路径剖面提取
  PaleoTask *startSectionExtraction(
      std::shared_ptr<SgyVolume> volume,
      const std::vector<glm::ivec2> &pathPoints,
      const SgySectionOptions &options,
      std::function<void(bool success, std::shared_ptr<const SgySliceImage> image, const SgySectionStats &stats, const QString &error)> onFinished);

signals:
  void indexingFinished(const QString &sgyPath, bool success);

private:
  PaleoTaskService *taskService_ = nullptr;
  SgyDataCache dataCache_;
};

} // namespace seismic
