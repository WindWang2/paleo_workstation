// 层：数据
#pragma once
#include <QVariantList>
#include <QVariantMap>
namespace FaciesCatalog {
QVariantList library();
QVariantList defaults();
QVariantMap find(const QVariantList &schema, const QVariant &code);
QVariantMap attributes(const QVariantList &schema, const QVariant &code);
// 解析相的纹理和底色；兼容旧远端结果缺纹理、统一灰色的分类快照。
QVariantMap appearance(const QVariantMap &facies);
QString resourcePath(const QString &texture);
// 填充用可平铺纹理：图签卡（带文字的 strat_*/tex_* 图例插图）映射到
// <base>_fill.svg 变体；本身就是 _fill 名则原样解析；无变体返回空
// （调用侧退化为纯色填充 + label 标注——相名不嵌在填充图案里）。
QString fillPath(const QString &texture);
// 测井相点的「这一层的相」：井段（{top,bottom,code}）按 [top, base) 裁剪后
// 累计厚度最大的编码（优势相）；等厚取较小编码；区间内无井段返回 0。
// 不给范围（±inf）即整井口径。智能预测的井点相、井段修订与属性表改相
// 共用这一口径，点上的相与地层相属性表同源。
int dominantCode(const QVariantList &intervals, double top, double base);
// 把 [top, base) 内的井段改成 code：跨界井段在边界处切开，界外部分原样
// 保留，井段仍首尾相接。
QVariantList recodeIntervals(const QVariantList &intervals, int code,
                             double top, double base);
// 测井最终结果：按地层范围及最终编码生成唯一预测相记录，兼容旧井段结果。
QVariantMap formationPrediction(const QVariantMap &point, const QVariantList &schema,
                               const QString &horizon = {});
// 点要素的地层范围（formation_top / formation_base）；缺失或无效即整井。
void formationRange(const QVariant &top, const QVariant &base, double *outTop,
                    double *outBase);
} // namespace FaciesCatalog
