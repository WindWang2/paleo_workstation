// 层：视图
#include "wellcompositepanel.h"
#include "curveconfigdialog.h"
#include "trackops.h"
#include "hiddentrackbar.h"
#include "derivedsink.h"
#include "editsession.h"
#include "../paleotheme.h"
#include "../dialogs/coreimageviewer.h"
#include "../shortcuts/shortcutcatalog.h"
#include "../help/whatsthiscatalog.h"
#include "../notifications/paleonotify.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QShortcut>
#include <utility>

namespace WellComposite
{
namespace { WellCompositePanel::FaciesWorkflowFactory s_faciesFactory; }
void WellCompositePanel::setFaciesWorkflowFactory(FaciesWorkflowFactory factory) {
  s_faciesFactory = std::move(factory);
}

WellCompositePanel::WellCompositePanel(QWidget *parent)
  : QWidget(parent)
{
  setupUi();
  connect(m_canvas, &WellCompositeCanvas::imagePreviewRequested, this,
          [this](const ImageDepthItem &item) { PaleoCoreImageViewer::open(item, m_wellName, this); });

  // D2.7 Ctrl+G 跳深度（方向63：键序登记在 shortcuts/shortcutcatalog）
  auto *shortcut = paleo::shortcuts::bindShortcut(QStringLiteral("wellcomposite.gotoDepth"), this);
  connect(shortcut, &QShortcut::activated, this, &WellCompositePanel::openGotoDepthDialog);

  // D1：登记进活跃面板表并挂接默认 sink（壳 attachWorkflows 先于预览页建立；
  // sink 迟装时由 setDefault 补挂）。
  WellCompositeDerivedSink::registerPanel(this);
  if (s_faciesFactory) bindFaciesWorkflow(s_faciesFactory(this));

  // 方向63：懒建面板自行按清单回填「这是什么？」说明。
  paleo::help::applyWhatsThis(this);
}

WellCompositePanel::~WellCompositePanel()
{
  saveSessionState();
  // 子控件由 ~QWidget 在本类析构完成之后才删除；期间画布刷新刻度、组合框失焦
  // （editingFinished → setScaleRatio → scaleRatioChanged）等仍会发信号，进入捕获
  // this 的 lambda 访问已析构成员（UBSan：member access … not WellCompositePanel）。
  // 先断开所有子对象 → this 的连接；this 作为 context 的自动断开要到 ~QObject 才发生。
  const auto kids = findChildren<QObject *>();
  for (QObject *child : kids)
    QObject::disconnect(child, nullptr, this, nullptr);
}

void WellCompositePanel::openCurveConfigDialog()
{
  CurveConfigDialog dlg(m_canvas, this);
  dlg.setCurvePool(TrackOps::combinedCurvePool(m_data.continuousCurves, m_data.discreteCurves));
  dlg.exec();
  m_hiddenBar->setTracks(currentSpecs());
  onTrackOrderOrWidthChanged();
}

void WellCompositePanel::setWellName(const QString &name, bool reference)
{
  m_wellName = name;
  m_referenceWell = reference;

  // 测区井徽章 = 中性徽章（surface-alt-raised 底 + text 字）；
  // 参考井徽章 = warning 胶囊语义（辅助资料、待区分）。
  const auto neutralBadgeStyle = [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "background: %1; color: %2; font-weight: bold; border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.body}pt;"))
        .arg(t.surfaceAltRaised.name(), t.text.name());
  };
  const auto referenceBadgeStyle = [] {
    return PaleoTheme::capsuleStyleSheet(PaleoTheme::CapsuleKind::Warning) +
           PaleoTheme::metricStyleSheet(QStringLiteral(" font-weight: bold; padding: {spacing.xs}px {spacing.sm}px; font-size: {typography.body}pt;"));
  };

  if (name.isEmpty())
  {
    m_lblWellName->setText(tr("井号: —"));
    PaleoTheme::applyThemedStyleSheet(m_lblWellName, neutralBadgeStyle);
    m_lblWellName->setToolTip(QString());
  }
  else if (reference)
  {
    m_lblWellName->setText(tr("参考井: %1").arg(name));
    PaleoTheme::applyThemedStyleSheet(m_lblWellName, referenceBadgeStyle);
    m_lblWellName->setToolTip(tr("辅助资料中的参考井，不属于本测区井序列"));
  }
  else
  {
    m_lblWellName->setText(tr("井号: %1").arg(name));
    PaleoTheme::applyThemedStyleSheet(m_lblWellName, neutralBadgeStyle);
    m_lblWellName->setToolTip(QString());
  }
}

void WellCompositePanel::setProjectName(const QString &project)
{
  m_projectName = project.trimmed().isEmpty() ? QStringLiteral("default") : project.trimmed();
}

void WellCompositePanel::setSourceDataPath(const QString &path)
{
  m_sourceDataPath = path;
  if (path.isEmpty())
  {
    m_store.reset();
    return;
  }
  m_store = std::make_unique<WellCompositeStore>(path);
  if (!m_store->load())
  {
    // sidecar 损坏/不可读：先备份为 .corrupt 再如实报告。旧实现丢弃 load()
    // 返回值直接按空库装配，下一次保存会用空库整体覆盖原文件——钉注/书签/
    // 地层指派/曲线覆盖层永久丢失且无痕迹。
    const QString sidecar = m_store->sidecarPath();
    const QString backup = sidecar + QStringLiteral(".corrupt");
    QString removeErr;
    if (QFile::exists(backup) && !QFile::remove(backup))
      removeErr = QFile(backup).errorString();
    if (removeErr.isEmpty() && !QFile::copy(sidecar, backup))
      removeErr = QFile(backup).errorString();
    if (removeErr.isEmpty())
    {
      PaleoNotify::warning(
          this, tr("井组合附属数据损坏"),
          tr("无法读取 %1，已备份为 %2。钉注/书签/地层指派/曲线覆盖层未加载，"
             "重新保存将写新的附属文件。")
              .arg(QDir::toNativeSeparators(sidecar), QDir::toNativeSeparators(backup)));
    }
    else
    {
      // 备份不了就整体不作数：保留原文件不动，也不进入空库装配（否则保存即覆盖）
      PaleoNotify::warning(this, tr("井组合附属数据损坏"),
                           tr("无法读取 %1，且无法备份（%2）。已停止加载该井的附属数据。")
                               .arg(QDir::toNativeSeparators(sidecar), removeErr));
      m_store.reset();
      return;
    }
  }
  const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
  if (m_editSession)
    m_editSession->setSourceMtime(mtime);
}

} // namespace WellComposite
