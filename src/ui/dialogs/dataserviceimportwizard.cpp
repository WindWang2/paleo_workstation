// 层：视图
#include "dataserviceimportwizard.h"
#include "../../workflow/projectsetup.h"
#include "../../qgis/crsselector.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>

DataServiceImportWizard::DataServiceImportWizard(ProjectSetupWorkflow *workflow, QWidget *parent)
  : QWizard(parent), m_workflow(workflow)
{
  setWindowTitle(tr("新建 Data_service 工区"));
  setWizardStyle(QWizard::ModernStyle);
  setOption(QWizard::NoBackButtonOnStartPage, true);
  setOption(QWizard::HaveHelpButton, false);
  setMinimumSize(720, 420);
  addPage(projectPage()); addPage(settingsPage()); addPage(reviewPage());
  setButtonText(QWizard::FinishButton, tr("创建并导入"));
  if (auto *create = button(QWizard::FinishButton)) create->setObjectName(QStringLiteral("dataServiceCreateButton"));
  connect(this, &QWizard::currentIdChanged, this, &DataServiceImportWizard::refreshReview);
  refreshReview();
}

QWizardPage *DataServiceImportWizard::projectPage()
{
  auto *page = new QWizardPage(this);
  page->setTitle(tr("工程与资料"));
  page->setSubTitle(tr("直接建立 Data_service 工区模型，原始文件默认只引用、不复制。"));
  auto *layout = new QVBoxLayout(page);
  auto *form = new QFormLayout;
  m_name = new QLineEdit(tr("未命名工程"), page); m_name->setObjectName(QStringLiteral("dataServiceProjectNameEdit"));
  m_output = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Paleo"), page);
  m_output->setObjectName(QStringLiteral("dataServiceProjectDirectoryEdit"));
  m_root = new QLineEdit(page); m_root->setObjectName(QStringLiteral("dataServiceSourceRootEdit"));
  m_root->setPlaceholderText(tr("选择需要导入的资料目录，无需按固定目录结构整理"));
  form->addRow(tr("工程名称"), m_name);
  form->addRow(tr("工程保存位置"), pathRow(m_output, true));
  form->addRow(tr("资料总目录"), pathRow(m_root, true));
  layout->addLayout(form);
  m_points = new QLineEdit(page); m_points->setObjectName(QStringLiteral("dataServicePointFileEdit"));
  m_points->setPlaceholderText(tr("可选：井位坐标表；留空由数据模型自动识别"));
  form->addRow(tr("井位坐标表"), pathRow(m_points, false));
  m_status = new QLabel(page); m_status->setObjectName(QStringLiteral("dataServiceWizardStatus"));
  m_status->setWordWrap(true); layout->addWidget(m_status); layout->addStretch(1);
  return page;
}

