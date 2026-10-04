// 层：视图
#pragma once
#include <QDialog>

#include "../../services/cataloghealth.h"

class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;

// ui/dialogs/cataloghealthdialog — 资产体检对话框（方向 30：健康度仪表盘）。
// 纯渲染 + 意图出口：报告由壳（DataListPanel）用 services/cataloghealth 装好
// 灌进来；重新体检/SHA 复验/双击跳转全部经信号出壳——本对话框不碰 catalog、
// 不起线程。分类列表左、问题明细右；某类为零也留行（计 0），布局稳定。
class CatalogHealthDialog : public QDialog
{
  Q_OBJECT
  public:
    explicit CatalogHealthDialog(QWidget *parent = nullptr);

    // 灌入报告（快速面 + 可选已完成的 SHA 复验问题段）；回收站积压是 UI
    // sidecar 侧的数据，由壳算好随行传入（count<0 = 不显示该行）。
    void setReport(const paleo::health::HealthReport &report, int recycleCount,
                   qint64 recycleBytes);
    // SHA 复验进行中/完成的态标（「校验中…」/「未扫完」如实显示）。
    void setShaState(const QString &text);
    // 校验按钮双态切换（true = 运行中，点击将发 cancelVerifyRequested）。
    void setVerifyRunning(bool running);

  signals:
    void refreshRequested();
    void verifyShaRequested();
    void cancelVerifyRequested();
    void jumpToAsset(const QString &assetId);
    void jumpToEntity(const QString &entityId);

  private:
    void rebuildCategoryList();
    void fillIssueTable();

    paleo::health::HealthReport m_report;
    int m_recycleCount = -1;
    qint64 m_recycleBytes = 0;
    int m_selectedKind = -1; // 类别序（与 kCategoryRows 对应）

    QListWidget *m_categories = nullptr;
    QTableWidget *m_issues = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_shaState = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_verifyBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
};
