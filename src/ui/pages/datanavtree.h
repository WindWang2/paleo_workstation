// 层：视图
// ui/pages/datanavtree — D3 拖拽的数据导航树（dataTree 本体换成本子类，
// objectName 不变）。
//
// 拖放语义（D3.x）：
//   · 树内资产节点拖到实体（井）节点 = 挂接/转移挂接（D3.1/D3.4）
//   · 资产拖到标签分组节点 = 打标签（D3.5）
//   · 拖源提供自定义 mime（application/x-paleo-asset-ids），拖到预览区/
//     标签栏的宿主（壳/测试）消费（D3.3 拖源半边）
//   · 多选拖拽：拖任一选中项带全部选中（D3.7）
//   · 非法目标：红线反馈 + tooltip 原因（D3.6）；Esc/拖出窗口由 Qt 取消
//     语义 + dragLeave 清反馈（D3.8）
//   · 外部文件/目录拖入 = 导入流程（D3.2）——发信号给装配方（复用
//     FolderImport 确认表由壳执行；本树只做 URL 解包 + 预估提示）。
#pragma once

#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>

#include "../paleotheme.h"

namespace paleo::dataops
{

// 树节点类型标记（UserRole+2 既有词表扩展 dataops 节点）。
inline constexpr const char *kMimeAssetIds = "application/x-paleo-asset-ids";

class DataNavTree : public QTreeWidget
{
  Q_OBJECT

public:
  explicit DataNavTree(QWidget *parent = nullptr)
    : QTreeWidget(parent)
  {
    // D1.1：ExtendedSelection（Ctrl/Shift/框选，跨类型混合选中）。
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    // D3：树内拖放（资产→实体/标签节点）+ 接收外部文件。
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
  }

  static bool isAssetMime(const QMimeData *mime)
  {
    return mime && mime->hasFormat(QLatin1String(kMimeAssetIds));
  }
  static QStringList assetIdsFromMime(const QMimeData *mime)
  {
    if (!isAssetMime(mime))
      return QStringList();
    QString raw = QString::fromUtf8(mime->data(QLatin1String(kMimeAssetIds)));
    return raw.split(QLatin1Char(';'), Qt::SkipEmptyParts);
  }
  static QMimeData *mimeForAssetIds(const QStringList &ids)
  {
    auto *mime = new QMimeData;
    mime->setData(QLatin1String(kMimeAssetIds), ids.join(QLatin1Char(';')).toUtf8());
    // 文本回退：拖到外部目标（文件管理器/编辑器）时给文件名列表。
    mime->setText(ids.join(QLatin1Char('\n')));
    return mime;
  }

  // 当前拖拽悬浮的目标节点（供测试断言反馈态）。
  QTreeWidgetItem *dropTargetItem() const { return m_dropTarget; }
  QString lastDropRejectReason() const { return m_lastReject; }
  // 编程式驱动面：QApplication::notify 会吞掉无拖拽会话的合成
  // DragMove/Drop（真实路由只在 QDrag::exec 会话内），测试/自动化直调。
  void handleDragMove(QDragMoveEvent *e) { dragMoveEvent(e); }
  void handleDrop(QDropEvent *e) { dropEvent(e); }
  void handleDragLeave(QDragLeaveEvent *e) { dragLeaveEvent(e); }

signals:
  // D3.1/D3.4：资产（可能多个）拖到实体节点 = 挂接（未决）/ 转移（已决）。
  void assetsDroppedOnEntity(const QStringList &assetIds, const QString &entityId); // NOLINT(readability-inconsistent-declaration-parameter-name)
  // D3.5：资产拖到标签节点 = 打标签。
  void assetsDroppedOnTag(const QStringList &assetIds, const QString &tag); // NOLINT(readability-inconsistent-declaration-parameter-name)
  // D3.2：外部文件/目录拖入（绝对路径列表）。
  void externalFilesDropped(const QStringList &paths); // NOLINT(readability-inconsistent-declaration-parameter-name)
  // D3.3：拖源启动（预览区/标签栏宿主可监听 drag 提前量；本信号带 id 集）。
  void assetDragStarted(const QStringList &assetIds); // NOLINT(readability-inconsistent-declaration-parameter-name)

protected:
  void startDrag(Qt::DropActions supportedActions) override
  {
    // D3.7：拖任一选中项带全部选中（选中集非空优先于单节点）。
    QStringList ids;
    const QList<QTreeWidgetItem *> sel = selectedItems();
    for (QTreeWidgetItem *it : sel)
    {
      const QString id = it->data(0, Qt::UserRole).toString();
      const QString nodeType = it->data(0, Qt::UserRole + 2).toString();
      if (!id.isEmpty() && nodeType != QLatin1String("category") &&
          nodeType != QLatin1String("survey_area"))
        ids << id;
    }
    if (ids.isEmpty())
      return; // 分类/测区节点不可拖
    if (sel.size() == 1)
      emit assetDragStarted(ids);
    else
      emit assetDragStarted(ids);
    auto *drag = new QDrag(this);
    drag->setMimeData(mimeForAssetIds(ids));
    drag->exec(supportedActions, Qt::MoveAction);
  }

