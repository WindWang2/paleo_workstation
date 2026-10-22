// 层：视图
#include "paleomainwindow.h"
#include "pages/datapage.h"
#include "pages/datalist.h"
#include "notifications/paleonotify.h"
#include "dialogs/importsourcedialog.h"
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
#include <QLineEdit>
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
  connect(page->listPanel(), &DataListPanel::normalizePaleomapsRequested, this,
          [this](const QStringList &ids) {
    auto *catalog = m_previewDoc ? m_previewDoc->catalog() : nullptr;
    if (!catalog || catalog->refusesWrites()) return;
    const auto directory = catalog->projectDir(); const auto sequence = catalog->mutationSeq();
    QDialog dialog(this); dialog.setObjectName("paleomapNormalizeDialog");
    dialog.setWindowTitle(tr("整理三级相与系统纹理"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("整理选中的 %1 套现有古地理图，沿用各自层位。从工程保存的原件识别微相，补齐相 → 亚相 → 微相，并应用系统纹理和分级图例。生成新的三级相版本，原图及历史版本保留。无法识别的类别会报告原因，不猜测归属。").arg(ids.size()), &dialog);
    hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); layout->addWidget(hint);
    auto *form = new QFormLayout;
    auto *field = new QLineEdit(&dialog); field->setObjectName("paleomapNormalizeField");
    field->setPlaceholderText(tr("自动识别；自定义字段如 SEDCLASS"));
    form->addRow(tr("微相字段"), field); layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("开始整理"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (!m_previewDoc || m_previewDoc->catalog() != catalog || catalog->projectDir() != directory || catalog->mutationSeq() != sequence) {
      PaleoNotify::warning(this, tr("三级相整理未开始"), tr("工程或目录已变化，请重新选择图件")); return;
    }
    PaleoMapLibrary::Request request; request.assetIds = ids; request.microfaciesField = field->text().trimmed();
    QString error;
    auto *task = PaleoMapLibrary::importMaps(catalog, m_layerSvc, m_taskSvc, request, this,
        [this](const PaleoMapLibrary::Result &result) {
      const auto message = result.cancelled ? tr("三级相整理已取消")
          : tr("三级相整理：处理 %1 项，内容未变 %2 项，问题 %3 项").arg(result.assets.size()).arg(result.unchanged).arg(result.errors.size());
      statusBar()->showMessage(message, 15000);
      if (!result.errors.isEmpty()) PaleoNotify::warning(this, tr("三级相整理"), message + "\n" + result.errors.join("\n"));
    }, &error);
    statusBar()->showMessage(task ? tr("正在后台整理三级相与纹理，可在任务中心取消") : error, 15000);
  });
  connect(page, &DataPage::importRequested, this, [this](const QString &kind) {
    if (kind != "paleomap" && kind != "paleomap_folder") return;
    if (!m_projectSvc || m_projectSvc->projectPath().isEmpty() || isProjectReadOnly() || m_projectSvc->isOpening()) return;
    const auto session = m_projectSvc->sessionId();
    ImportSourceDialog source(kind, this);
    if (source.exec() != QDialog::Accepted || m_projectSvc->sessionId() != session) return;
    PaleoMapLibrary::Request request;
    request.folder = source.folder(); request.files = source.files();
    if (request.folder.isEmpty() && request.files.isEmpty()) return;
    QDialog dialog(this); dialog.setObjectName("paleomapImportDialog"); dialog.setWindowTitle(tr("古地理图层位归属"));
    auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    auto *horizon = new QComboBox(&dialog); horizon->setObjectName("paleomapImportHorizon"); horizon->addItems(mappingHorizons());
    if (m_selection) horizon->setCurrentText(m_selection->activeHorizon());
    form->addRow(tr("所属层位／未识别时使用"), horizon); layout->addLayout(form);
    auto *infer = new QCheckBox(tr("优先按目录或文件名识别层位"), &dialog); infer->setChecked(true); layout->addWidget(infer);
    auto *normalize = new QCheckBox(tr("识别微相，补齐相／亚相并使用系统纹理"), &dialog);
    normalize->setObjectName("paleomapNormalizeFacies"); normalize->setChecked(true); layout->addWidget(normalize);
    auto *microField = new QLineEdit(&dialog); microField->setObjectName("paleomapMicrofaciesField");
    microField->setPlaceholderText(tr("自动识别；也可填写自定义微相字段名")); form->addRow(tr("微相字段"), microField);
    connect(normalize, &QCheckBox::toggled, microField, &QWidget::setEnabled);
    auto *hint = new QLabel(tr("Shapefile 按一套图件管理，须保留同名 SHP、SHX、DBF；配套坐标和样式文件会一并保存。无法识别或名称含多个层位时使用上方层位。微相优先匹配工程分类，再匹配内置模板；不能确定归属时保留原图并提示。三级相副本另存为 GeoPackage，原始数据不改。未配准图片和 PDF 作为参考图管理。"), &dialog);
    hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("开始导入")); buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (m_projectSvc->sessionId() != session || isProjectReadOnly()) {
      PaleoNotify::warning(this, tr("导入古地理图"), tr("工程已变化，请重新选择资料。")); return;
    }
    request.normalizeFacies = normalize->isChecked(); request.microfaciesField = microField->text().trimmed();
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
