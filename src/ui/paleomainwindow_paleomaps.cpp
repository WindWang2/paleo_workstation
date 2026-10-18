// 层：视图
#include "paleomainwindow.h"
#include "pages/datapage.h"
#include "notifications/paleonotify.h"
#include "../catalog/datacatalog.h"
#include "../domain/mappinghorizons.h"
#include "../linkage/selectioncontext.h"
#include "../services/previewdoc.h"
#include "../workflow/paleomaplibrary.h"
#include "../qgis/qgisprojectservice.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QStatusBar>
#include <QPushButton>
#include <QVBoxLayout>
void PaleoMainWindow::attachPaleomapLibrary(DataPage *page) {
  if (!page) return;
  const auto restore = [this] {
    if (m_previewDoc) {
      const auto errors = PaleoMapLibrary::declareMaps(m_previewDoc->catalog(), m_layerSvc);
      if (!errors.isEmpty()) statusBar()->showMessage(errors.join("\n"), 10000);
    }
  };
  if (m_projectSvc) connect(m_projectSvc, &QgisProjectService::projectOpened, this, restore);
  restore();
  connect(page, &DataPage::importRequested, this, [this](const QString &kind) {
    if (kind != "paleomap" && kind != "paleomap_folder") return;
    PaleoMapLibrary::Request request;
    if (kind == "paleomap_folder") request.folder = QFileDialog::getExistingDirectory(this, tr("导入古地理图文件夹"));
    else request.files = QFileDialog::getOpenFileNames(this, tr("导入现有古地理图"), {}, tr("古地理图 (*.shp *.gpkg *.geojson *.tif *.tiff *.img *.png *.jpg *.jpeg *.pdf *.svg);;所有文件 (*)"));
    if (request.folder.isEmpty() && request.files.isEmpty()) return;
    QDialog dialog(this); dialog.setObjectName("paleomapImportDialog"); dialog.setWindowTitle(tr("古地理图层位归属"));
    auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    auto *horizon = new QComboBox(&dialog); horizon->setObjectName("paleomapImportHorizon"); horizon->addItems(mappingHorizons());
    if (m_selection) horizon->setCurrentText(m_selection->activeHorizon());
    form->addRow(tr("所属层位／未识别时使用"), horizon); layout->addLayout(form);
    auto *infer = new QCheckBox(tr("优先按目录或文件名识别层位"), &dialog); infer->setChecked(true); layout->addWidget(infer);
    auto *hint = new QLabel(tr("Shapefile 按一套图件管理，须保留同名 SHP、SHX、DBF；配套坐标和样式文件会一并保存。无法识别或名称含多个层位时使用上方层位。未配准图片和 PDF 作为参考图管理。"), &dialog);
    hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("开始导入")); buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    request.horizon = horizon->currentText(); request.inferHorizons = infer->isChecked(); QString error;
    auto *task = PaleoMapLibrary::importMaps(m_previewDoc ? m_previewDoc->catalog() : nullptr, m_layerSvc, m_taskSvc, request, this,
        [this](const PaleoMapLibrary::Result &result) {
      const auto message = result.cancelled ? tr("古地理图导入已取消") : tr("已导入 %1 项，内容未变 %2 项，问题 %3 项").arg(result.assets.size()).arg(result.unchanged).arg(result.errors.size());
      statusBar()->showMessage(message, 15000);
      if (!result.errors.isEmpty()) PaleoNotify::warning(this, tr("古地理图导入"), message + "\n" + result.errors.join("\n"));
    }, &error);
    statusBar()->showMessage(task ? tr("古地理图正在后台导入，可在任务中心查看或取消") : error, 15000);
  });
}