  void dragEnterEvent(QDragEnterEvent *event) override
  {
    if (event->mimeData()->hasUrls())
    {
      event->acceptProposedAction(); // D3.2 外部文件/夹
      return;
    }
    if (isAssetMime(event->mimeData()))
    {
      event->acceptProposedAction(); // 树内拖
      return;
    }
    event->ignore();
  }

  void dragMoveEvent(QDragMoveEvent *event) override
  {
    if (event->mimeData()->hasUrls())
    {
      clearDropFeedback();
      event->acceptProposedAction();
      return;
    }
    if (!isAssetMime(event->mimeData()))
    {
      event->ignore();
      return;
    }
    QTreeWidgetItem *target = itemAt(event->position().toPoint());
    const QString reason = dropRejectReason(target);
    if (!reason.isEmpty())
    {
      // D3.6：非法目标红线反馈 + 原因 tooltip。
      showDropFeedback(target, reason);
      event->ignore();
      return;
    }
    clearDropFeedback();
    if (target)
    {
      m_dropTarget = target;
      highlightTarget(target, true);
    }
    event->acceptProposedAction();
  }

  void dragLeaveEvent(QDragLeaveEvent *event) override
  {
    // D3.8：拖出窗口/取消 → 清全部反馈。
    clearDropFeedback();
    QTreeWidget::dragLeaveEvent(event);
  }

  void dropEvent(QDropEvent *event) override
  {
    clearDropFeedback();
    if (event->mimeData()->hasUrls())
    {
      QStringList paths;
      const QList<QUrl> urls = event->mimeData()->urls();
      for (const QUrl &u : urls)
        if (u.isLocalFile())
          paths << u.toLocalFile();
      if (!paths.isEmpty())
      {
        event->acceptProposedAction();
        emit externalFilesDropped(paths);
      }
      return;
    }
    if (!isAssetMime(event->mimeData()))
    {
      event->ignore();
      return;
    }
    QTreeWidgetItem *target = itemAt(event->position().toPoint());
    const QString reason = dropRejectReason(target);
    const QStringList ids = assetIdsFromMime(event->mimeData());
    if (!reason.isEmpty() || ids.isEmpty() || !target)
    {
      // D3.6：drop 落在非法目标同样给出原因反馈（不只是 dragMove 悬浮态）。
      if (!reason.isEmpty())
        showDropFeedback(target, reason);
      event->ignore();
      return;
    }
    event->acceptProposedAction();
    const QString nodeType = target->data(0, Qt::UserRole + 2).toString();
    if (nodeType == QLatin1String("tag_group") || nodeType == QLatin1String("tag_leaf"))
    {
      const QString tag = target->data(0, Qt::UserRole + 3).toString();
      emit assetsDroppedOnTag(ids, tag);
      return;
    }
    // 井节点（UserRole+1 = entityId）或资产节点下挂井上下文。
    QString entityId = target->data(0, Qt::UserRole + 1).toString();
    if (entityId.isEmpty() && target->parent())
      entityId = target->parent()->data(0, Qt::UserRole + 1).toString();
    if (!entityId.isEmpty() && target->data(0, Qt::UserRole + 2).toString() != QLatin1String("seismic_line"))
      emit assetsDroppedOnEntity(ids, entityId);
  }

