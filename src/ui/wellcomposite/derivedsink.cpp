// 层：视图
#include "derivedsink.h"

#include "wellcompositepanel.h"

#include "../../catalog/datacatalog.h"
#include "../../workflow/derivedassets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace WellComposite
{

namespace
{
// catalogPath = <projectDir>/artifacts/metadata/catalog.json → projectDir。
// catalog 未打开/路径形状异常 → 空串（调用方按「工程目录未解析」如实报错）。
QString projectDirOfCatalog(const QString &catalogPath)
{
  const QString suffix = QStringLiteral("/artifacts/metadata/catalog.json");
  return catalogPath.endsWith(suffix) ? catalogPath.left(catalogPath.size() - suffix.size())
                                      : QString();
}
} // namespace

QPointer<WellCompositeDerivedSink> WellCompositeDerivedSink::s_default = nullptr;

QList<QPointer<WellCompositePanel>> &WellCompositeDerivedSink::livePanels()
{
  static QList<QPointer<WellCompositePanel>> panels;
  return panels;
}

WellCompositeDerivedSink::WellCompositeDerivedSink(QObject *parent)
  : QObject(parent)
{
}

void WellCompositeDerivedSink::setDefault(WellCompositeDerivedSink *sink)
{
  s_default = sink;
  // 迟装补挂：sink 建立前已构造的面板（测试直驱/非常规装配序）在此接上。
  if (sink)
  {
    auto &panels = livePanels();
    for (auto it = panels.begin(); it != panels.end();)
    {
      if (*it)
      {
        sink->watch(*it);
        ++it;
      }
      else
      {
        it = panels.erase(it);
      }
    }
  }
}

WellCompositeDerivedSink *WellCompositeDerivedSink::defaultSink()
{
  return s_default;
}

void WellCompositeDerivedSink::registerPanel(WellCompositePanel *panel)
{
  if (!panel)
    return;
  auto &panels = livePanels();
  if (!panels.contains(panel))
    panels.append(panel);
  if (s_default)
    s_default->watch(panel);
}

void WellCompositeDerivedSink::bind(DataCatalog *catalog, const QString &projectDir)
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

void WellCompositeDerivedSink::setSerializer(SerializeFn fn)
{
  m_serialize = std::move(fn);
}

void WellCompositeDerivedSink::setDepthTableParsers(DeviationParseFn deviation,
                                                    TimeDepthParseFn timeDepth)
{
  m_parseDeviation = std::move(deviation);
  m_parseTimeDepth = std::move(timeDepth);
}

void WellCompositeDerivedSink::watch(WellCompositePanel *panel)
{
  if (!panel)
    return;
  // 意图信号 → DERIVED 登记（面板只发意图；落盘/登记在这里，工作流层执行；
  // registered/failed 由 registerDerived 单点发射——这里不重复）
  connect(panel, &WellCompositePanel::derivedDocumentReady, this,
          [this, panel](const ComprehensiveWellData &doc, const QString &summary,
                        const QStringList &auditLines) {
            Q_UNUSED(summary); // 摘要进 extra（manifest 风格）；审计行进工作表
            QString err;
            QString path;
            registerDerived(doc, auditLines, panel->sourceDataPath(), &err, &path);
          });
  // 装载完成 → 井斜/时深自动喂表（壳注入的 io 解析器；缺表/缺注入如实回空）
  connect(panel, &WellCompositePanel::wellLoaded, this, [this, panel](const QString &) {
    feedDepthTables(panel, panel->sourceDataPath());
    if (m_alignmentProvider)
      m_alignmentProvider(panel);
  });
}

void WellCompositeDerivedSink::setAlignmentProvider(std::function<void(WellCompositePanel *)> provider)
{
  m_alignmentProvider = std::move(provider);
  refreshAlignments();
}

void WellCompositeDerivedSink::refreshAlignments()
{
  if (m_alignmentProvider)
    for (const auto &panel : livePanels())
      if (panel)
        m_alignmentProvider(panel);
}

void WellCompositeDerivedSink::feedDepthTables(WellCompositePanel *panel,
                                               const QString &sourcePath)
{
  if (!panel || sourcePath.isEmpty() || !hasDepthTableParsers())
    return;
  QVector<DeviationStation> stations;
  QString devErr;
  const bool hasDev = m_parseDeviation(sourcePath, &stations, &devErr);
  QVector<QPair<double, double>> pairs;
  QString tdErr;
  const bool hasTd = m_parseTimeDepth(sourcePath, &pairs, &tdErr);
  if (hasDev || hasTd)
    panel->applyDepthTables(hasDev ? stations : QVector<DeviationStation>(), hasTd ? pairs
                                                                                   : QVector<QPair<double, double>>());
  emit depthTablesApplied(sourcePath, hasDev, hasTd);
}

QString WellCompositeDerivedSink::registerDerived(const ComprehensiveWellData &doc,
                                                  const QStringList &auditLines,
                                                  const QString &sourcePath, QString *error,
                                                  QString *managedPath)
{
  if (error)
    error->clear();
  if (!isBound())
  {
    const QString msg = QStringLiteral("catalog 未绑定——派生版本无法登记（壳需先 bind）");
    if (error)
      *error = msg;
    emit derivedFailed(msg);
    return QString();
  }
  // 工程目录：显式 bind 优先；未给则按 catalog 当前打开的工程解析（换工程
  // 后无需重绑）。解析不到 = 如实失败，不落临时目录。
  const QString projectDir = !m_projectDir.isEmpty()
                                 ? m_projectDir
                                 : projectDirOfCatalog(m_catalog->catalogPath());
  if (projectDir.isEmpty())
  {
    const QString msg = QStringLiteral("工程目录未解析（catalog 未打开工程）——派生版本无法登记");
    if (error)
      *error = msg;
    emit derivedFailed(msg);
    return QString();
  }
  if (!m_serialize)
  {
    const QString msg = QStringLiteral(
        "派生序列化器未注入——组装根需绑定 io::writeComprehensiveWellXml"
        "（ui→io 白名单只放行 lasdoc.h，函数经装配期注入）");
    if (error)
      *error = msg;
    emit derivedFailed(msg);
    return QString();
  }

  // 受管文件名：井名净化后仍是合法路径段才用，否则退通用名（不造坏路径）。
  QString wellSeg = doc.wellName;
  wellSeg.remove(QRegularExpression(QStringLiteral("[/\\\\<>:\"|?*\\x00-\\x1f]")));
  wellSeg = wellSeg.trimmed();
  if (wellSeg.isEmpty() || !DataCatalog::isSafePathSegment(wellSeg))
    wellSeg = QStringLiteral("well");
  const QString fileName = QStringLiteral("%1_derived.xml").arg(wellSeg);

  // 派生资产沿用源资产 type（预览路由与源一致）；解析不到 → unknown
  //（.xml 的综合柱状图预览分支按扩展名路由，不依赖 type）。
  QString assetType = QStringLiteral("unknown");
  DerivedAssetRegistrar registrar(m_catalog, projectDir);
  const QStringList parents = registrar.parentVersionIdsFor(QStringList{sourcePath});
  for (const QString &pid : parents)
  {
    const CatalogVersion v = m_catalog->versionById(pid);
    if (!v.assetId.isEmpty())
    {
      const CatalogAsset a = m_catalog->assetById(v.assetId);
      if (!a.id.isEmpty())
      {
        assetType = a.type;
        break;
      }
    }
  }

  QString stageErr;
  const DerivedStaging st = registrar.stage(assetType,
      QStringLiteral("%1（编辑派生）").arg(doc.wellName.isEmpty() ? QStringLiteral("井") : doc.wellName),
      fileName, &stageErr);
  if (!st.isValid())
  {
    if (error)
      *error = stageErr;
    emit derivedFailed(stageErr);
    return QString();
  }

  const QByteArray bytes = m_serialize(doc, auditLines);
  if (bytes.isEmpty())
  {
    const QString msg = QStringLiteral("派生 XML 序列化产物为空：%1").arg(sourcePath);
    if (error)
      *error = msg;
    emit derivedFailed(msg);
    return QString();
  }
  {
    QFile f(st.absolutePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      const QString msg = QStringLiteral("无法写派生产物 %1").arg(st.absolutePath);
      if (error)
        *error = msg;
      emit derivedFailed(msg);
      return QString();
    }
    f.write(bytes);
    f.close();
  }

  QVariantMap extra;
  extra.insert(QStringLiteral("origin"), QStringLiteral("wellcomposite-edit"));
  extra.insert(QStringLiteral("well"), doc.wellName);
  extra.insert(QStringLiteral("auditCount"), auditLines.size());

  QString commitErr;
  if (!registrar.commit(st, parents, sourcePath, extra, &commitErr))
  {
    if (error)
      *error = commitErr;
    emit derivedFailed(commitErr);
    return QString();
  }
  if (managedPath)
    *managedPath = st.absolutePath;
  emit derivedRegistered(st.absolutePath, st.versionId);
  return st.versionId;
}

} // namespace WellComposite
