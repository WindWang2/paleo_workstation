// 层：视图
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "../services/agentnavigationaccess.h" // 访问契约（数据层；qgis/workflow/ui 三方共用）

class DataPreviewTabs;
class SelectionContext;
class WellMapLink;

// ui/agentnavigationadapter.h — I4：语义导航的视图侧适配器。
//
// 实现 workflow/agentnavigation.h 的 SelectionAccess + PreviewAccess：
//   · 选择：SelectionContext 的可信井来源广播（origin=agent_navigation）+
//     真实回读（业务选择 + WellMapLink 井层原生选中集——复用既有索引与
//     QSignalBlocker 防回声，不为新工具扫描井层）；
//   · 预览：DataPreviewTabs 的 typed 接缝（固定版本/严格线号/完成回执），
//     per-request 只读 SHA 策略随每次打开传入。
// 只接有限语义接口，不做对象名驱动的通用控件自动化。
class AgentNavigationAdapter : public QObject,
                               public paleo::agent::navigation::SelectionAccess,
                               public paleo::agent::navigation::PreviewAccess
{
  Q_OBJECT
public:
  AgentNavigationAdapter(DataPreviewTabs *preview, SelectionContext *selection,
                         WellMapLink *wellLink, QObject *parent = nullptr);

  // ---- SelectionAccess ----
  QString unavailabilityReason() const override;
  QJsonObject snapshot() const override;
  QJsonObject select(const QStringList &wellIds) override;
  QJsonObject clear() override;

  // ---- PreviewAccess ----
  QJsonObject previewSnapshot() const override;
  QJsonObject openWellView(const QString &versionId, const QString &wellId,
                           const paleo::agent::navigation::PreviewAccess::Completion &done) override;
  QJsonObject openSeismicLine(const QString &versionId, const QString &axis, int lineNumber,
                              const paleo::agent::navigation::PreviewAccess::Completion &done) override;

private:
  QString selectionToken(const QStringList &ids, const QString &origin) const;

  QPointer<DataPreviewTabs> m_preview;
  QPointer<SelectionContext> m_selection;
  QPointer<WellMapLink> m_wellLink;
};
