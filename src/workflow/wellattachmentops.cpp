// 层：功能
#include "wellattachmentops.h"

#include "../catalog/datacatalog.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace paleo
{

WellAttachmentOps::WellAttachmentOps(DataCatalog *catalog)
  : m_catalog(catalog)
{
}

QVector<WellAttachmentRow> WellAttachmentOps::rowsForWell(
    const QString &wellId, const QString &projectDir) const
{
  QVector<WellAttachmentRow> out;
  if (!m_catalog || wellId.isEmpty())
    return out;
  for (const EntityAssetLink &l : m_catalog->linksForEntity(wellId))
  {
    if (l.role != QLatin1String("core") && l.role != QLatin1String("lab_analysis"))
      continue;
    const CatalogAsset a = m_catalog->assetById(l.assetId);
    if (a.id.isEmpty())
      continue;
    const CatalogVersion v = m_catalog->currentVersion(l.assetId);
    if (v.id.isEmpty())
      continue; // 资产无版本 = 管理面无内容（导入中间态），如实跳过
    WellAttachmentRow row;
    row.assetId = l.assetId;
    row.versionId = v.id;
    row.role = l.role;
    row.displayName = a.displayName;
    row.fileName = v.fileName.isEmpty() ? a.displayName : v.fileName;
    row.versionNumber = v.versionNumber;
    row.stage = v.stage;
    row.managed = v.managed;
    row.unresolved = l.unresolved;
    const QVariant depth = v.extra.value(QStringLiteral("depthMd"));
    if (depth.isValid() && std::isfinite(depth.toDouble()) &&
        depth.toDouble() > 0.0)
    {
      row.hasAnchor = true;
      row.depthMd = depth.toDouble();
      row.anchorSource =
          v.extra.value(QStringLiteral("depthMd#source")).toString();
    }
    row.path = DataCatalog::resolvedVersionPath(projectDir, v);
    out.append(row);
  }
  std::sort(out.begin(), out.end(),
            [](const WellAttachmentRow &x, const WellAttachmentRow &y) {
              // 有锚在前（按深度升序），未锚定按文件名——补锚入口稳定可寻。
              if (x.hasAnchor != y.hasAnchor)
                return x.hasAnchor;
              if (x.hasAnchor && x.depthMd != y.depthMd)
                return x.depthMd < y.depthMd;
              return x.fileName < y.fileName;
            });
  return out;
}

DepthInputStatus WellAttachmentOps::parseDepthInput(const QString &text,
                                                    double *out)
{
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty())
    return DepthInputStatus::Clear;
  // 数值 + 可选 m/米 后缀；其余后缀是单位错误（不换算不猜），与无法解析
  // 区分列因——面板/对话框按 reasonText() 出文案。
  static const QRegularExpression num(
      QStringLiteral("^([+-]?\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)\\s*(.*)$"));
  const QRegularExpressionMatch m = num.match(trimmed);
  if (!m.hasMatch())
    return DepthInputStatus::NotFinite;
  const QString suffix = m.captured(2).trimmed();
  const bool meterSuffix =
      suffix.compare(QLatin1String("m"), Qt::CaseInsensitive) == 0 ||
      suffix == QLatin1String("米");
  if (!suffix.isEmpty() && !meterSuffix)
    return DepthInputStatus::BadUnit;
  bool ok = false;
  const double value = m.captured(1).toDouble(&ok);
  if (!ok || !std::isfinite(value))
    return DepthInputStatus::NotFinite;
  if (value <= 0.0)
    return DepthInputStatus::NonPositive;
  if (out)
    *out = value;
  return DepthInputStatus::Ok;
}

bool WellAttachmentOps::setDepthAnchor(const QString &versionId,
                                       const QString &text,
                                       DepthInputStatus *status,
                                       QString *error)
{
  double depth = 0.0;
  const DepthInputStatus st = parseDepthInput(text, &depth);
  if (status)
    *status = st;
  if (!m_catalog)
  {
    if (error)
      *error = QStringLiteral("catalog not open");
    return false;
  }
  if (st == DepthInputStatus::Clear)
    return m_catalog->updateVersionExtra(versionId,
                                         QStringLiteral("depthMd"), {},
                                         error);
  if (st != DepthInputStatus::Ok)
    return false;
  return m_catalog->updateVersionExtra(versionId, QStringLiteral("depthMd"),
                                       depth, error);
}

QString WellAttachmentOps::reasonText(DepthInputStatus status)
{
  switch (status)
  {
    case DepthInputStatus::Ok:
    case DepthInputStatus::Clear:
      return QString();
    case DepthInputStatus::NotFinite:
      return QCoreApplication::translate(
          "WellAttachmentOps", "无法解析为深度数值（单位固定为米 m）");
    case DepthInputStatus::NonPositive:
      return QCoreApplication::translate(
          "WellAttachmentOps", "深度必须为正数（单位米）");
    case DepthInputStatus::BadUnit:
      return QCoreApplication::translate(
          "WellAttachmentOps", "不认识的单位后缀（深度单位固定为米 m，不换算）");
  }
  return QString();
}

} // namespace paleo
