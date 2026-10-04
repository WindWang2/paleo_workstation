// 层：视图
#pragma once
#include <QCheckBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "../../workflow/assetops.h" // PendingProposal（数据面；扫描/应用在壳）

namespace paleo::dataops
{

// ui/pages/dataops/pendinglinkdialog — 未决链接批量归位对话框（方向 30）。
// 纯渲染 + 意图出口：可归位提案由壳用 workflow/assetops 扫出灌入；应用经
// applyRequested 信号出壳（壳走 applyPendingResolutions，单事务）。零匹配/
// 多候选的未决不进表——那些没有「恰好一口井」可归（与导入同一判据，不猜），
// 由底部说明句如实告知剩余未决数。
class PendingLinkDialog : public QDialog
{
  Q_OBJECT
  public:
    explicit PendingLinkDialog(QWidget *parent = nullptr)
      : QDialog(parent)
    {
      setObjectName(QStringLiteral("pendingLinkDialog"));
      setWindowTitle(tr("未决链接批量归位"));
      setModal(true);
      resize(720, 420);
      auto *lay = new QVBoxLayout(this);
      m_note = new QLabel(this);
      m_note->setObjectName(QStringLiteral("pendingLinkNote"));
      m_note->setWordWrap(true);
      lay->addWidget(m_note);
      m_table = new QTableWidget(0, 4, this);
      m_table->setObjectName(QStringLiteral("pendingLinkTable"));
      m_table->setHorizontalHeaderLabels({tr("归位"), tr("资产"), tr("目标井"), tr("判定名")});
      m_table->verticalHeader()->setVisible(false);
      m_table->horizontalHeader()->setStretchLastSection(true);
      lay->addWidget(m_table, 1);
      auto *row = new QWidget(this);
      auto *rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      m_applyBtn = new QPushButton(tr("归位选中项"), row);
      m_applyBtn->setObjectName(QStringLiteral("pendingApplyButton"));
      m_closeBtn = new QPushButton(tr("关闭"), row);
      rl->addWidget(m_applyBtn);
      rl->addStretch(1);
      rl->addWidget(m_closeBtn);
      lay->addWidget(row);
      connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
      connect(m_applyBtn, &QPushButton::clicked, this, [this] {
        QVector<int> idx;
        for (int i = 0; i < m_checks.size(); ++i)
          if (m_checks.at(i) && m_checks.at(i)->isChecked())
            idx << m_proposals.at(i).linkIndex;
        emit applyRequested(idx);
      });
    }

    // remainingUnresolved = 全部未决数（含无法自动归位的）——底部句如实分摊。
    void setProposals(const QVector<paleo::assetops::PendingProposal> &proposals,
                      int remainingUnresolved)
    {
      m_proposals = proposals;
      m_table->setRowCount(0);
      m_checks.clear();
      for (const paleo::assetops::PendingProposal &p : proposals)
      {
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        auto *cb = new QCheckBox(m_table);
        cb->setChecked(true);
        m_checks.append(cb);
        m_table->setCellWidget(r, 0, cb);
        const auto mk = [](const QString &t) {
          auto *it = new QTableWidgetItem(t);
          it->setFlags(it->flags() & ~Qt::ItemIsEditable);
          return it;
        };
        m_table->setItem(r, 1, mk(p.assetName));
        m_table->setItem(r, 2, mk(QObject::tr("井 %1").arg(p.wellName)));
        m_table->setItem(r, 3, mk(p.sourceName));
      }
      m_note->setText(
          proposals.isEmpty()
              ? tr("当前没有可自动归位的未决链接（%1 条未决均无「恰好一口井」的"
                   "匹配——多候选或零匹配不猜，请走单资产「挂接到实体」）。")
                    .arg(remainingUnresolved)
              : tr("%1 条未决链接中，%2 条可按文件名/备注恰好命中一口井自动归位；"
                   "其余 %3 条无唯一匹配，保持未决。")
                    .arg(remainingUnresolved)
                    .arg(proposals.size())
                    .arg(remainingUnresolved - proposals.size()));
      m_applyBtn->setEnabled(!proposals.isEmpty());
    }

    // 应用结果回执（壳调）：清掉已归位的勾选行态，显示结果句。
    void setApplied(int applied, int requested)
    {
      m_note->setText(requested == applied
                          ? tr("已归位 %1 条（单事务落盘）。").arg(applied)
                          : tr("请求 %1 条，实际归位 %2 条（执行时按 catalog 当前"
                               "事实重判——部分项已不满足判据或落位被占）。")
                                .arg(requested)
                                .arg(applied));
    }

  signals:
    void applyRequested(const QVector<int> &linkIndexes);

  private:
    QVector<paleo::assetops::PendingProposal> m_proposals;
    QVector<QCheckBox *> m_checks;
    QLabel *m_note = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_applyBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
};

} // namespace paleo::dataops
