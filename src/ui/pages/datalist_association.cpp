// 层：视图
#include "datalist.h"
#include "../paleotheme.h"
#include "../../workflow/assetassociation.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

void DataListPanel::editAssociationForSelection() {
  const auto selected = currentAssetSelection();
  if (!m_ctx.valid() || selected.size() != 1) return;
  const auto assetId = *selected.begin();
  const QPointer<DataCatalog> cat = m_ctx.cat;
  const auto sequence = cat->mutationSeq();
  const auto project = cat->catalogPath();
  auto *dialog = new QDialog(this);
  dialog->setObjectName("assetAssociationEditor");
  dialog->setWindowTitle(tr("调整资料归属"));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  auto *layout = new QVBoxLayout(dialog);
  layout->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd,
                            PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *file = new QLabel(tr("文件：%1").arg(cat->assetById(assetId).displayName), dialog);
  file->setWordWrap(true); layout->addWidget(file);
  auto *form = new QFormLayout;
  auto *operation = new QComboBox(dialog); operation->setObjectName("associationOperation");
  operation->addItems({tr("添加补充资料"), tr("替换此用途的主文件"), tr("更改已有归属或用途")});
  form->addRow(tr("操作"), operation);
  auto *source = new QComboBox(dialog); source->setObjectName("associationSource");
  const auto links = cat->links();
  for (int i = 0; i < links.size(); ++i) {
    const auto &link = links[i];
    if (link.assetId != assetId) continue;
    const auto entity = cat->entityById(link.entityId);
    QString purpose = link.role;
    for (const auto &entry : cat->roleRegistry().forEntity(link.entityType))
      if (entry.role == link.role) purpose = entry.display;
    source->addItem(tr("%1 · %2%3").arg(link.unresolved ? tr("待关联") : entity.name,
        purpose, link.isPrimary ? tr(" · 主文件") : QString()), i);
  }
  source->setPlaceholderText(tr("此文件尚无关联"));
  source->setEnabled(false);
  form->addRow(tr("原关联"), source);
  auto *target = new QComboBox(dialog); target->setObjectName("associationTarget");
  target->addItem(tr("选择井或地震测区"), QString());
  for (const auto &entity : cat->entities())
    if ((entity.entityType == "well" || entity.entityType == "seismic_survey") && !m_recycle.isRemoved(entity.id))
      target->addItem(tr("%1 · %2").arg(entity.entityType == "well" ? tr("井") : tr("地震测区"),
          m_entityOv.displayName(entity)), entity.id);
  target->setEditable(true); target->setInsertPolicy(QComboBox::NoInsert);
  target->completer()->setCaseSensitivity(Qt::CaseInsensitive);
  target->completer()->setFilterMode(Qt::MatchContains);
  form->addRow(tr("归属对象"), target);
  auto *role = new QComboBox(dialog); role->setObjectName("associationRole");
  form->addRow(tr("资料用途"), role);
  auto *primary = new QCheckBox(tr("作为此用途的主文件"), dialog);
  primary->setObjectName("associationPrimary"); primary->setEnabled(false);
  form->addRow(QString(), primary);
  layout->addLayout(form);
  auto *hint = new QLabel(dialog); hint->setObjectName("associationHint"); hint->setWordWrap(true);
  layout->addWidget(hint);
  auto *error = new QLabel(dialog); error->setObjectName("associationError"); error->setWordWrap(true);
  layout->addWidget(error);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
  buttons->button(QDialogButtonBox::Save)->setText(tr("保存关联"));
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消")); layout->addWidget(buttons);
  const auto updateRoles = [cat, target, role, source, links] {
    if (!cat) return;
    const auto previous = role->currentData().toString();
    role->clear();
    for (const auto &entry : cat->roleRegistry().forEntity(cat->entityById(target->currentData().toString()).entityType))
      role->addItem(entry.display, entry.role);
    QString preferred = previous;
    const int row = source->currentIndex() < 0 ? -1 : source->currentData().toInt();
    if (preferred.isEmpty() && row >= 0 && row < links.size()) preferred = links[row].role;
    role->setCurrentIndex(qMax(0, role->findData(preferred)));
  };
  const auto update = [cat, target, role, operation, source, primary, hint, buttons] {
    source->setEnabled(operation->currentIndex() == 2);
    primary->setEnabled(operation->currentIndex() == 2);
    if (operation->currentIndex() != 2) primary->setChecked(operation->currentIndex() == 1);
    hint->setText(operation->currentIndex() == 0
      ? tr("将文件加入所选用途；已有主文件保持生效。尚无主文件时，首份资料自动成为主文件。")
      : operation->currentIndex() == 1
        ? tr("此文件成为所选用途的主文件，原主文件保留为补充资料。原始文件和历史版本均保留。")
        : tr("只更改上方选中的一条关联，其他井、测区及用途的关联保持不变。"));
    buttons->button(QDialogButtonBox::Save)->setEnabled(cat && !target->currentData().toString().isEmpty() &&
      target->currentText() == target->itemText(target->currentIndex()) && role->count() > 0 &&
      (operation->currentIndex() != 2 || source->count() > 0));
  };
  connect(target, &QComboBox::currentIndexChanged, dialog, [updateRoles, update] { updateRoles(); update(); });
  connect(target, &QComboBox::editTextChanged, dialog, [update] { update(); });
  connect(operation, &QComboBox::currentIndexChanged, dialog, [update] { update(); });
  connect(source, &QComboBox::currentIndexChanged, dialog, [source, target, primary, links, updateRoles, update] {
    const int row = source->currentIndex() < 0 ? -1 : source->currentData().toInt();
    if (row < 0 || row >= links.size()) return;
    target->setCurrentIndex(qMax(0, target->findData(links[row].entityId)));
    primary->setChecked(links[row].isPrimary); updateRoles(); update();
  });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, dialog,
      [this, cat, sequence, project, assetId, operation, source, target, role, primary, error, dialog] {
    if (!cat || cat != m_ctx.cat || cat->catalogPath() != project) { error->setText(tr("工程已切换，请关闭此编辑器后重试")); return; }
    paleo::assetassociation::Change change{assetId, target->currentData().toString(), role->currentData().toString(),
      sequence, operation->currentIndex() == 2 ? source->currentData().toInt() : -1, primary->isChecked()};
    if (m_recycle.isRemoved(assetId) || m_recycle.isRemoved(change.entityId)) {
      error->setText(tr("文件或目标对象已移入回收清单，请重新选择")); return;
    }
    QString message;
    if (!paleo::assetassociation::apply(cat, change, &message)) { error->setText(message); return; }
    if (m_opStack && cat->mutationSeq() != sequence) m_opStack->clear(); // 旧命令保留的是更改前的关联身份。
    if (m_history) m_history->push(tr("调整资料归属：%1 → %2 · %3").arg(assetId, target->currentText(), role->currentText()));
    refreshAssetTable(); emit entityRefreshRequested(); emit assetAssociationChanged(assetId);
    emit statusMessage(tr("已更新资料归属，井与地震资料视图将同步刷新"));
    dialog->accept();
  });
  operation->setCurrentIndex(source->count() ? 2 : 0);
  if (source->count()) {
    const auto &link = links[source->currentData().toInt()];
    target->setCurrentIndex(qMax(0, target->findData(link.entityId)));
    primary->setChecked(link.isPrimary);
  }
  updateRoles(); update(); dialog->resize(560, 400); dialog->show();
}
