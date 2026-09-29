// 层：视图
// ui/pages/dataopspanelextra — D4 实体/属性面板增强件。
//   · VersionTimeline：资产版本时间线（版本卡：RAW/DERIVED、时间、大小、
//     SHA 短码、来源摘要）（D4.3）+ 版本间 diff 入口（D4.4 清单级）
//   · VersionDiffDialog：两版本清单级 diff（大小/行数/字段差异）
//   · TopologyGraph：实体↔资产节点边图（QGraphicsView），点击定位（D4.5）
//   · EntityCrudBar：新建/重命名/删除（含资产处置选择）（D4.6）
//   · RoleEditDialog：挂接角色编辑（D4.7）
//   · OperationsHistoryDialog：最近操作历史小窗（D4.10，会话内）
#pragma once

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QGraphicsEllipseItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "../paleotheme.h"
#include "dataops/dataopsmodel.h"

namespace paleo::dataops
{

// ---- D4.3 版本卡 + 时间线 -------------------------------------------------------
struct VersionCard
{
  QString versionId;
  int versionNumber = 0;
  QString stage;      // RAW | DERIVED | INTERMEDIATE | OUTPUT
  bool managed = true;
  QString fileName;
  QString path;
  QString shaShort;   // SHA-256 前 8 位
  qint64 sizeBytes = 0;
  QDateTime created;  // 文件 mtime
  QString sourceSummary; // sourceUri 摘要（目录名 + 文件名）

