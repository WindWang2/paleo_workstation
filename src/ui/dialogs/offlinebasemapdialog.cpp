// 层：视图
#include "offlinebasemapdialog.h"
#include "../paleotheme.h"
#include "../../workflow/offlinebasemapimport.h"
#include "../../qgis/qgisprojectservice.h"
#include "../../services/paleotaskservice.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include "ui/common/screenfit.h" // R7 #887：默认尺寸按所在屏收紧
OfflineBasemapDialog::OfflineBasemapDialog(bool registered, QWidget *parent) : QDialog(parent) {
  setObjectName("offlineBasemapImportDialog"); setWindowTitle(tr("导入工程离线底图")); paleo::ui::resizeWithinScreen(this, 620, 300);
  auto *layout = new QVBoxLayout(this); const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingMd, tokens.spacingMd, tokens.spacingMd, tokens.spacingMd); layout->setSpacing(tokens.spacingSm);
  auto *hint = new QLabel(tr("导入已下载的 MBTiles、GeoTIFF、IMG、栅格 GeoPackage 或已配准图片。底图及定位／样式文件会复制进工程，移动整个工程后仍可离线使用。"), this);
  hint->setWordWrap(true); layout->addWidget(hint);
  auto *form = new QFormLayout; auto *row = new QHBoxLayout;
  m_path = new QLineEdit(this); m_path->setObjectName("offlineBasemapPath"); m_path->setPlaceholderText(tr("选择底图文件或粘贴完整路径")); row->addWidget(m_path, 1);
  auto *browse = new QPushButton(tr("选择文件…"), this); browse->setObjectName("offlineBasemapBrowse"); row->addWidget(browse); form->addRow(tr("底图文件"), row);
  m_role = new QComboBox(this); m_role->setObjectName("offlineBasemapRole"); m_role->addItems({tr("地形／影像底图"), tr("地形阴影")}); form->addRow(tr("用作"), m_role); layout->addLayout(form);
  m_display = new QCheckBox(tr("导入后显示工程离线底图"), this); m_display->setObjectName("offlineBasemapDisplay"); m_display->setChecked(registered); layout->addWidget(m_display);
  if (!registered) { auto *note = new QLabel(tr("工程尚未配准：可以先导入保存，完成“文件 → 工程坐标与底图”设置后再显示。"), this); note->setWordWrap(true); layout->addWidget(note); }
  auto *note = new QLabel(tr("同一用途的新底图替换当前选择，已有文件保留。普通 PNG／JPG 需带定位文件和坐标系；VRT 请先导出为 GeoTIFF。"), this); note->setWordWrap(true); layout->addWidget(note);
  m_error = new QLabel(this); m_error->setObjectName("offlineBasemapError"); m_error->setTextFormat(Qt::PlainText); m_error->setWordWrap(true); layout->addWidget(m_error);
  m_progress = new QProgressBar(this); m_progress->setRange(0, 0); m_progress->hide(); layout->addWidget(m_progress);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  m_import = buttons->button(QDialogButtonBox::Ok); m_import->setText(tr("导入并保存")); m_import->setObjectName("offlineBasemapConfirm"); m_import->setEnabled(false);
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消")); layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &OfflineBasemapDialog::importRequested);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_path, &QLineEdit::textChanged, this, [this] { m_import->setEnabled(!path().isEmpty()); m_error->clear(); });
  connect(browse, &QPushButton::clicked, this, [this] {
    const auto selected = QFileDialog::getOpenFileName(this, tr("选择离线底图"), m_path->text(),
        tr("离线栅格底图 (*.mbtiles *.tif *.tiff *.img *.gpkg *.png *.jpg *.jpeg);;所有文件 (*)"));
    if (!selected.isEmpty()) m_path->setText(selected);
  });
}
QString OfflineBasemapDialog::path() const { return m_path->text().trimmed(); }
bool OfflineBasemapDialog::hillshade() const { return m_role->currentIndex() == 1; }
bool OfflineBasemapDialog::display() const { return m_display->isChecked(); }
void OfflineBasemapDialog::setBusy(bool busy) {
  m_path->setEnabled(!busy); m_role->setEnabled(!busy); m_display->setEnabled(!busy);
  findChild<QPushButton *>("offlineBasemapBrowse")->setEnabled(!busy);
  m_import->setEnabled(!busy && !path().isEmpty()); m_progress->setVisible(busy);
}
void OfflineBasemapDialog::showError(const QString &error) { m_error->setText(error); }
void openOfflineBasemapImport(QgisProjectService *project, QgisLayerService *layers, PaleoTaskService *tasks, QWidget *parent) {
  if (!project || project->projectPath().isEmpty() || project->isReadOnly() || project->isOpening()) return;
  auto *dialog = new OfflineBasemapDialog(project->mapConfiguration().georeference.has_value(), parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose); QPointer<OfflineBasemapDialog> view(dialog);
  const auto session = project->sessionId();
  QObject::connect(dialog, &OfflineBasemapDialog::importRequested, dialog, [=] {
    if (project->sessionId() != session) { dialog->showError(QObject::tr("工程已切换，请重新打开导入。")); return; }
    QString error;
    auto *task = PaleoOfflineBasemap::importFile(project, layers, tasks, {dialog->path(), dialog->hillshade(), dialog->display()}, dialog,
        [view](bool success, const QString &error) { if (!view) return; view->setBusy(false); if (success) view->accept(); else view->showError(error); }, &error);
    if (!task) { dialog->showError(error); return; }
    dialog->showError(QString()); dialog->setBusy(true);
    QObject::connect(dialog, &QDialog::rejected, task, &PaleoTask::requestCancel);
    QObject::connect(dialog, &QObject::destroyed, task, &PaleoTask::requestCancel);
  });
  dialog->open();
}
