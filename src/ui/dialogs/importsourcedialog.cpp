// 层：视图
#include "importsourcedialog.h"
#include "../paleotheme.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

ImportSourceDialog::ImportSourceDialog(const QString &kind, QWidget *parent)
  : QDialog(parent), m_kind(kind)
{
  setObjectName(QStringLiteral("dataImportSourceDialog")); setProperty("importKind", kind); resize(680, 430);
  const QMap<QString, QString> titles{{"wells", tr("导入井数据")}, {"well_log", tr("导入测井数据")},
      {"seismic", tr("导入地震数据")}, {"boundary", tr("导入边界数据")}, {"files", tr("导入资料文件")},
      {"folder", tr("导入工区文件夹")}, {"paleomap", tr("导入现有古地理图")}, {"paleomap_folder", tr("导入古地理图文件夹")}};
  setWindowTitle(titles.value(kind, tr("导入资料")));
  auto *layout = new QVBoxLayout(this); const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingMd, tokens.spacingMd, tokens.spacingMd, tokens.spacingMd); layout->setSpacing(tokens.spacingSm);
  const QMap<QString, QString> hints{{"wells", tr("选择井位、分层、时深或轨迹资料，支持 XLSX、文本表和点矢量。可多选文件，也可选择井资料目录。")},
      {"well_log", tr("选择 LAS、DLIS、LIS、XML 或文本测井资料，也可选择测井目录。下一步核对识别类型及井归属。")},
      {"seismic", tr("选择 SEG-Y 文件或地震目录。地震体保持外部链接，确认后在后台建立索引。")},
      {"boundary", tr("选择测区范围面矢量。SHP 须与 SHX、DBF 等配套文件放在同一目录，下一步核对后生成范围图层。")},
      {"paleomap", tr("选择现有古地理图，或切换到目录模式。完整保存原图与配套文件，下一步设置层位与三级相整理。")},
      {"paleomap_folder", tr("选择古地理图目录。SHP 按一套图件处理，下一步设置层位归属与三级相整理。")}};
  auto *hint = new QLabel(hints.value(kind, tr("先选择资料来源，再预览和确认导入内容。取消不会导入任何资料。")), this);
  hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); layout->addWidget(hint);
  auto *form = new QFormLayout;
  m_mode = new QComboBox(this); m_mode->setObjectName(QStringLiteral("importSourceMode"));
  m_mode->addItems({tr("选择文件（可多选）"), tr("选择文件夹（包含子目录）")}); form->addRow(tr("资料来源"), m_mode); layout->addLayout(form);
  m_pages = new QStackedWidget(this);
  auto *filePage = new QWidget(this); auto *fileLayout = new QVBoxLayout(filePage); fileLayout->setContentsMargins(0, 0, 0, 0);
  m_files = new QListWidget(filePage); m_files->setObjectName(QStringLiteral("importSourceFileList")); m_files->setSelectionMode(QAbstractItemView::ExtendedSelection); fileLayout->addWidget(m_files, 1);
  auto *fileButtons = new QHBoxLayout;
  auto *add = new QPushButton(tr("选择文件…"), filePage); add->setObjectName(QStringLiteral("importSourceAddFiles")); fileButtons->addWidget(add);
  auto *remove = new QPushButton(tr("移除所选"), filePage); remove->setObjectName(QStringLiteral("importSourceRemoveFiles")); fileButtons->addWidget(remove); fileButtons->addStretch(); fileLayout->addLayout(fileButtons);
  m_pages->addWidget(filePage);
  auto *folderPage = new QWidget(this); auto *folderLayout = new QVBoxLayout(folderPage); folderLayout->setContentsMargins(0, 0, 0, 0);
  auto *folderRow = new QHBoxLayout;
  m_folder = new QLineEdit(folderPage); m_folder->setObjectName(QStringLiteral("importSourceFolderEdit")); m_folder->setPlaceholderText(tr("选择目录或粘贴目录路径")); folderRow->addWidget(m_folder, 1);
  auto *browse = new QPushButton(tr("选择文件夹…"), folderPage); browse->setObjectName(QStringLiteral("importSourceBrowseFolder")); folderRow->addWidget(browse); folderLayout->addLayout(folderRow);
  auto *recursive = new QLabel(tr("将递归识别目录中的资料，下一步逐项核对；未确认前不开始导入。"), folderPage); recursive->setWordWrap(true); folderLayout->addWidget(recursive); folderLayout->addStretch(); m_pages->addWidget(folderPage);
  layout->addWidget(m_pages, 1);
  m_count = new QLabel(this); m_count->setObjectName(QStringLiteral("importSourceCount")); layout->addWidget(m_count);
  auto *note = new QLabel(kind.startsWith("paleomap") ? tr("原图与配套文件整套保存；可另外生成三级相版本。")
      : tr("下一步可核对类型、显示名称与导入方式；默认建立索引，也可选择复制进工程。"), this); note->setWordWrap(true); layout->addWidget(note);
  m_error = new QLabel(this); m_error->setObjectName(QStringLiteral("importSourceError")); m_error->setWordWrap(true); m_error->setTextFormat(Qt::PlainText); layout->addWidget(m_error);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  m_confirm = buttons->button(QDialogButtonBox::Ok); m_confirm->setText(tr("下一步：核对")); m_confirm->setObjectName(QStringLiteral("importSourceConfirm"));
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消")); layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &ImportSourceDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int index) { m_pages->setCurrentIndex(index); refresh(); });
  connect(m_folder, &QLineEdit::textChanged, this, &ImportSourceDialog::refresh);
  connect(add, &QPushButton::clicked, this, [this] {
    const auto picked = QFileDialog::getOpenFileNames(this, windowTitle(), {}, fileFilter());
    if (picked.isEmpty()) return;
    setSources(files() + picked);
  });
  connect(remove, &QPushButton::clicked, this, [this] { qDeleteAll(m_files->selectedItems()); refresh(); });
  connect(browse, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, windowTitle(), m_folder->text());
    if (!path.isEmpty()) m_folder->setText(QDir::fromNativeSeparators(path));
  });
  m_mode->setCurrentIndex(kind == "folder" || kind == "paleomap_folder" ? 1 : 0); refresh();
}
QString ImportSourceDialog::fileFilter() const
{
  if (m_kind == "seismic") return tr("地震数据 (*.sgy *.segy);;所有文件 (*)");
  if (m_kind == "well_log") return tr("测井与柱状图 (*.las *.dlis *.lis *.xml *.txt *.csv *.tsv);;所有文件 (*)");
  if (m_kind == "wells") return tr("井位与井资料 (*.xlsx *.txt *.csv *.tsv *.dat *.las *.dlis *.lis *.xml *.gpkg *.shp *.geojson);;所有文件 (*)");
  if (m_kind == "boundary") return tr("测区范围面矢量 (*.shp *.gpkg *.geojson);;所有文件 (*)");
  if (m_kind.startsWith("paleomap")) return tr("古地理图 (*.shp *.gpkg *.geojson *.tif *.tiff *.img *.png *.jpg *.jpeg *.pdf *.svg);;所有文件 (*)");
  return tr("资料文件 (*.xlsx *.pdf *.ppt *.pptx *.doc *.docx *.png *.jpg *.jpeg *.tif *.tiff *.las *.dlis *.lis *.sgy *.segy *.xml *.txt *.csv *.tsv *.dat *.gpkg *.shp *.geojson);;所有文件 (*)");
}
QString ImportSourceDialog::preferredType(const QString &kind, const QString &path)
{
  const auto extension = QFileInfo(path).suffix().toLower();
  if (kind == "boundary" && QStringList{"shp", "gpkg", "geojson"}.contains(extension)) return QStringLiteral("boundary");
  if (kind == "seismic" && QStringList{"sgy", "segy"}.contains(extension)) return QStringLiteral("seismic");
  return {};
}
QStringList ImportSourceDialog::files() const
{
  QStringList paths; if (m_mode->currentIndex() != 0) return paths;
  for (int row = 0; row < m_files->count(); ++row) paths << m_files->item(row)->data(Qt::UserRole).toString();
  return paths;
}
QString ImportSourceDialog::folder() const { return m_mode->currentIndex() == 1 ? QDir::fromNativeSeparators(m_folder->text().trimmed()) : QString(); }
void ImportSourceDialog::setSources(const QStringList &paths, bool directory)
{
  m_mode->setCurrentIndex(directory ? 1 : 0);
  if (directory) m_folder->setText(paths.value(0));
  else {
    m_files->clear(); QStringList unique;
    for (const auto &path : paths) { const auto clean = QDir::fromNativeSeparators(path); if (!unique.contains(clean)) unique << clean; }
    for (const auto &path : unique) { auto *item = new QListWidgetItem(QFileInfo(path).fileName(), m_files); item->setData(Qt::UserRole, path); item->setToolTip(path); }
  }
  refresh();
}
void ImportSourceDialog::refresh()
{
  m_error->clear(); const bool directory = m_mode->currentIndex() == 1;
  m_count->setText(directory ? tr("确认后预览目录中的资料。") : tr("已选择 %1 个文件").arg(m_files->count()));
  m_confirm->setEnabled(directory ? !folder().isEmpty() : m_files->count() > 0);
}
void ImportSourceDialog::accept()
{
  const auto paths = folder().isEmpty() ? files() : QStringList{folder()};
  if (paths.isEmpty()) { m_error->setText(tr("请先选择资料文件或文件夹。")); return; }
  for (const auto &path : paths) if (folder().isEmpty() ? !QFileInfo(path).isFile() : !QFileInfo(path).isDir()) {
    m_error->setText(tr("资料路径不存在或类型不符：%1").arg(path)); return;
  }
  QDialog::accept();
}
