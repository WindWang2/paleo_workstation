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

// 标准首屏曲线：三个单曲线道（道名统一「测井曲线」）；有 GR 时 GR 必为其一，
// 其余按 SP → 深电阻率 → 密度 → 声波 → 中子 → 浅电阻率 → 冲洗带 → 井径 优先级，
// 仍不足时依文件顺序补首个可用曲线。其余曲线保留为隐藏辅助道。
QList<std::shared_ptr<WellTrack>> standardCurveTracks(const QVector<CurveData> &curves);
QVector<int> defaultCurveSelection(const QVector<CurveData> &curves, int count = 3);
QString defaultCurveTrackTitle();

// 默认测井查看模板：深度道 → 地层道 → 岩性道 → 测井曲线 ×3 → 沉积相（相|亚相|微相）
// → 层序（有数据才出）。其余已有数据（地层系统、砂层组、旋回符号、取心、文本、照片、
// 辅助曲线）建为隐藏道，可从隐藏道条恢复。
QList<std::shared_ptr<WellTrack>> defaultLayoutTracks(const ComprehensiveWellData &data,
                                                      const QString &scaleRatio);
// 布局签名（类型/标题/显隐/宽度/曲线名/类型参数），用于判断当前是否默认布局
QStringList layoutSignature(const QList<TrackSpec> &specs);

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
