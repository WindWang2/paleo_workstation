// 层：视图
#include "wellcompositepanel.h"
#include "curveconfigdialog.h"
#include "derivedsink.h"
#include "editsession.h"
#include "../paleotheme.h"
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

  // D2.7 Ctrl+G 跳深度
  auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+G")), this);
  connect(shortcut, &QShortcut::activated, this, &WellCompositePanel::openGotoDepthDialog);

  // D1：登记进活跃面板表并挂接默认 sink（壳 attachWorkflows 先于预览页建立；
  // sink 迟装时由 setDefault 补挂）。
  WellCompositeDerivedSink::registerPanel(this);
  if (s_faciesFactory) bindFaciesWorkflow(s_faciesFactory(this));
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
  dlg.exec();
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
  m_store->load();
  const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
  if (m_editSession)
    m_editSession->setSourceMtime(mtime);
}

} // namespace WellComposite
