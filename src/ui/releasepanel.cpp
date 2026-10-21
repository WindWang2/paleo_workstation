// 层：视图
#include "releasepanel.h"

#include "../metadata/releasestore.h"
#include "paleotheme.h"

#include <QComboBox>
#include <QFileInfo>
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
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);

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
  nameEdit->setAccessibleName(tr("发布名称"));
  auto *noteEdit = new QLineEdit(this);
  noteEdit->setObjectName(QStringLiteral("releaseNoteEdit"));
  noteEdit->setPlaceholderText(tr("备注（可选）"));
  noteEdit->setAccessibleName(tr("发布备注"));
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
  comboA->setAccessibleName(tr("对比基准版本"));
  auto *comboB = new QComboBox(this);
  comboB->setObjectName(QStringLiteral("diffB"));
  comboB->setAccessibleName(tr("对比目标版本"));
  auto *diffBtn = new QPushButton(tr("对比"), this);
  diffBtn->setObjectName(QStringLiteral("diffButton"));
  diffRow->addWidget(comboA, 1);
  diffRow->addWidget(new QLabel(QStringLiteral("→"), this));
  diffRow->addWidget(comboB, 1);
  diffRow->addWidget(diffBtn);
  lay->addLayout(diffRow);

  auto *diffOut = new QListWidget(this);
  diffOut->setObjectName(QStringLiteral("diffOutput"));
  diffOut->setAccessibleName(tr("版本差异结果"));
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
    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty())
    {
      emit statusMessage(tr("发布名称不能为空"));
      return;
    }
    // §6 单写实例降级：工程被另一实例持锁时发布页不得写 project.sqlite
    // （readOnlyProvider 由工程存储的只读态驱动）。过了这道闸才具名落库，
    // 刷新/对比路径的 store 也按同一只读态打开（见 refresh/diff）。
    if (projectReadOnly())
    {
      emit statusMessage(tr("工程被另一个实例锁定——只读，无法创建发布"));
      return;
    }
    ReleaseStore store(path);
    store.setReadOnly(false); // 已过只读闸：显式可写
    QString err;
    if (!store.open(&err))
    {
      emit statusMessage(tr("发布库打开失败：%1").arg(err));
      return;
    }
    const QString id = store.createRelease(name,
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
    if (idA.isEmpty() || idB.isEmpty())
    {
      emit statusMessage(tr("先在两侧各选一个发布版本"));
      return;
    }
    if (idA == idB)
    {
      emit statusMessage(tr("两侧是同一个版本——选择不同的版本进行对比"));
      return;
    }
    ReleaseStore store(m_dbPath());
    store.setReadOnly(projectReadOnly()); // 只读实例不建表、不写（§6）
    QString openErr;
    if (!store.open(&openErr))
    {
      emit statusMessage(tr("发布库打开失败：%1").arg(openErr));
      return;
    }
    QStringList added, removed, changed;
    QString diffErr;
    if (!store.diff(idA, idB, &added, &removed, &changed, &diffErr))
    {
      // 失败要可行动：缺哪个版本/库为什么打不开说清楚，不笼统「对比失败」。
      emit statusMessage(diffErr.isEmpty()
                             ? tr("版本对比失败")
                             : tr("版本对比失败：%1").arg(diffErr));
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
                                std::function<QVector<LayerDeclaration>()> manifestProvider,
                                std::function<bool()> readOnlyProvider)
{
  m_dbPath = std::move(dbPath);
  m_manifestProvider = std::move(manifestProvider);
  m_readOnlyProvider = std::move(readOnlyProvider);
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

  // 单写实例降级（§6）：只读实例禁用创建入口（读面/对比照常）。
  if (auto *createBtn = findChild<QPushButton *>(QStringLiteral("createReleaseButton")))
    createBtn->setEnabled(!projectReadOnly());

  // 空态文案：不留白板（无工程 / 库打不开 / 还没有发布，三种含义分清）。
  const auto showEmptyGuidance = [list](const QString &text) {
    auto *it = new QTreeWidgetItem(list, {text}); // 构造即挂树
    it->setFlags(Qt::NoItemFlags);
    PaleoTheme::setItemTextColor(it, 0, PaleoTheme::ItemTextColor::Muted);
    it->setFirstColumnSpanned(true);
  };

  if (!m_dbPath || m_dbPath().isEmpty())
  {
    showEmptyGuidance(tr("打开工程后可创建发布快照"));
    return;
  }

  // 只读实例：发布库走只读连接（不建表、不写——§6）。可写时才建 schema。
  const bool readOnly = projectReadOnly();
  // 只读 + 库文件还没建（新工程/从未发布）：不得建库（#80），按空态处理，
  // 不报「发布库打开失败」。
  if (readOnly && !QFileInfo::exists(m_dbPath()))
  {
    showEmptyGuidance(tr("还没有发布 — 工程被另一个实例锁定（只读），不能创建发布"));
    return;
  }
  ReleaseStore store(m_dbPath());
  store.setReadOnly(readOnly);
  QString openErr;
  if (!store.open(&openErr))
  {
    emit statusMessage(tr("发布库打开失败：%1").arg(openErr));
    showEmptyGuidance(tr("发布库暂时不可用"));
    return;
  }
  for (const ReleaseInfo &r : store.releases())
  {
    auto *item = new QTreeWidgetItem(
        list, {r.id, r.name, r.createdUtc,
               r.manifestCorrupt ? tr("损坏") : QString::number(r.layerCount)});
    list->addTopLevelItem(item);
    const QString label = QStringLiteral("%1 %2").arg(r.id, r.name);
    comboA->addItem(label, r.id);
    comboB->addItem(label, r.id);
  }
  if (list->topLevelItemCount() == 0)
  {
    showEmptyGuidance(readOnly ? tr("还没有发布 — 工程被另一个实例锁定（只读），不能创建发布")
                               : tr("还没有发布 — 在下方填名称后点「创建发布」"));
  }
  if (comboB->count() > 1)
    comboB->setCurrentIndex(comboB->count() - 1); // newest as the default "to"
}
