// 层：视图
#pragma once

#include <QString>
#include <QStringList>

#include <memory>

#include "trackregistry.h"

// ui/wellcomposite/trackops — 道级操作集（D1.5/D1.9/D1.7 数据注入）
//
// 纯函数工具箱：CSV 导出、道复制、spec 数据注入（曲线名 → 曲线体回填）。
// 画布右键菜单与配置对话框共用；无状态、可单测。

namespace WellComposite
{

class WellTrack;
struct ComprehensiveWellData;

namespace TrackOps
{

// D1.5 导出该道 CSV：曲线道 = 深度,曲线1,曲线2,...（NaN 留空）；区间道 =
// 顶深,底深,名称[,备注]；符号道 = 顶深,底深,符号。带 UTF-8 BOM 头方便 Excel。
QString exportTrackCsv(const WellTrack &track);

// D1.5 复制道：同类型同标题（后缀 " 副本"）同宽度，数据体深拷贝。
std::shared_ptr<WellTrack> duplicateTrack(const std::shared_ptr<WellTrack> &track);

// 曲线族 spec 数据注入：在 data 的连续/离散曲线池里按 params.curves 找曲线体，
// 应用 curveOverrides 后装进 CurveTrack；找不到的名字忽略。返回实际装入根数。
int injectCurvesFromData(CurveTrack &track, const TrackSpec &spec,
                         const QVector<CurveData> &continuousPool,
                         const QVector<CurveData> &discretePool);

// 曲线池汇总（continuous + discrete），注入查找用。
QVector<CurveData> combinedCurvePool(const QVector<CurveData> &continuous,
                                     const QVector<CurveData> &discrete);

// 以 ComprehensiveWellData 为源的便捷注入（面板装配用）
int injectCurvesFromWellData(CurveTrack &track, const TrackSpec &spec,
                             const struct ComprehensiveWellData &data);

} // namespace TrackOps

} // namespace WellComposite
