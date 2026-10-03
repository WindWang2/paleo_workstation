// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

// canonical 组名词表 —— 单一权威定义（mapping 主线1）。
//
// 此前「七个组名」的真身散在 qgislayerprofile.cpp defaultProfileGroups() 的
// 页表代码体里，layermanifest.h / layertreepanel.cpp 的「词表见 …」注释是
// 悬空指引；现在全部经本头收口：
//   · canonical 七组（固定顺序）：01_Base / 02_Prediction / 03_Constraints /
//     04_SingleFactor / 05_PaleoMap / 06_Reference / 07_Validation；
//   · 页面档案表（页 → 组集合）；
//   · 组 → 编图页映射（图层树右键跳页）；
//   · 旧组名 → canonical 别名（旧 .qgz / project.sqlite 声明兼容）。
//
// 旧名产点（workflows.cpp / io/dataimportservice.cpp / appcontext.cpp /
// registration.cpp 的历史值）不改产点、不迁移存量数据——所有消费面
//（档案摆树、树面板跳页、页面清单）统一经 canonicalize()/groupFamily()
// 在此吸收（见 docs/progress/mapping.md 主线1）。
namespace PaleoLayerVocabulary
{

inline const QString kConstraintsGroup = QStringLiteral("03_Constraints");

// canonical 七组（固定顺序；01_Base 底图 → 07_Validation 验证叠加）。
inline QStringList canonicalGroups()
{
  return {QStringLiteral("01_Base"),     QStringLiteral("02_Prediction"),
          kConstraintsGroup, QStringLiteral("04_SingleFactor"),
          QStringLiteral("05_PaleoMap"),  QStringLiteral("06_Reference"),
          QStringLiteral("07_Validation")};
}

inline bool isCanonical(const QString &group)
{
  return canonicalGroups().contains(group);
}

// 旧组名 → canonical。已 canonical / 未知组（如 "00_Data" 数据页组、
// "04_SingleFactor/Contours" 子组路径）原样返回。
inline QString canonicalize(const QString &group)
{
  if (group == QLatin1String("01_Prediction") || group == QLatin1String("03_Predict"))
    return QStringLiteral("02_Prediction");
  if (group == QLatin1String("02_Constraints"))
    return kConstraintsGroup;
  if (group == QLatin1String("03_Composite"))
    return QStringLiteral("05_PaleoMap");
  return group;
}

// canonical → 自身 + 全部旧别名（页面内清单兼容旧声明用）。
inline QStringList groupFamily(const QString &canonical)
{
  if (canonical == QLatin1String("02_Prediction"))
    return {canonical, QStringLiteral("01_Prediction"), QStringLiteral("03_Predict")};
  if (canonical == kConstraintsGroup)
    return {canonical, QStringLiteral("02_Constraints")};
  if (canonical == QLatin1String("05_PaleoMap"))
    return {canonical, QStringLiteral("03_Composite")};
  return {canonical};
}

// 子组路径的根组：04_SingleFactor/Contours → 04_SingleFactor。无斜线时就是自身。
inline QString groupRoot(const QString &group)
{
  const QString canonical = canonicalize(group);
  const int slash = canonical.indexOf(QLatin1Char('/'));
  return slash < 0 ? canonical : canonical.left(slash);
}

// 档案成员判定：声明组（可能是旧名或子组）是否落在档案组集合（canonical）内。
// 档案应用旧 .qgz 时，"01_Prediction" 产层经此不再被表外隐藏。
// 子组跟随根组：单因素等值线、制图工作场随 04_SingleFactor 出现在单因素页和编图页。
inline bool profileContains(const QStringList &profileGroups, const QString &declGroup)
{
  const QString canonical = canonicalize(declGroup);
  return profileGroups.contains(declGroup)
         || profileGroups.contains(canonical)
         || profileGroups.contains(groupRoot(canonical));
}

// 页面档案表（页 → canonical 组集合）。data 页无地图不操作画布 → 空；
// 未知页空（由调用面拒绝）。
// 注：「00_Data」（井位等原始数据组）不在 canonical 七组内，canonicalize 原样
// 透传；井位是所有编图页的基准参考层，故四张地图页档案都显式带上它——否则
// stageTreeVisibility 会把表外层整体隐藏（井位点在地图上消失）。
inline QStringList profileGroupsForPage(const QString &pageId)
{
  const QString data = QStringLiteral("00_Data");
  if (pageId == QLatin1String("predict"))
    return {data, QStringLiteral("01_Base"), QStringLiteral("02_Prediction")};
  if (pageId == QLatin1String("constraint"))
    return {data, QStringLiteral("01_Base"), kConstraintsGroup,
            QStringLiteral("04_SingleFactor")};
  if (pageId == QLatin1String("compose"))
    return {data, QStringLiteral("01_Base"), kConstraintsGroup,
            QStringLiteral("04_SingleFactor"), QStringLiteral("05_PaleoMap"),
            QStringLiteral("06_Reference")};
  if (pageId == QLatin1String("validate"))
    return {data, QStringLiteral("01_Base"), QStringLiteral("07_Validation")};
  return {};
}

// 组 → 编图页（图层树「在编图页中显示」跳转用）。返回空 pageId 时 *reason
// 带禁用原因（DESIGN.md：禁用控件必须带 reason）。旧组名先 canonicalize。
inline QString pageForGroup(const QString &group, QString *reason = nullptr)
{
  const QString canonical = groupRoot(group);
  if (reason)
    reason->clear();
  if (canonical == QLatin1String("02_Prediction"))
    return QStringLiteral("predict");
  if (canonical == kConstraintsGroup || canonical == QLatin1String("04_SingleFactor"))
    return QStringLiteral("constraint");
  if (canonical == QLatin1String("05_PaleoMap") || canonical == QLatin1String("06_Reference"))
    return QStringLiteral("compose");
  if (canonical == QLatin1String("07_Validation"))
    return QStringLiteral("validate");
  if (reason)
  {
    if (canonical == QLatin1String("01_Base"))
      *reason = QObject::tr("基础底图层（01_Base）不属于任何编图页");
    else if (canonical.isEmpty())
      *reason = QObject::tr("该图层未编入图层组，无法确定所属编图页");
    else
      *reason = QObject::tr("图层组「%1」没有对应的编图页").arg(canonical);
  }
  return QString();
}

} // namespace PaleoLayerVocabulary
