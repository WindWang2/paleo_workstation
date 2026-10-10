// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

#include "officepreviewwidget.h"
#include "../../workflow/officepreviewsession.h"  // OfficePreviewSession::supports/commitEdit
#include <QFileDialog>
#include <QPdfDocument>
#include <QPdfView>

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入（族内先例：
// datapreviewtabworkbook/tabxml 同款）。
using namespace paleo::datapreview_detail;

QWidget *DataPreviewTabs::buildContent(const QString &assetId, QWidget *page)
{
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat->assetById(assetId);
  if (asset.id.isEmpty())
    return nullptr;
  const QString chosenId = m_chosenVersionOfAsset.value(assetId);
  const CatalogVersion v = chosenId.isEmpty() ? cat->currentVersion(assetId) : m_doc->versionForPreview(chosenId);
  if (v.id.isEmpty() || v.assetId != assetId) return stateLabel(tr("所选版本不在目录中"), page, true);
  page->setProperty("previewVersionId", v.id);
  CatalogVersion sourceVersion = v; // abs 实际对应的版本（文档标签锚回 RAW 原件）
  QString abs = m_doc->absolutePathForVersion(v);
  // 文档/工作簿资产：RAW 原件是规范来源——currentVersion 可能已指向
  // DERIVED 转换件，缺失检查与「用系统程序打开」必须锚在原件上；
  // Office 原件交给本机编辑页。
  if ((asset.type == QLatin1String("document") ||
       asset.type == QLatin1String("outsource_workbook")) &&
      chosenId.isEmpty())
    for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
      if (cv.stage == QLatin1String("RAW"))
      {
        sourceVersion = cv;
        abs = m_doc->absolutePathForVersion(cv);
        break;
      }

  const auto links = cat->linksForAsset(assetId);
  // 已决井链接 → 多井标签的「井」下拉框数据源（未决链接不进列表，§4）。
  QVector<QPair<QString, QString>> wells; // (entityId, 井名)
  QString linkedBoundary;
  bool hasResolvedNonAux = false;
  bool hasAuxLink = false;
  for (const EntityAssetLink &l : links)
  {
    if (l.unresolved || l.entityId.isEmpty())
      continue;
    if (l.entityType == QLatin1String("well"))
    {
      const CatalogEntity w = cat->entityById(l.entityId);
      wells.append({l.entityId, w.name.isEmpty() ? l.entityId : w.name});
      hasResolvedNonAux = true;
      continue;
    }
    if (l.entityType == QLatin1String("sequence_boundary") && linkedBoundary.isEmpty())
      linkedBoundary = l.entityId;
    if (l.entityType == QLatin1String("auxiliary"))
      hasAuxLink = true;
    else
      hasResolvedNonAux = true;
  }
  // 固定辅助参考（§4 阶段 D）：XML 被内容判成井类但按规则钉在辅助实体上
  // （如 参考资料/ 下的 HZ28-6-1）——链接全部是 auxiliary 时一律走参考面板，
  // 绝不拿 well_head/well_log 类型去解析。
  const bool auxOnly = hasAuxLink && !hasResolvedNonAux;

  QWidget *host = new QWidget(this);
  auto *lay = new QVBoxLayout(host);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);

  // 外链/受管缺失态（§4：「找不到源文件」+路径）。外链版本（wave4）多给一个
  // 「重新定位文件…」出口——服务层流式 SHA-256 复验，内容一致才重接，不一致
  // 如实拒绝；受管文件缺失不是这条恢复路径能解的，不给按钮、只留文案。
  if (abs.isEmpty() || !QFile::exists(abs))
  {
    lay->addWidget(stateLabel(tr("找不到源文件\n%1").arg(abs.isEmpty() ? v.path : abs), host, true), 1);
    if (!sourceVersion.managed && m_doc)
    {
      auto *btn = new QPushButton(tr("重新定位文件…"), host);
      btn->setObjectName(QStringLiteral("relocateBtn"));
      const QString versionId = sourceVersion.id;
      connect(btn, &QPushButton::clicked, host, [this, assetId, versionId] {
        const QString picked = QFileDialog::getOpenFileName(
            this, tr("重新定位源文件"), QString(), QString());
        if (!picked.isEmpty())
          relocateMissingSourceWith(assetId, versionId, picked);
      });
      lay->addWidget(btn, 0, Qt::AlignHCenter);
    }
    return host;
  }

  // 外链完整性（§3）：入库时留过 SHA-256 的源文件被改过就不再解码——
  // 正文如实写「源文件与入库时的 SHA-256 不一致」。
  // D1：地震资产接了任务服务时把这道哈希移交异步解码任务——体量大不该堵
  // 住建标签；辅助 XML 的校验同样交后台解析会话。
  // 托管/无指纹/本会话已验的短路、失配后的下游标过时都在门面里。
  const bool deferShaToTask =
      OfficePreviewSession::supports(abs) ||
      (abs.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive) &&
       (auxOnly || asset.type == QLatin1String("unknown") || asset.type == QLatin1String("auxiliary") ||
        asset.type == QLatin1String("outsource_workbook"))) ||
      (m_doc->taskService() && asset.type == QLatin1String("seismic"));
  if (!deferShaToTask)
  {
    QString verr;
    if (!m_doc->verifyExternalSha(assetId, sourceVersion, &verr))
    {
      lay->addWidget(stateLabel(verr, host, true), 1);
      return host;
    }
  }

  // Office 原件交给本机编辑页。SHA 在后台核对。编辑结果另存为
  // DERIVED 版本，不覆盖 RAW，也不生成 PDF。
  if (OfficePreviewSession::supports(abs))
  {
    auto *office = new OfficePreviewWidget(abs, sourceVersion.managed ? QString() : sourceVersion.sha256, host);
    const QString editAsset = assetId;
    const QString editParent = sourceVersion.id;
    connect(office, &OfficePreviewWidget::editSaved, host, [this, office, editAsset, editParent](const QString &saved) {
      if (!m_doc || !m_doc->catalog()) return;
      QString error;
      if (OfficePreviewSession::commitEdit(m_doc->catalog(), editAsset, editParent, saved, &error))
        office->showMessage(tr("已另存为工程中的新版本，原件未改"));
      else
        office->showMessage(error, true);
    });
    lay->addWidget(office, 1);
    return host;
  }

  if (asset.type == QLatin1String("well_log") && !auxOnly)
    return buildWellLogContent(cat, asset, v, abs, assetId, wells, auxOnly,
                              links, host, lay);

  const bool wellFilterable = asset.type == QLatin1String("well_head") ||
                              asset.type == QLatin1String("well_stratification") ||
                              asset.type == QLatin1String("time_depth");

  if (wellFilterable && !auxOnly)
    return buildWellFilteredContent(cat, asset, abs, assetId, wells, host, lay);

  if (asset.type == QLatin1String("horizon"))
    return buildHorizonContent(cat, asset, v, assetId, linkedBoundary, host, lay);

  if (asset.type == QLatin1String("seismic"))
    return buildSeismicContent(cat, asset, v, abs, assetId, links, host, lay);

  if (asset.type == QLatin1String("image_reference"))
    return buildImageReferenceContent(cat, asset, v, abs, assetId, host, lay);

  if (asset.type == QLatin1String("document"))
  {
    // PDF 原件仍走 QtPdf；Office 后缀已在上方接入 Calligra，绝不转换。
    if (QFileInfo(abs).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
    {
      auto *doc = new QPdfDocument(host);
      if (doc->load(abs) == QPdfDocument::Error::None)
      {
        auto *view = new QPdfView(host);
        view->setObjectName(QStringLiteral("pdfView"));
        view->setDocument(doc);
        view->setPageMode(QPdfView::PageMode::MultiPage);
        lay->addWidget(view, 1);
      }
      else
        lay->addWidget(stateLabel(tr("PDF 原件无法加载\n%1").arg(abs), host, true), 1);
    }
    else
    {
      lay->addWidget(stateLabel(tr("该格式暂无内嵌预览"), host), 1);
      lay->addWidget(makeOpenExternalRow(abs, host));
    }
    return host;
  }

  // SpreadsheetML XML 保留原生数据表；xls/xlsx 已由 Calligra 直接预览。
  if (asset.type == QLatin1String("outsource_workbook")) {
    if (abs.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
      return buildAuxiliaryXmlContent(abs, assetId, host, lay, sourceVersion.managed ? QString() : sourceVersion.sha256);
    return buildOutsourceWorkbookContent(abs, host, lay);
  }

  if (asset.type == QLatin1String("geojson") ||
      (asset.type == QLatin1String("boundary") &&
       asset.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive)))
    return buildGeoJsonContent(cat, asset, v, abs, assetId, host, lay);

  if (isMapProductAsset(asset, v))
    return buildMapProductContent(cat, asset, v, abs, assetId, host, lay);

  // ---- 辅助/参考与未知类型（§4 阶段 D）：预览内容为主，文件名/类型等属性
  // 信息由右侧属性面板承担（不再重复占空间）；「未配准，不加入地图」警告照旧；
  // HZ28-6-1 XML 额外写「不对应 A1–A20」；无内嵌预览时留「用系统程序打开」
  // 兜底出口。----
  {
    QString auxName;
    for (const EntityAssetLink &l : links)
      if (l.entityType == QLatin1String("auxiliary") && !l.entityId.isEmpty())
      {
        auxName = cat->entityById(l.entityId).name;
        break;
      }
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host));
    // 参考资料/ 下 HZ28-6-1 的 XML：不按内容挂井、不并进 A1–A20（§3 固定规则）。
    if (asset.displayName.contains(QStringLiteral("HZ28-6-1")) ||
        auxName.contains(QStringLiteral("HZ28-6-1")))
      lay->addWidget(warnLabel(tr("不对应 A1–A20"), host));

    if (abs.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
    {
      return buildAuxiliaryXmlContent(abs, assetId, host, lay, sourceVersion.managed ? QString() : sourceVersion.sha256);
    }
    // P2 D2.12 未知类型：统一「不支持预览」态 + 可支持类型清单（不再留白）。
    static const QStringList kKnownTypes = {
        QStringLiteral("well_log"),      QStringLiteral("well_head"),
        QStringLiteral("well_stratification"), QStringLiteral("time_depth"),
        QStringLiteral("horizon"),       QStringLiteral("seismic"),
        QStringLiteral("image_reference"), QStringLiteral("document"),
        QStringLiteral("outsource_workbook"),
        QStringLiteral("geojson"),       QStringLiteral("boundary"),
        QStringLiteral("seismic_prediction"), QStringLiteral("wells_prediction"),
        QStringLiteral("composed_facies"), QStringLiteral("facies_polygons"),
        QStringLiteral("edited_facies"), QStringLiteral("single_factor_raster"),
        QStringLiteral("contour_lines"), QStringLiteral("facies_fusion_raster")};
    if (!kKnownTypes.contains(asset.type))
      lay->addWidget(PreviewMapStates::buildUnsupportedPage(asset.type, host), 1);
    else
      lay->addStretch(1);
    lay->addWidget(makeOpenExternalRow(abs, host), 0, Qt::AlignLeft);
    return host;
  }
}
