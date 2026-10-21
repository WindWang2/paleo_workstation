// 层：视图
#include "folderconfirm_internal.h"
#include "../paleotheme.h"
#include "../../domain/projectclassifier.h"
#include <QComboBox>
#include <QDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace PaleoFolderConfirm::detail {
namespace {
bool isWellLog(const DialogState &state, int row) {
  return state.typeChoices.value(row) &&
         state.typeChoices[row]->currentData().toString() == QLatin1String("well_log");
}
}

void addWellChoice(const std::shared_ptr<DialogState> &state, int row) {
  const auto &source = state->preview[row];
  auto *choice = new QComboBox(state->table);
  choice->setObjectName(QStringLiteral("folderWell%1").arg(row));
  choice->setAccessibleName(QObject::tr("%1 的目标井").arg(QFileInfo(source.path).fileName()));
  choice->addItem(QObject::tr("选择已有井或新建井…"), QString());
  for (auto it = source.availableWells.cbegin(); it != source.availableWells.cend(); ++it)
    choice->addItem(QObject::tr("已有井：%1").arg(it.value()), it.key());
  if (!source.proposedWellName.isEmpty())
    choice->addItem(QObject::tr("新建井：%1").arg(source.proposedWellName),
                    QStringLiteral("new:") + source.proposedWellName);
  choice->addItem(QObject::tr("新建其他井…"), QStringLiteral("__new__"));
  choice->setToolTip(QObject::tr("确认导入后，测井文件按此选择归属；原有同名曲线保留。"));
  state->wellChoices.insert(row, choice);
  state->table->setCellWidget(row, 2, choice);
  QObject::connect(choice, &QComboBox::currentIndexChanged, state->dialog,
                   [state, choice, row](int) {
    QString target = choice->currentData().toString();
    if (target == QLatin1String("__new__")) {
      bool accepted = false;
      const QString name = QInputDialog::getText(state->dialog, QObject::tr("新建井"),
          QObject::tr("井名（确认导入后创建）"), QLineEdit::Normal,
          state->preview[row].proposedWellName, &accepted).trimmed();
      if (!accepted || name.isEmpty()) {
        choice->setCurrentIndex(0);
        return;
      }
      // F4（review-security）：撞名不新建——输入与已有井精确同名时直接改选
      // 已有井并告知，不留「新建井:X」却静默并入旧 X 的假意图。
      for (auto it = state->preview[row].availableWells.cbegin();
           it != state->preview[row].availableWells.cend(); ++it)
        if (it.value().trimmed().compare(name, Qt::CaseInsensitive) == 0)
        {
          const int existIdx = choice->findData(it.key());
          if (existIdx >= 0)
          {
            choice->setCurrentIndex(existIdx); // 重入本槽把目标写为已有井 id
            state->errors->setText(QObject::tr("井 %1 已存在，已为你改选已有井；数据将归入该井。")
                                       .arg(it.value()));
            state->errors->show();
            return;
          }
        }
      target = QStringLiteral("new:") + name;
      int index = choice->findData(target);
      if (index < 0) {
        index = choice->count() - 1;
        choice->insertItem(index, QObject::tr("新建井：%1").arg(name), target);
      }
      choice->setCurrentIndex(index);
    }
    if (target.isEmpty()) state->wellTargets.remove(state->preview[row].path);
    else state->wellTargets.insert(state->preview[row].path, target);
    state->updateAvailability();
  });
}

void DialogState::buildPreview(QVBoxLayout *layout) {
  const auto state = shared_from_this();
  dataPreview = new QPlainTextEdit(dialog);
  dataPreview->setObjectName(QStringLiteral("folderDataPreview"));
  dataPreview->setAccessibleName(QObject::tr("所选文件内容预览"));
  dataPreview->setReadOnly(true);
  dataPreview->setFont(PaleoTheme::monoFont());
  dataPreview->setMaximumHeight(dataPreview->fontMetrics().lineSpacing() * 8);
  dataPreview->hide();
  layout->addWidget(dataPreview);
  for (int row = 0; row < preview.size(); ++row) {
    const auto &source = preview[row];
    if (source.sizeBytes >= 0)
      table->item(row, 0)->setToolTip(table->item(row, 0)->toolTip() +
          QObject::tr("\n大小 %1 字节").arg(source.sizeBytes));
    QStringList status;
    if (!source.summary.isEmpty()) status << source.summary;
    if (!table->item(row, 3)->text().isEmpty()) status << table->item(row, 3)->text();
    if (!source.issues.isEmpty()) {
      status << source.issues;
      if (hooks.importAllWithWells && source.issues.join(QLatin1Char('\n')).contains(QObject::tr("同名")))
        status << QObject::tr("取消勾选可保留已有资料；保持勾选将另存并保留两份。");
      table->item(row, 3)->setToolTip(source.issues.join(QLatin1Char('\n')));
    }
    if (source.importBlocked)
      status.prepend(QObject::tr("该项不可导入；其余有效项可继续"));
    table->item(row, 3)->setText(status.join(QLatin1Char('\n')));
    // 自动建议仅展示；执行器按当前内容重新解析身份。只有用户选择才是覆盖。
    if (!source.sourceSha256.isEmpty())
      expectedHashes.insert(source.path, source.sourceSha256);
    if (source.needsWellChoice && !source.skipped && !source.importBlocked)
      addWellChoice(state, row);
    QObject::connect(typeChoices[row], &QComboBox::currentIndexChanged, dialog,
                     [state](int) { state->updateAvailability(); });
    if ((hooks.importAllWithForced || hooks.importAllWithWells) &&
        !source.skipped && source.decision == QLatin1String("skip")) {
      auto *force = new QPushButton(QObject::tr("仍导入"), table);
      force->setObjectName(QStringLiteral("folderForceImport%1").arg(row));
      force->setAccessibleName(QObject::tr("仍导入 %1").arg(table->item(row, 0)->text()));
      QObject::connect(force, &QPushButton::clicked, dialog, [state, force, row] {
        if (!state->rowSelected(row)) return;
        const QString path = state->preview[row].path;
        if (!state->forcePaths.contains(path)) state->forcePaths << path;
        force->setText(QObject::tr("将导入"));
        force->setEnabled(false);
        state->updateAvailability();
      });
      table->setCellWidget(row, 3, force);
    }
  }
  QObject::connect(table, &QTableWidget::currentCellChanged, dialog,
                   [state](int row, int, int, int) { state->showDataPreview(row); });
  QObject::connect(table, &QTableWidget::itemChanged, dialog,
                   [state](QTableWidgetItem *item) {
    if (item->column() == 0 && !state->importing && !state->completed)
      state->updateAvailability();
  });
  if (!preview.isEmpty()) {
    table->setCurrentCell(0, 0);
    showDataPreview(0);
  }
}

