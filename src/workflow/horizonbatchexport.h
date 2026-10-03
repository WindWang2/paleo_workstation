// 层：功能
#pragma once
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../qgis/layoutexport.h"

class DataCatalog;
class QgsMapLayer;
class QgsPrintLayout;
class QgisLayerService;

// workflow/horizonbatchexport — 按层位组一键批量出图（方向 25 M4）。
//
// 语义：以用户设计好的版面为骨架，逐层位换地图内容（该层位声明图层快照 +
// 全幅范围）与标题，导出一幅，登记 catalog（OUTPUT，图件纪律：不产游离
// 文件），文件名规则化 `<工程>_<层位>_<日期>.<ext>`。版面在批处理后恢复
// 原状（图层/范围/标题/跟随态逐一还原，批处理不动用户的版面）。
//
// 计数契约（Oracle 2）：Result.horizons 与请求的 horizonLayers 键集一一对应
// ——空图层集的层位也入账（ok=false，error 写明「无声明图层」），不静默跳过。
namespace PaleoHorizonBatchExport
{

using Format = PaleoLayoutExport::Format;

struct Request
{
  QgsPrintLayout *layout = nullptr;  //!< 版面骨架（地图项 + 标题标签 id="title"）
  QString projectName;               //!< 文件名/标题用工程名（空 = "project"）
  QMap<QString, QList<QgsMapLayer *>> horizonLayers; //!< 层位 → 图层集（快照源）
  DataCatalog *catalog = nullptr;    //!< 必填：成品只落 catalog 受管区（不产游离文件）
  QString projectDir;                //!< catalog 受管区根（必填）
  double dpi = 300.0;
  Format format = Format::Png;
  QString date;                      //!< 文件名日期段（yyyyMMdd；空 = 当天）
};

struct HorizonOutcome
{
  QString horizon;
  bool ok = false;
  QString file;     //!< catalog 受管成品路径（成功时）
  QString assetId;  //!< catalog 资产 id（成功时）
  QString sha256;   //!< 成品摘要（成功时）
  QString error;    //!< 人读原因（失败时）
};

struct Result
{
  int total = 0;
  int succeeded = 0;
  int failed = 0;
  QVector<HorizonOutcome> horizons; //!< 与请求键集同序（按名排序）
  QString summary() const;          //!< 一行人读进度
};

//! 从图层清单解析层位 → 实例化图层集（horizon 非空的声明分组实例化）。
//! 返回层位名单（按声明序）；layers 为空时回空 map。实例化失败的声明跳过
//! （该层位图层集可能因此为空——run 会如实入账为失败）。
QMap<QString, QList<QgsMapLayer *>> resolveHorizonLayers( QgisLayerService *layers,
                                                          QStringList *horizonsOut = nullptr );

//! 同步核心：逐层位导出 + 登记。layout/catalog 缺失 → 整体失败（total=0）。
Result run( const Request &request );

} // namespace PaleoHorizonBatchExport