QWizardPage *DataServiceImportWizard::settingsPage()
{
  auto *page = new QWizardPage(this);
  page->setTitle(tr("坐标与导入设置"));
  page->setSubTitle(tr("可以暂不设置，先建立数据模型；工程计算基准须为米制投影坐标系。"));
  auto *form = new QFormLayout(page);
  m_crs = new PaleoCrsSelector(true, page); m_crs->setObjectName(QStringLiteral("dataServiceCrsEdit"));
  m_crs->setNotSetText(tr("暂不设置，先建立数据模型"));
  m_crsUnit = new QComboBox(page); m_crsUnit->setObjectName(QStringLiteral("dataServiceCrsUnitCombo"));
  m_crsUnit->addItem(QStringLiteral("m")); m_crsUnit->setPlaceholderText(tr("未设置"));
  m_crsUnit->setEnabled(false);
  form->addRow(tr("工程坐标系"), m_crs->widget()); form->addRow(tr("坐标单位"), m_crsUnit);
  m_threads = new QComboBox(page); m_threads->setObjectName(QStringLiteral("dataServiceThreadsCombo"));
  m_threads->addItem(QStringLiteral("1"), 1); m_threads->addItem(QStringLiteral("2"), 2); m_threads->addItem(QStringLiteral("4"), 4);
  m_threads->setCurrentIndex(2);
  m_pending = new QComboBox(page); m_pending->setObjectName(QStringLiteral("dataServicePendingCombo"));
  m_pending->addItem(QStringLiteral("256 MiB"), 256); m_pending->addItem(QStringLiteral("512 MiB"), 512); m_pending->addItem(QStringLiteral("1024 MiB"), 1024);
  form->addRow(tr("并行解析线程"), m_threads); form->addRow(tr("待解析内存预算"), m_pending);
  auto *mappingNote = new QLabel(tr("仅在井位表使用自定义列名时填写；默认使用模型识别的井号、X、Y 表头。"), page);
  mappingNote->setWordWrap(true); form->addRow(mappingNote);
  m_wellField = new QLineEdit(page); m_wellField->setObjectName(QStringLiteral("dataServiceWellColumnEdit"));
  m_xField = new QLineEdit(page); m_xField->setObjectName(QStringLiteral("dataServiceXColumnEdit"));
  m_yField = new QLineEdit(page); m_yField->setObjectName(QStringLiteral("dataServiceYColumnEdit"));
  form->addRow(tr("井名列"), m_wellField); form->addRow(tr("X 列"), m_xField); form->addRow(tr("Y 列"), m_yField);
  m_coordinateStatus = new QLabel(page); m_coordinateStatus->setObjectName(QStringLiteral("dataServiceCoordinateStatus"));
  m_coordinateStatus->setWordWrap(true); form->addRow(m_coordinateStatus);
  connect(m_crs, &PaleoCrsSelector::definitionChanged, this, [this](const QString &) {
    const auto crs = m_crs->crs();
    m_crsUnit->setCurrentIndex(crs.isValid() && !crs.isGeographic() && crs.mapUnits() == Qgis::DistanceUnit::Meters ? 0 : -1);
    m_coordinateStatus->clear();
    refreshReview();
  });
  connect(m_threads, &QComboBox::currentTextChanged, this, &DataServiceImportWizard::refreshReview);
  connect(m_pending, &QComboBox::currentTextChanged, this, &DataServiceImportWizard::refreshReview);
  m_crs->clear();
  m_crsUnit->setCurrentIndex(-1);
  return page;
}

QWizardPage *DataServiceImportWizard::reviewPage()
{
  auto *page = new QWizardPage(this);
  page->setTitle(tr("确认创建"));
  page->setSubTitle(tr("创建后先建立数据模型，再由模型生成目录索引，不执行第二遍文件导入。"));
  auto *layout = new QVBoxLayout(page);
  m_review = new QLabel(page); m_review->setObjectName(QStringLiteral("dataServiceReview"));
  m_review->setWordWrap(true); m_review->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  m_reviewStatus = new QLabel(page); m_reviewStatus->setObjectName(QStringLiteral("dataServiceCreationStatus"));
  m_reviewStatus->setWordWrap(true);
  layout->addWidget(m_review); layout->addWidget(m_reviewStatus); layout->addStretch(1);
  return page;
}

QWidget *DataServiceImportWizard::pathRow(QLineEdit *edit, bool directory)
{
  auto *host = new QWidget(this); auto *layout = new QHBoxLayout(host);
  layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(edit, 1);
  auto *browse = new QPushButton(tr("浏览…"), host);
  connect(browse, &QPushButton::clicked, this, [this, edit, directory] {
    const QString current = edit->text().trimmed();
    const QString path = directory ? QFileDialog::getExistingDirectory(this, tr("选择目录"), current)
                                   : QFileDialog::getOpenFileName(this, tr("选择井位坐标表"), current,
                                         tr("表格文件 (*.csv *.txt *.xls *.xlsx *.xml)"));
    if (!path.isEmpty()) edit->setText(path);
  });
  layout->addWidget(browse); return host;
}

