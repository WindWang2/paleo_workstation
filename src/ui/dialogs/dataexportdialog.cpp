// 层：视图
#include "dataexportdialog.h"
#include "../../catalog/datacatalog.h"
#include "../paleotheme.h"
#include "../../workflow/dataexportworkflow.h"
#include "../../services/paleotaskservice.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QTimer>

DataExportDialog::DataExportDialog(DataCatalog *catalog, const QStringList &assetIds, PaleoTaskService *tasks, QWidget *parent)
    : QDialog(parent), m_catalog(catalog), m_tasks(tasks), m_assetIds(assetIds) {
  setObjectName("dataExportDialog"); setWindowTitle(tr("导出数据"));
  auto *root = new QVBoxLayout(this);
  root->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *form = new QFormLayout;
  m_scope = new QComboBox(this); m_scope->setObjectName("dataExportScope");
  m_scope->addItems({tr("当前成果／所选版本"), tr("原始文件（RAW）"), tr("成果及其制作源数据")});
  m_format = new QComboBox(this); m_format->setObjectName("dataExportFormat");
  // 格式名同样过 tr()（同 folderconfirm.cpp:39 口径）——下拉项是用户可见文案。
  m_format->addItems({tr("保持原格式"), tr("自动交换格式（GPKG / GeoTIFF）"), tr("GeoPackage"), tr("GeoJSON"), tr("Shapefile"), tr("CSV"), tr("GeoTIFF")});
  form->addRow(tr("导出内容"), m_scope); form->addRow(tr("数据格式"), m_format);
  m_directory = new QLineEdit(this); m_directory->setObjectName("dataExportDirectory"); m_directory->setReadOnly(true);
  auto *choose = new QPushButton(tr("选择目录…"), this);
  auto *directoryRow = new QHBoxLayout; directoryRow->addWidget(m_directory, 1); directoryRow->addWidget(choose);
  form->addRow(tr("保存到"), directoryRow); root->addLayout(form);
  connect(choose, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("选择导出父目录"), m_directory->text());
    if (!path.isEmpty()) m_directory->setText(path);
  });
  m_references = new QCheckBox(tr("附带仅作参考的资料（非计算输入）"), this);
  root->addWidget(m_references);
  m_versions = new QTableWidget(m_assetIds.size(), 2, this); m_versions->setObjectName("dataExportVersions");
  m_versions->setHorizontalHeaderLabels({tr("数据文件"), tr("导出版本")});
  m_versions->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  m_versions->horizontalHeader()->setStretchLastSection(true);
  root->addWidget(m_versions, 1);
  auto *note = new QLabel(tr("每次导出创建独立批次目录，附版本、来源关系和 SHA-256。\n"
      "制作源数据使用当次输入版本；未保存的编辑请先保存。\n"
      "GeoJSON 需要有效经纬度变换；Shapefile 可能截短字段。"), this);
  note->setWordWrap(true); root->addWidget(note);
  m_summary = new QLabel(this); m_summary->setObjectName("dataExportSummary"); m_summary->setWordWrap(true); root->addWidget(m_summary);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  m_save = buttons->button(QDialogButtonBox::Save); m_save->setText(tr("开始导出"));
  m_previewTimer = new QTimer(this); m_previewTimer->setSingleShot(true); m_previewTimer->setInterval(200);
  connect(m_previewTimer, &QTimer::timeout, this, [this] {
    auto preview = request(); preview.previewOnly = true;
    const auto generation = m_generation;
    QString error;
    m_previewTask = startDataExport(m_catalog, m_tasks, preview, this, [this, generation](const paleo::exporting::Result &result) {
      if (generation != m_generation) return;
      m_ready = !result.cancelled && result.error.isEmpty() && result.succeeded > 0;
      m_summary->setText(tr("已检查：%1 项数据，源文件约 %2 MB；%3 项问题。\n%4")
          .arg(result.succeeded).arg(result.plannedBytes / 1024. / 1024., 0, 'f', 1).arg(result.failures.size())
          .arg(result.error + result.failures.mid(0, 3).join("\n")));
      m_summary->setToolTip(result.failures.join("\n"));
      updateAvailability();
    }, &error);
    if (!m_previewTask) { m_summary->setText(error); updateAvailability(); }
  });
  connect(m_directory, &QLineEdit::textChanged, this, &DataExportDialog::updateAvailability);
  connect(m_scope, &QComboBox::currentIndexChanged, this, [this] { refreshVersions(); schedulePreview(); });
  connect(m_format, &QComboBox::currentIndexChanged, this, &DataExportDialog::schedulePreview);
  connect(m_references, &QCheckBox::toggled, this, &DataExportDialog::schedulePreview);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons); refreshVersions(); schedulePreview(); resize(680, 460);
}
DataExportDialog::~DataExportDialog() { if (m_previewTask && m_previewTask->running()) m_previewTask->requestCancel(); }
void DataExportDialog::updateAvailability() { m_save->setEnabled(m_ready && !m_directory->text().isEmpty()); }
void DataExportDialog::schedulePreview() {
  ++m_generation; m_ready = false;
  if (m_previewTask && m_previewTask->running()) m_previewTask->requestCancel();
  m_summary->setText(tr("正在检查所选版本、制作来源和文件大小…")); updateAvailability();
  m_previewTimer->start();
}
void DataExportDialog::refreshVersions() {
  const bool original = m_scope->currentIndex() == 1;
  m_format->setEnabled(!original);
  m_references->setEnabled(m_scope->currentIndex() == 2);
  for (int row = 0; row < m_assetIds.size(); ++row) {
    const auto asset = m_catalog->assetById(m_assetIds[row]);
    auto *name = new QTableWidgetItem(asset.displayName); name->setFlags(name->flags() & ~Qt::ItemIsEditable);
    m_versions->setItem(row, 0, name);
    auto *combo = new QComboBox(m_versions);
    const auto versions = m_catalog->versionsForAsset(asset.id);
    const auto current = m_catalog->currentVersion(asset.id);
    for (const auto &version : versions) {
      if (original && version.stage != "RAW") continue;
      combo->addItem(tr("v%1 · %2 · %3").arg(version.versionNumber).arg(version.stage, version.id), version.id);
    }
    const int at = combo->findData(current.id);
    combo->setCurrentIndex(at >= 0 ? at : combo->count() - 1);
    connect(combo, &QComboBox::currentIndexChanged, this, &DataExportDialog::schedulePreview);
    m_versions->setCellWidget(row, 1, combo);
  }
}
paleo::exporting::DataRequest DataExportDialog::request() const {
  using namespace paleo::exporting;
  DataRequest result;
  result.scope = m_scope->currentIndex() == 1 ? Scope::Original : m_scope->currentIndex() == 2 ? Scope::Sources : Scope::Current;
  result.format = result.scope == Scope::Original ? DataFormat::Native : DataFormat(m_format->currentIndex());
  result.includeReferences = m_references->isEnabled() && m_references->isChecked();
  result.destination = QDir(m_directory->text()).filePath("paleo_export_" +
      QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8));
  for (int row = 0; row < m_versions->rowCount(); ++row)
    result.versionIds << static_cast<QComboBox *>(m_versions->cellWidget(row, 1))->currentData().toString();
  return result;
}