  static VersionCard fromVersion(const CatalogVersion &v)
  {
    VersionCard c;
    c.versionId = v.id;
    c.versionNumber = v.versionNumber;
    c.stage = v.stage;
    c.managed = v.managed;
    c.fileName = v.fileName;
    c.path = v.path;
    c.shaShort = v.sha256.left(8);
    const QFileInfo fi(v.managed ? v.path : v.path);
    if (!v.path.isEmpty() && fi.exists())
    {
      c.sizeBytes = fi.size();
      c.created = fi.lastModified();
    }
    if (!v.sourceUri.isEmpty())
    {
      const QFileInfo si(v.sourceUri);
      c.sourceSummary = si.fileName();
    }
    return c;
  }
  QString stageText() const
  {
    if (stage == QLatin1String("DERIVED")) return QObject::tr("派生");
    if (stage == QLatin1String("INTERMEDIATE")) return QObject::tr("中间");
    if (stage == QLatin1String("OUTPUT")) return QObject::tr("输出");
    return QObject::tr("原始");
  }
};

// 时间线：纵向卡片列表（新→旧），每卡 [v# 阶段] 文件名 · 大小 · SHA 短码 ·
// 来源；可勾选两张做 diff（D4.4 入口）。
class VersionTimeline : public QWidget
{
  Q_OBJECT
public:
  explicit VersionTimeline(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setObjectName(QStringLiteral("versionTimeline"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    auto *head = new QWidget(this);
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(0, 0, 0, 0);
    auto *cap = new QLabel(tr("版本时间线"), head);
    PaleoTheme::applyThemedStyleSheet(cap,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    hl->addWidget(cap);
    hl->addStretch(1);
    m_diffBtn = new QPushButton(tr("比较所选两版"), head);
    m_diffBtn->setObjectName(QStringLiteral("versionDiffButton"));
    m_diffBtn->setEnabled(false);
    hl->addWidget(m_diffBtn);
    lay->addWidget(head);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    auto *host = new QWidget(m_scroll);
    m_cardsLay = new QVBoxLayout(host);
    m_cardsLay->setContentsMargins(0, 0, 0, 0);
    m_cardsLay->setSpacing(4);
    m_cardsLay->addStretch(1);
    m_scroll->setWidget(host);
    lay->addWidget(m_scroll, 1);
    connect(m_diffBtn, &QPushButton::clicked, this, [this] {
      if (m_checked.size() == 2)
        emit diffRequested(m_checked.at(0), m_checked.at(1));
    });
    m_empty = new QLabel(tr("该资产暂无多版本记录"), this);
    m_empty->setObjectName(QStringLiteral("versionTimelineEmpty"));
    PaleoTheme::applyThemedStyleSheet(m_empty,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(m_empty);
  }

  void loadVersions(const QVector<CatalogVersion> &versions)
  {
    // 清旧卡。
    while (m_cardsLay->count() > 1)
    {
      QLayoutItem *it = m_cardsLay->takeAt(0);
      if (it->widget())
        it->widget()->deleteLater();
      delete it;
    }
    m_cards.clear();
    m_checked.clear();
    m_diffBtn->setEnabled(false);
    // 新→旧（versionNumber 降序）。
    QVector<CatalogVersion> sorted = versions;
    std::sort(sorted.begin(), sorted.end(),
              [](const CatalogVersion &a, const CatalogVersion &b) {
                return a.versionNumber > b.versionNumber;
              });
    m_empty->setVisible(sorted.size() <= 1);
    m_scroll->setVisible(sorted.size() > 1);
    for (const CatalogVersion &v : sorted)
    {
      const VersionCard card = VersionCard::fromVersion(v);
      auto *w = new QWidget(this);
      w->setObjectName(QStringLiteral("versionCard"));
      // 卡片：border + surface 底（DESIGN.md 面板直角/卡片 8px 圆角）。
      PaleoTheme::applyThemedStyleSheet(w, [] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("QLabel { color: %2; } QWidget#versionCard { background: %1;"
                              " border: 1px solid %3; border-radius: 8px; }")
            .arg(t.surface.name().toUpper(), t.text.name().toUpper(),
                 t.border.name().toUpper());
      });
      auto *l = new QVBoxLayout(w);
      l->setContentsMargins(8, 6, 8, 6);
      l->setSpacing(2);
      auto *row1 = new QWidget(w);
      auto *r1 = new QHBoxLayout(row1);
      r1->setContentsMargins(0, 0, 0, 0);
      auto *tag = new QLabel(QStringLiteral("v%1 · %2").arg(card.versionNumber).arg(card.stageText()), row1);
      QFont tf = tag->font();
      tf.setBold(true);
      tag->setFont(tf);
      // 阶段语义胶囊色：DERIVED=warning 系（派生待复核语义），RAW=success。
      const bool derived = card.stage == QLatin1String("DERIVED");
      PaleoTheme::applyThemedStyleSheet(tag, [derived] {
        const auto &t = PaleoTheme::tokens();
        return QStringLiteral("color: %1; font-weight: 600;")
            .arg((derived ? t.warning : t.success).name().toUpper());
      });
      r1->addWidget(tag);
      r1->addStretch(1);
      auto *pick = new QToolButton(row1);
      pick->setText(tr("选为比较"));
      pick->setCheckable(true);
      pick->setObjectName(QStringLiteral("versionPickButton"));
      const QString vid = v.id;
      connect(pick, &QToolButton::toggled, this, [this, vid, pick](bool on) {
        if (on)
        {
          m_checked.append(vid);
          // 最多两张：超额自动取消最早一张。
          if (m_checked.size() > 2)
          {
            const QString drop = m_checked.takeFirst();
            for (QWidget *c : m_cards)
              if (auto *b = c->findChild<QToolButton *>(QStringLiteral("versionPickButton")))
                if (b->isChecked() && b != pick && c->property("versionId").toString() == drop)
                  b->setChecked(false);
          }
        }
        else
          m_checked.removeAll(vid);
        m_diffBtn->setEnabled(m_checked.size() == 2);
      });
      w->setProperty("versionId", vid);
      r1->addWidget(pick);
      l->addWidget(row1);
      auto *name = new QLabel(card.fileName.isEmpty() ? card.path : card.fileName, w);
      l->addWidget(name);
      auto *meta = new QLabel(w);
      meta->setFont(PaleoTheme::monoFont()); // SHA/大小 = mono 数字面
      QStringList bits;
      if (card.sizeBytes > 0)
        bits << tr("%1 KB").arg(card.sizeBytes / 1024);
      if (!card.shaShort.isEmpty())
        bits << QStringLiteral("SHA ") + card.shaShort;
      if (card.created.isValid())
        bits << card.created.toString(QStringLiteral("yyyy-MM-dd"));
      meta->setText(bits.join(QStringLiteral(" · ")));
      PaleoTheme::applyThemedStyleSheet(meta, [] {
        return QStringLiteral("color: %1; font-size: 8pt;")
            .arg(PaleoTheme::tokens().textMuted.name().toUpper());
      });
      l->addWidget(meta);
      if (!card.sourceSummary.isEmpty())
      {
        auto *src = new QLabel(tr("来源：%1").arg(card.sourceSummary), w);
        src->setToolTip(card.path);
        PaleoTheme::applyThemedStyleSheet(src, [] {
          return QStringLiteral("color: %1; font-size: 8pt;")
              .arg(PaleoTheme::tokens().textMuted.name().toUpper());
        });
        l->addWidget(src);
      }
      m_cardsLay->insertWidget(m_cardsLay->count() - 1, w);
      m_cards.append(w);
    }
  }
  QStringList checkedVersionIds() const { return m_checked; }

signals:
  void diffRequested(const QString &versionIdA, const QString &versionIdB);

private:
  QScrollArea *m_scroll = nullptr;
  QVBoxLayout *m_cardsLay = nullptr;
  QLabel *m_empty = nullptr;
  QPushButton *m_diffBtn = nullptr;
  QStringList m_checked;
  QList<QWidget *> m_cards;
};

// ---- D4.4 版本间 diff（清单级）---------------------------------------------------
struct VersionDiff
{
  // 字段差异（两侧非空时给 delta；仅一侧有时给有无标记）。
  qint64 sizeDelta = 0;
  int versionA = 0, versionB = 0;
  QString stageA, stageB;
  QString shaA, shaB;
  bool sameSha = false;
  QStringList fieldNotes; // 逐字段差异行
};
inline VersionDiff diffVersions(const CatalogVersion &a, const CatalogVersion &b,
                                const QString &absA, const QString &absB)
{
  VersionDiff d;
  d.versionA = a.versionNumber;
  d.versionB = b.versionNumber;
  d.stageA = a.stage;
  d.stageB = b.stage;
  d.shaA = a.sha256;
  d.shaB = b.sha256;
  d.sameSha = !a.sha256.isEmpty() && a.sha256 == b.sha256;
  const auto statOf = [](const QString &p) -> qint64 {
    const QFileInfo fi(p);
    return fi.exists() ? fi.size() : -1;
  };
  const qint64 sa = statOf(absA), sb = statOf(absB);
  if (sa >= 0 && sb >= 0)
    d.sizeDelta = sb - sa;
  if (a.managed != b.managed)
    d.fieldNotes << QObject::tr("存储方式：%1 → %2")
                        .arg(a.managed ? QObject::tr("受管") : QObject::tr("外链"),
                             b.managed ? QObject::tr("受管") : QObject::tr("外链"));
  if (a.stage != b.stage)
    d.fieldNotes << QObject::tr("阶段：%1 → %2").arg(a.stage, b.stage);
  if (d.sameSha)
    d.fieldNotes << QObject::tr("内容相同（SHA-256 一致）");
  return d;
}

class VersionDiffDialog : public QDialog
{
  Q_OBJECT
public:
  explicit VersionDiffDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("versionDiffDialog"));
    setWindowTitle(tr("版本比较"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("versionDiffText"));
    m_text->setReadOnly(true);
    lay->addWidget(m_text, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }
  void showDiff(const VersionDiff &d)
  {
    QStringList lines;
    lines << QObject::tr("v%1（%2） ↔ v%3（%4）")
                 .arg(d.versionA)
                 .arg(d.stageA.isEmpty() ? QStringLiteral("RAW") : d.stageA)
                 .arg(d.versionB)
                 .arg(d.stageB.isEmpty() ? QStringLiteral("RAW") : d.stageB);
    if (d.sameSha)
      lines << QObject::tr("· 内容一致（SHA-256 相同）");
    else
    {
      lines << QObject::tr("· 大小差：%1 KB").arg(double(d.sizeDelta) / 1024.0, 0, 'f', 1);
      lines << QObject::tr("· SHA：%1… ↔ %2…")
                   .arg(d.shaA.left(8), d.shaB.left(8));
    }
    lines << QObject::tr("· 字段差异：");
    lines << (d.fieldNotes.isEmpty() ? QStringList{QObject::tr("  （无）")} : d.fieldNotes);
    m_text->setPlainText(lines.join(QLatin1Char('\n')));
  }

private:
  QPlainTextEdit *m_text = nullptr;
};

// ---- D4.5 关联拓扑小图 ------------------------------------------------------------
// 实体（左列）↔ 资产（右列）二部图；边 = 链接（未决虚线）。点击节点发定位。
class TopologyGraph : public QGraphicsView
{
  Q_OBJECT
public:
  explicit TopologyGraph(QWidget *parent = nullptr)
    : QGraphicsView(parent)
  {
    setObjectName(QStringLiteral("topologyGraph"));
    setAccessibleName(tr("关联拓扑图"));
    setScene(&m_scene);
    setRenderHint(QPainter::Antialiasing);
    setMinimumHeight(180);
  }
  ~TopologyGraph() override { setScene(nullptr); }

  struct Node
  {
    QString id;      // 实体 id 或资产 id
    bool isEntity = false;
    QString label;
    QGraphicsEllipseItem *item = nullptr;
  };

  void loadTopology(DataCatalog *cat, const EntityOverrideStore &overrides)
  {
    m_scene.clear();
    m_nodes.clear();
    if (!cat)
      return;
    const QVector<CatalogEntity> ents = cat->entities();
    const QVector<CatalogAsset> assets = cat->assets();
    const int n = qMax(ents.size(), assets.size());
    if (n == 0)
      return;
    const double vGap = 34.0;
    const double height = qMax(220.0, n * vGap + 40);
    int row = 0;
    for (const CatalogEntity &e : ents)
    {
      const QString name = overrides.displayName(e);
      auto *g = m_scene.addEllipse(6, 20 + row * vGap, 14, 14,
                                   QPen(PaleoTheme::tokens().primary, 2),
                                   QBrush(PaleoTheme::tokens().surface));
      auto *t = m_scene.addSimpleText(name.isEmpty() ? e.id : name);
      t->setPos(26, 16 + row * vGap);
      Node nd;
      nd.id = e.id;
      nd.isEntity = true;
      nd.label = name;
      nd.item = g;
      g->setData(0, e.id);
      g->setData(1, QStringLiteral("entity"));
      m_nodes.append(nd);
      ++row;
    }
    row = 0;
    for (const CatalogAsset &a : assets)
    {
      auto *g = m_scene.addEllipse(300, 20 + row * vGap, 14, 14,
                                   QPen(PaleoTheme::tokens().textMuted, 2),
                                   QBrush(PaleoTheme::tokens().surface));
      auto *t = m_scene.addSimpleText(a.displayName);
      t->setPos(320, 16 + row * vGap);
      Node nd;
      nd.id = a.id;
      nd.isEntity = false;
      nd.label = a.displayName;
      nd.item = g;
      g->setData(0, a.id);
      g->setData(1, QStringLiteral("asset"));
      m_nodes.append(nd);
      ++row;
    }
    // 边：链接（未决 = 虚线 warning 色）。
    for (const EntityAssetLink &l : cat->links())
    {
      const Node *en = nodeById(l.entityId, true);
      const Node *an = nodeById(l.assetId, false);
      if (!en || !an || !en->item || !an->item)
        continue;
      QPen pen(l.unresolved ? PaleoTheme::tokens().warning : PaleoTheme::tokens().textMuted,
               l.isPrimary && !l.unresolved ? 2 : 1);
      if (l.unresolved)
        pen.setStyle(Qt::DashLine);
      m_scene.addLine(en->item->rect().right(), en->item->rect().center().y(),
                      an->item->rect().left(), an->item->rect().center().y(), pen);
    }
    m_scene.setSceneRect(0, 0, 460, height);
  }

  const Node *nodeById(const QString &id, bool entity) const
  {
    for (const Node &nd : m_nodes)
      if (nd.id == id && nd.isEntity == entity)
        return &nd;
    return nullptr;
  }
  int nodeCount() const { return m_nodes.size(); }

signals:
  void nodeClicked(const QString &id, bool isEntity);

protected:
  void mousePressEvent(QMouseEvent *event) override
  {
    const QPointF p = mapToScene(event->pos());
    QGraphicsItem *it = m_scene.itemAt(p, QTransform());
    if (it && !it->data(0).toString().isEmpty())
      emit nodeClicked(it->data(0).toString(), it->data(1).toString() == QLatin1String("entity"));
    QGraphicsView::mousePressEvent(event);
  }

private:
  QGraphicsScene m_scene;
  QVector<Node> m_nodes;
};

// ---- D4.6 实体 CRUD ---------------------------------------------------------------
// 新建实体对话框：类型下拉（well/auxiliary/seismic_survey/sequence_boundary）
// + 名称（必填，重名校验由装配方做）+ 井坐标（well 类型时）。
class EntityCreateDialog : public QDialog
{
  Q_OBJECT
public:
  explicit EntityCreateDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("entityCreateDialog"));
    setWindowTitle(tr("新建实体"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    form->setSpacing(6);
    m_type = new QComboBox(this);
    m_type->setObjectName(QStringLiteral("entityTypeCombo"));
    m_type->addItem(tr("井"), QStringLiteral("well"));
    m_type->addItem(tr("辅助资料"), QStringLiteral("auxiliary"));
    m_type->addItem(tr("地震工区"), QStringLiteral("seismic_survey"));
    m_type->addItem(tr("层序界面"), QStringLiteral("sequence_boundary"));
    form->addRow(tr("类型:"), m_type);
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("entityNameEdit"));
    form->addRow(tr("名称:"), m_name);
    m_x = new QDoubleSpinBox(this);
    m_x->setObjectName(QStringLiteral("entityXSpin"));
    m_x->setRange(-1e9, 1e9);
    m_y = new QDoubleSpinBox(this);
    m_y->setObjectName(QStringLiteral("entityYSpin"));
    m_y->setRange(-1e9, 1e9);
    m_coordRow = new QWidget(this);
    auto *cl = new QHBoxLayout(m_coordRow);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(new QLabel(tr("X:"), m_coordRow));
    cl->addWidget(m_x);
    cl->addWidget(new QLabel(tr("Y:"), m_coordRow));
    cl->addWidget(m_y);
    form->addRow(tr("井口坐标:"), m_coordRow);
    lay->addLayout(form);
    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("entityCreateNote"));
    PaleoTheme::applyThemedStyleSheet(m_note,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(m_note);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
    connect(m_type, &QComboBox::currentIndexChanged, this, [this] {
      m_coordRow->setVisible(m_type->currentData().toString() == QLatin1String("well"));
    });
    m_coordRow->setVisible(true);
  }
  QString chosenType() const { return m_type->currentData().toString(); }
  QString chosenName() const { return m_name->text().trimmed(); }
  double chosenX() const { return m_x->value(); }
  double chosenY() const { return m_y->value(); }
  void setNote(const QString &n) { m_note->setText(n); }

private:
  QComboBox *m_type = nullptr;
  QLineEdit *m_name = nullptr;
  QDoubleSpinBox *m_x = nullptr, *m_y = nullptr;
  QWidget *m_coordRow = nullptr;
  QLabel *m_note = nullptr;
};

// 删除实体的资产处置选择（D4.6：删除时选择资产的处置）。
class EntityDeleteDialog : public QDialog
{
  Q_OBJECT
public:
  explicit EntityDeleteDialog(int assetCount, QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("entityDeleteDialog"));
    setWindowTitle(tr("删除实体"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    auto *head = new QLabel(tr("该实体关联 %1 个资产。删除实体（软删，可从可回收清单"
                               "恢复）时，资产如何处置？")
                                .arg(assetCount), this);
    head->setWordWrap(true);
    lay->addWidget(head);
    m_keep = new QRadioButton(tr("保留资产（解除全部关联，资产转为未挂接）"), this);
    m_soft = new QRadioButton(tr("资产一并移入可回收清单"), this);
    m_keep->setChecked(true);
    lay->addWidget(m_keep);
    lay->addWidget(m_soft);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }
  bool assetsToRecycle() const { return m_soft->isChecked(); }

private:
  QRadioButton *m_keep = nullptr;
  QRadioButton *m_soft = nullptr;
};

// 实体内联编辑对话框（D4.1/D4.2：名称 + 坐标 + 备注；写回 override store）。
class EntityEditDialog : public QDialog
{
  Q_OBJECT
public:
  explicit EntityEditDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("entityEditDialog"));
    setWindowTitle(tr("编辑实体"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("entityEditName"));
    form->addRow(tr("名称:"), m_name);
    m_x = new QDoubleSpinBox(this);
    m_x->setRange(-1e9, 1e9);
    m_y = new QDoubleSpinBox(this);
    m_y->setRange(-1e9, 1e9);
    auto *cr = new QWidget(this);
    auto *cl = new QHBoxLayout(cr);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(m_x);
    cl->addWidget(m_y);
    form->addRow(tr("坐标 X / Y:"), cr);
    m_note = new QPlainTextEdit(this);
    m_note->setObjectName(QStringLiteral("entityEditNote"));
    m_note->setMaximumHeight(72);
    form->addRow(tr("备注:"), m_note);
    lay->addLayout(form);
    m_err = new QLabel(this);
    m_err->setObjectName(QStringLiteral("entityEditError"));
    m_err->hide();
    PaleoTheme::applyThemedStyleSheet(m_err, [] {
      return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().error.name().toUpper());
    });
    lay->addWidget(m_err);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, [this] {
      // D4.1 校验：非空 + 非法字符（路径段规则同 catalog isSafePathSegment 语义）。
      const QString n = m_name->text().trimmed();
      if (n.isEmpty())
      {
        m_err->setText(tr("名称不能为空"));
        m_err->show();
        return;
      }
      if (n.contains(QLatin1Char('/')) || n.contains(QLatin1Char('\\')) ||
          n.contains(QStringLiteral("..")))
      {
        m_err->setText(tr("名称含非法字符（/ \\ ..）"));
        m_err->show();
        return;
      }
      accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }
  void loadEntity(const CatalogEntity &e, const EntityOverride &ov)
  {
    m_name->setText(ov.name.isEmpty() ? e.name : ov.name);
    if (ov.hasCoords)
    {
      m_x->setValue(ov.surfaceX);
      m_y->setValue(ov.surfaceY);
    }
    else if (e.hasSurface)
    {
      m_x->setValue(e.surfaceX);
      m_y->setValue(e.surfaceY);
    }
    m_note->setPlainText(ov.note);
  }
  EntityEditDialog &edited(EntityOverride *out) const
  {
    out->name = m_name->text().trimmed();
    out->hasCoords = true;
    out->surfaceX = m_x->value();
    out->surfaceY = m_y->value();
    out->note = m_note->toPlainText().trimmed();
    return const_cast<EntityEditDialog &>(*this);
  }
  QString editedName() const { return m_name->text().trimmed(); }
  double editedX() const { return m_x->value(); }
  double editedY() const { return m_y->value(); }
  QString editedNote() const { return m_note->toPlainText().trimmed(); }

private:
  QLineEdit *m_name = nullptr;
  QDoubleSpinBox *m_x = nullptr, *m_y = nullptr;
  QPlainTextEdit *m_note = nullptr;
  QLabel *m_err = nullptr;
};

// ---- D4.10 最近操作历史小窗 --------------------------------------------------------
// 会话内（不落盘）：撤销/重做/批量/编辑操作推入，最新在前；点击历史条目
// 不回放（审计面，不是执行面）。
class OperationsHistoryDialog : public QDialog
{
  Q_OBJECT
public:
  explicit OperationsHistoryDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("operationsHistoryDialog"));
    setWindowTitle(tr("最近操作"));
    setModal(false);
    resize(420, 300);
    auto *lay = new QVBoxLayout(this);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("operationsHistoryList"));
    lay->addWidget(m_list, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }
  void setEntries(const QStringList &entries)
  {
    m_list->clear();
    m_list->addItems(entries);
  }

private:
  QListWidget *m_list = nullptr;
};

} // namespace paleo::dataops
