// 层：视图
#include "releasepanel.h"

#include "../metadata/releasestore.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

// The header fixes the member set (two provider std::functions), so widget
// pointers are resolved via objectName lookups — same discipline as
// pagepanels.cpp / paleomainwindow.cpp.

ReleasePanel::ReleasePanel(QWidget *parent)
  : QWidget(parent)
{
  setObjectName(QStringLiteral("releasePanel"));
  // T32 a11y：屏幕阅读器面（面板/列表/动作各自报名）。
  setAccessibleName(tr("发布管理面板"));
  setAccessibleDescription(
      tr("管理地图发布：创建发布快照、比较两个版本的图层差异"));
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(8, 8, 8, 8);

  auto *title = new QLabel(tr("发布管理"), this);
  QFont f = title->font();
  f.setBold(true);
  title->setFont(f);
  lay->addWidget(title);

  // Release list: id / name / created / layers.
  auto *list = new QTreeWidget(this);
  list->setObjectName(QStringLiteral("releaseList"));
  list->setAccessibleName(tr("发布列表"));
  list->setHeaderLabels({tr("ID"), tr("名称"),
                         tr("时间"), tr("图层数")});
  list->setRootIsDecorated(false);
  lay->addWidget(list, 1);

  // Create form: name + note inline (no modal dialog — offscreen-safe).
  auto *form = new QFormLayout;
  auto *nameEdit = new QLineEdit(this);
  nameEdit->setObjectName(QStringLiteral("releaseNameEdit"));
  nameEdit->setPlaceholderText(tr("v1.0 / 阶段名…"));
  auto *noteEdit = new QLineEdit(this);
  noteEdit->setObjectName(QStringLiteral("releaseNoteEdit"));
  noteEdit->setPlaceholderText(tr("备注（可选）"));
  form->addRow(tr("名称"), nameEdit);
  form->addRow(tr("备注"), noteEdit);
  lay->addLayout(form);

  auto *createBtn = new QPushButton(tr("创建发布（快照当前清单）"), this);
  createBtn->setObjectName(QStringLiteral("createReleaseButton"));
  createBtn->setAccessibleName(tr("创建发布"));
  lay->addWidget(createBtn);

  // Diff section: pick A/B, show added/removed/changed.
  auto *diffLabel = new QLabel(tr("版本差异"), this);
  diffLabel->setFont(f);
  lay->addWidget(diffLabel);

  auto *diffRow = new QHBoxLayout;
  auto *comboA = new QComboBox(this);
  comboA->setObjectName(QStringLiteral("diffA"));
  auto *comboB = new QComboBox(this);
  comboB->setObjectName(QStringLiteral("diffB"));
  auto *diffBtn = new QPushButton(tr("对比"), this);
  diffBtn->setObjectName(QStringLiteral("diffButton"));
  diffRow->addWidget(comboA, 1);
  diffRow->addWidget(new QLabel(QStringLiteral("→"), this));
  diffRow->addWidget(comboB, 1);
  diffRow->addWidget(diffBtn);
  lay->addLayout(diffRow);

  auto *diffOut = new QListWidget(this);
  diffOut->setObjectName(QStringLiteral("diffOutput"));
  lay->addWidget(diffOut, 1);

  connect(createBtn, &QPushButton::clicked, this, [this, nameEdit, noteEdit] {
    if (!m_dbPath || !m_manifestProvider)
      return;
    const QString path = m_dbPath();
    if (path.isEmpty())
    {
      emit statusMessage(tr("无打开工程 — 无法创建发布"));
      return;
    }
    ReleaseStore store(path);
    QString err;
    if (!store.open(&err))
    {
      emit statusMessage(tr("发布库打开失败：%1").arg(err));
      return;
    }
    const QString id = store.createRelease(nameEdit->text().trimmed(),
                                           noteEdit->text().trimmed(),
                                           m_manifestProvider(), &err);
    if (id.isEmpty())
    {
      emit statusMessage(tr("创建发布失败：%1").arg(err));
      return;
    }
    nameEdit->clear();
    noteEdit->clear();
    emit releaseCreated(id);
    emit statusMessage(tr("已创建发布 %1").arg(id));
    refresh();
  });

  connect(diffBtn, &QPushButton::clicked, this, [this, diffOut, comboA, comboB] {
    diffOut->clear();
    if (!m_dbPath)
      return;
    const QString idA = comboA->currentData().toString();
    const QString idB = comboB->currentData().toString();
    if (idA.isEmpty() || idB.isEmpty() || idA == idB)
      return;
    ReleaseStore store(m_dbPath());
    if (!store.open())
      return;
    QStringList added, removed, changed;
    if (!store.diff(idA, idB, &added, &removed, &changed))
    {
      emit statusMessage(tr("版本对比失败"));
      return;
    }
    for (const QString &s : added)
      diffOut->addItem(QStringLiteral("+ %1").arg(s));
    for (const QString &s : removed)
      diffOut->addItem(QStringLiteral("- %1").arg(s));
    for (const QString &s : changed)
      diffOut->addItem(QStringLiteral("~ %1").arg(s));
    if (added.isEmpty() && removed.isEmpty() && changed.isEmpty())
      diffOut->addItem(tr("（两版本声明集一致）"));
  });
}

void ReleasePanel::setProviders(std::function<QString()> dbPath,
                                std::function<QVector<LayerDeclaration>()> manifestProvider)
{
  m_dbPath = std::move(dbPath);
  m_manifestProvider = std::move(manifestProvider);
  refresh();
}

void ReleasePanel::refresh()
{
  auto *list = findChild<QTreeWidget *>(QStringLiteral("releaseList"));
  auto *comboA = findChild<QComboBox *>(QStringLiteral("diffA"));
  auto *comboB = findChild<QComboBox *>(QStringLiteral("diffB"));
  if (!list || !comboA || !comboB)
    return;

  list->clear();
  comboA->clear();
  comboB->clear();

  if (!m_dbPath || m_dbPath().isEmpty())
    return;

  ReleaseStore store(m_dbPath());
  if (!store.open())
    return;
  for (const ReleaseInfo &r : store.releases())
  {
    auto *item = new QTreeWidgetItem(list, {r.id, r.name, r.createdUtc,
                                            QString::number(r.layerCount)});
    list->addTopLevelItem(item);
    const QString label = QStringLiteral("%1 %2").arg(r.id, r.name);
    comboA->addItem(label, r.id);
    comboB->addItem(label, r.id);
  }
  if (comboB->count() > 1)
    comboB->setCurrentIndex(comboB->count() - 1); // newest as the default "to"
}