void DialogState::showDataPreview(int row) {
  if (row < 0 || row >= preview.size()) return;
  const auto &source = preview[row];
  QStringList lines;
  if (!source.summary.isEmpty()) lines << source.summary;
  if (!source.dataPreview.isEmpty()) lines << source.dataPreview;
  if (!source.issues.isEmpty()) lines << source.issues;
  dataPreview->setPlainText(lines.join(QLatin1Char('\n')));
  dataPreview->setVisible(!lines.isEmpty());
}

bool DialogState::rowSelected(int row) const {
  return !preview[row].skipped && (!hooks.importAllWithWells ||
      table->item(row, 0)->checkState() == Qt::Checked);
}

void DialogState::updateAvailability() {
  if (importing) return;
  // 手动改成测井时，原预览没有读出井身份；必须显式选归属。先建控件再遍历，
  // 避免迭代 wellChoices 途中插入导致迭代器失效。
  for (int row = 0; row < preview.size(); ++row)
    if (!preview[row].skipped && preview[row].classifiedType != QLatin1String("well_log") &&
        isWellLog(*this, row) && !wellChoices.contains(row))
      addWellChoice(shared_from_this(), row);
  int unresolved = 0;
  if (!completed)
    for (int row = 0; row < preview.size(); ++row) {
      const bool selected = rowSelected(row);
      typeChoices[row]->setEnabled(selected && preview[row].typeEditable &&
          !isFixedAuxiliaryPath(preview[row].path));
      if (auto *force = table->findChild<QPushButton *>(QStringLiteral("folderForceImport%1").arg(row)))
        force->setEnabled(selected && !forcePaths.contains(preview[row].path));
    }
  for (auto it = wellChoices.cbegin(); it != wellChoices.cend(); ++it) {
    const int row = it.key();
    const bool active = isWellLog(*this, row) &&
        (preview[row].decision != QLatin1String("skip") || forcePaths.contains(preview[row].path));
    it.value()->setVisible(active);
    if (!completed) {
      it.value()->setEnabled(rowSelected(row));
      it.value()->setToolTip(rowSelected(row)
          ? QObject::tr("确认导入后，测井文件按此选择归属；原有同名曲线保留。")
          : QObject::tr("本次跳过此文件，已有资料保持不变"));
    }
    if (active && rowSelected(row) && wellTargets.value(preview[row].path).isEmpty()) ++unresolved;
  }
  if (completed) return; // 结果页只开放失败行的修正/重试，不再开放整批确认。
  QStringList skipped;
  for (int row = 0; row < preview.size(); ++row)
    if (!preview[row].skipped && !rowSelected(row)) skipped << preview[row].path;
  estimate->setText(folderEstimateText(preview, skipped, forcePaths));
  confirm->setEnabled(unresolved == 0);
  confirm->setToolTip(unresolved ? QObject::tr("请为 %1 项测井数据选择目标井").arg(unresolved)
                                : QObject::tr("按当前分类与目标井导入；问题行原位报告"));
  hint->setText(unresolved
      ? QObject::tr("已自动识别文件；%1 项需要选择目标井，选择后直接确认导入。").arg(unresolved)
      : hooks.importAllWithWells
          ? QObject::tr("已自动识别文件。检查内容后直接确认导入；取消勾选可跳过该文件、保留已有资料。")
          : QObject::tr("已自动识别文件。检查内容后直接确认导入；仅有问题的项目需要调整。"));
}
} // namespace PaleoFolderConfirm::detail