  void keyPressEvent(QKeyEvent *event) override
  {
    // D6.5 Vim 风可选（默认关）：j/k 上下、g 跳顶、G 跳底、/ 聚焦搜索。
    if (vimMode())
    {
      switch (event->key())
      {
        case Qt::Key_J:
          navigateBy(+1);
          return;
        case Qt::Key_K:
          navigateBy(-1);
          return;
        case Qt::Key_G:
          if (event->modifiers() & Qt::ShiftModifier)
          {
            if (topLevelItemCount() > 0)
              setCurrentItem(lastItem());
          }
          else if (topLevelItemCount() > 0)
            setCurrentItem(topLevelItem(0));
          return;
        case Qt::Key_Slash:
          emit searchFocusRequested();
          return;
        default:
          break;
      }
    }
    QTreeWidget::keyPressEvent(event);
  }

signals:
  void searchFocusRequested();

public:
  // D6.5 开关（QSettings dataops/vimMode；DataPage 装配时下发）。
  bool vimMode() const { return m_vim; }
  void setVimMode(bool on) { m_vim = on; }

private:
  void navigateBy(int delta)
  {
    // 视觉序导航：indexRow() 需 item 的父链——这里用 QTreeWidgetItemIterator
    // 展平序做 ±1（与 ↑↓ 键视觉序一致）。
    QTreeWidgetItemIterator it(this);
    QTreeWidgetItem *cur = currentItem();
    QTreeWidgetItem *prev = nullptr, *next = nullptr;
    bool sawCur = false;
    while (*it)
    {
      if (*it == cur)
      {
        sawCur = true;
        ++it;
        continue;
      }
      if (!sawCur)
        prev = *it;
      else
      {
        next = *it;
        break;
      }
      ++it;
    }
    QTreeWidgetItem *to = delta > 0 ? next : prev;
    if (to)
      setCurrentItem(to);
  }

  QTreeWidgetItem *lastItem() const
  {
    QTreeWidgetItem *it = topLevelItemCount() > 0 ? topLevelItem(topLevelItemCount() - 1) : nullptr;
    while (it && it->childCount() > 0)
      it = it->child(it->childCount() - 1);
    return it;
  }

  // 目标合法性：井节点 / 标签节点可落；分类、资产自身、测区、测线、根都拒。
  QString dropRejectReason(QTreeWidgetItem *target) const
  {
    if (!target)
      return QStringLiteral("not-item");
    const QString nodeType = target->data(0, Qt::UserRole + 2).toString();
    if (nodeType == QLatin1String("well"))
      return QString();
    if (nodeType == QLatin1String("tag_group") || nodeType == QLatin1String("tag_leaf"))
      return QString();
    if (nodeType == QLatin1String("category"))
      return QStringLiteral("category");
    if (nodeType == QLatin1String("survey_area"))
      return QStringLiteral("survey");
    if (nodeType == QLatin1String("seismic_line"))
      return QStringLiteral("line");
    if (!target->data(0, Qt::UserRole).toString().isEmpty())
      return QStringLiteral("asset");
    return QStringLiteral("unknown");
  }

  void showDropFeedback(QTreeWidgetItem *target, const QString &reasonKey)
  {
    m_lastReject = reasonText(reasonKey);
    if (target)
      target->setBackground(0, PaleoTheme::tokens().error); // D3.6 红线
    if (viewport())
    {
      // tooltip 原因：挂在视口（offscreen 下不弹窗也能断言字符串）。
      viewport()->setToolTip(m_lastReject);
    }
  }
  void clearDropFeedback()
  {
    if (m_dropTarget)
      m_dropTarget->setBackground(0, QBrush());
    m_dropTarget = nullptr;
    m_lastReject.clear();
    if (viewport())
      viewport()->setToolTip(QString());
  }
  void highlightTarget(QTreeWidgetItem *target, bool on)
  {
    Q_UNUSED(on);
    if (target)
      target->setBackground(0, PaleoTheme::tokens().surfaceAltRaised);
  }

  static QString reasonText(const QString &key)
  {
    if (key == QLatin1String("category"))
      return DataNavTree::tr("分类节点不可作为挂接目标 — 请拖到具体井或标签");
    if (key == QLatin1String("survey"))
      return DataNavTree::tr("测区节点不支持挂接");
    if (key == QLatin1String("line"))
      return DataNavTree::tr("测线节点不支持挂接 — 请拖到地震数据体所在工区");
    if (key == QLatin1String("asset"))
      return DataNavTree::tr("资产节点不可作为目标 — 挂接请拖到井节点");
    return DataNavTree::tr("此处不可放置");
  }

  bool m_vim = false;
  QTreeWidgetItem *m_dropTarget = nullptr;
  QString m_lastReject;

};

} // namespace paleo::dataops