ProjectSetupSources DataServiceImportWizard::sources() const
{
  ProjectSetupSources result;
  result.root = m_root->text().trimmed(); result.points = m_points->text().trimmed();
  result.projectCrs = m_crs->text().trimmed();
  result.pointMapping.insert("well", m_wellField->text().trimmed());
  result.pointMapping.insert("x", m_xField->text().trimmed());
  result.pointMapping.insert("y", m_yField->text().trimmed());
  result.pointMapping.insert("crs", result.projectCrs);
  result.pointMapping.insert("unit", result.projectCrs.isEmpty() ? QString() : m_crsUnit->currentText());
  return result;
}

ProjectSetupPreview DataServiceImportWizard::preview() const
{
  ProjectSetupPreview result;
  result.projectCrs = m_crs->text().trimmed();
  result.pointMapping = sources().pointMapping;
  result.notes << tr("由 Data_service 建立工区模型，再直接生成目录索引。");
  return result;
}

QString DataServiceImportWizard::projectPath() const
{
  const auto name = m_name->text().trimmed();
  return QDir(m_output->text().trimmed()).filePath(name + '/' + name + QStringLiteral(".qgz"));
}

void DataServiceImportWizard::setSourceRoot(const QString &root) { m_root->setText(root); refreshReview(); }

void DataServiceImportWizard::showCreationError(const QString &error)
{
  showPageError(2, error);
}

void DataServiceImportWizard::showPageError(int page, const QString &error)
{
  m_status->clear(); m_coordinateStatus->clear(); m_reviewStatus->clear();
  setCurrentId(page);
  (page == 0 ? m_status : page == 1 ? m_coordinateStatus : m_reviewStatus)->setText(error);
}

void DataServiceImportWizard::refreshReview()
{
  if (!m_review) return;
  m_review->setText(tr("工程：%1\n资料目录：%2\n坐标系：%3 [%4]\n解析线程：%5；内存预算：%6 MiB\n\n原始文件只引用、不复制；模型导入完成后直接生成索引。")
      .arg(m_name->text().trimmed(), m_root->text().trimmed(),
           m_crs->text().trimmed().isEmpty() ? tr("未设置（先建立模型，保留资料原有坐标约定）") : m_crs->text().trimmed(),
           m_crs->text().trimmed().isEmpty() ? tr("未设置") : m_crsUnit->currentText(),
           m_threads->currentText(), m_pending->currentText()));
}

bool DataServiceImportWizard::validateProjectInputs()
{
  const auto name = m_name->text().trimmed();
  if (name.isEmpty() || name == "." || name == ".." || name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")))) {
    showPageError(0, tr("请填写有效的工程名称。")); return false;
  }
  if (!QFileInfo(m_root->text().trimmed()).isDir()) { showPageError(0, tr("请选择有效的资料目录。")); return false; }
  if (!m_points->text().trimmed().isEmpty() && !QFileInfo(m_points->text().trimmed()).isFile()) {
    showPageError(0, tr("井位坐标表不存在，请重新选择。")); return false;
  }
  QString error;
  if (!ProjectSetupWorkflow::checkProjectLocation(projectPath(), &error)) { showPageError(0, error); return false; }
  m_status->clear(); return true;
}

bool DataServiceImportWizard::validateCoordinateInputs()
{
  QString error;
  if (!m_crs->validate(&error, true)) {
    showPageError(1, error); m_crs->widget()->setFocus(); return false;
  }
  m_coordinateStatus->clear(); return true;
}

bool DataServiceImportWizard::validateCurrentPage()
{
  if (currentId() == 0) return validateProjectInputs();
  if (currentId() == 1) return validateCoordinateInputs();
  return true;
}

void DataServiceImportWizard::finish()
{
  if (!validateProjectInputs() || !validateCoordinateInputs()) return;
  qputenv("PALEO_IMPORT_PARSER_THREADS", m_threads->currentData().toByteArray());
  qputenv("PALEO_IMPORT_PENDING_MB", m_pending->currentData().toByteArray());
  QWizard::accept();
}

void DataServiceImportWizard::accept()
{
  finish();
}
