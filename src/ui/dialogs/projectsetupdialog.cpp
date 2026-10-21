// 层：视图
#include "projectsetupdialog.h"
#include "../../workflow/projectsetup.h"
#include "../paleotheme.h"
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QTableWidget>
#include <QVBoxLayout>

ProjectSetupDialog::ProjectSetupDialog(ProjectSetupWorkflow *workflow, QWidget *parent)
  : QDialog(parent), m_workflow(workflow)
{
  setObjectName(QStringLiteral("projectSetupDialog")); setWindowTitle(tr("新建工程")); resize(840, 650);
  auto *layout = new QVBoxLayout(this);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingMd, tokens.spacingMd, tokens.spacingMd, tokens.spacingMd); layout->setSpacing(tokens.spacingSm);
  m_steps = new QTabBar(this);
  for (const auto &title : {tr("1 工程与来源"), tr("2 井点"), tr("3 其他资料"), tr("4 确认创建")}) m_steps->addTab(title);
  m_steps->setExpanding(true); layout->addWidget(m_steps);
  m_pages = new QStackedWidget(this);
  m_pages->addWidget(projectPage()); m_pages->addWidget(pointPage());
  m_pages->addWidget(sourcesPage()); m_pages->addWidget(reviewPage()); layout->addWidget(m_pages, 1);
  m_status = new QLabel(this); m_status->setWordWrap(true); m_status->setObjectName(QStringLiteral("projectSetupStatus")); layout->addWidget(m_status);
  auto *buttons = new QHBoxLayout;
  auto *empty = new QPushButton(tr("创建空白工程"), this); empty->setObjectName(QStringLiteral("createEmptyProjectButton")); buttons->addWidget(empty);
  auto *review = new QPushButton(tr("直接核对"), this); buttons->addWidget(review); buttons->addStretch();
  auto *cancel = new QPushButton(tr("取消"), this); buttons->addWidget(cancel);
  m_previous = new QPushButton(tr("上一步"), this); buttons->addWidget(m_previous);
  m_next = new QPushButton(tr("下一步"), this); m_next->setObjectName(QStringLiteral("projectSetupNextButton")); m_next->setDefault(true); buttons->addWidget(m_next);
  layout->addLayout(buttons);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(m_steps, &QTabBar::currentChanged, this, &ProjectSetupDialog::showStep);
  connect(m_previous, &QPushButton::clicked, this, [this] { m_steps->setCurrentIndex(m_steps->currentIndex() - 1); });
  connect(review, &QPushButton::clicked, this, [this] { m_steps->setCurrentIndex(3); });
  connect(m_next, &QPushButton::clicked, this, [this] { if (m_steps->currentIndex() == 3) finish(); else m_steps->setCurrentIndex(m_steps->currentIndex() + 1); });
  connect(empty, &QPushButton::clicked, this, [this] { m_empty = true; finish(); });
  connect(m_workflow, &ProjectSetupWorkflow::previewReady, this, &ProjectSetupDialog::showPreview);
  for (auto *edit : {m_points, m_wells, m_seismic, m_horizons, m_references, m_boundary, m_crs})
    connect(edit, &QLineEdit::textEdited, this, [this] { m_dirty = true; ++m_revision; });
  showStep(0);
}
QWidget *ProjectSetupDialog::pathRow(QLineEdit *edit, bool directory, const QString &filter)
{
  auto *row = new QWidget(this); auto *layout = new QHBoxLayout(row); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(8); layout->addWidget(edit);
  auto *button = new QPushButton(tr("浏览…"), row); layout->addWidget(button);
  connect(button, &QPushButton::clicked, this, [this, edit, directory, filter] {
    const QString path = directory ? QFileDialog::getExistingDirectory(this, tr("选择文件夹"), edit->text())
                                  : QFileDialog::getOpenFileName(this, tr("选择资料"), edit->text(), filter);
    if (path.isEmpty()) return;
    edit->setText(QDir::fromNativeSeparators(path)); m_dirty = true; ++m_revision;
    if (edit == m_root) discover();
    if (edit == m_points) { m_lastPointFile = path; for (auto *box : m_fields) box->clear(); m_crs->clear(); scan(); }
  });
  return row;
}
QWidget *ProjectSetupDialog::projectPage()
{
  auto *page = new QWidget(this); auto *layout = new QVBoxLayout(page);
  auto *text = new QLabel(tr("选择资料总目录可自动填写各项；全部资料均可留空，创建后可继续导入。"), page); text->setWordWrap(true); layout->addWidget(text);
  auto *form = new QFormLayout;
  m_name = new QLineEdit(tr("未命名工程"), page); m_name->setObjectName(QStringLiteral("projectNameEdit"));
  m_output = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Paleo"), page); m_output->setObjectName(QStringLiteral("projectDirectoryEdit"));
  m_root = new QLineEdit(page); m_root->setObjectName(QStringLiteral("projectSourceRootEdit")); m_root->setPlaceholderText(tr("可选：选择井、地震、层位等资料所在的总目录"));
  form->addRow(tr("工程名称"), m_name); form->addRow(tr("工程保存位置"), pathRow(m_output, true)); form->addRow(tr("资料总目录（可选）"), pathRow(m_root, true));
  layout->addLayout(form);
  auto *note = new QLabel(tr("将在保存位置下新建同名工程文件夹。原资料保留，工程保存导入副本及生成图层；地震体登记外部链接。"), page); note->setWordWrap(true); layout->addWidget(note); layout->addStretch();
  connect(m_root, &QLineEdit::editingFinished, this, &ProjectSetupDialog::discover);
  return page;
}
QWidget *ProjectSetupDialog::pointPage()
{
  auto *page = new QWidget(this); auto *layout = new QVBoxLayout(page); auto *form = new QFormLayout;
  m_points = new QLineEdit(page); m_points->setObjectName(QStringLiteral("projectPointsEdit")); m_points->setPlaceholderText(tr("可选：井位表或点矢量；井名和位置自动识别"));
  connect(m_points, &QLineEdit::editingFinished, this, [this] {
    if (m_lastPointFile == m_points->text().trimmed()) return;
    m_lastPointFile = m_points->text().trimmed();
    for (auto *box : m_fields) box->clear(); m_crs->clear(); m_dirty = true; ++m_revision; scan();
  });
  form->addRow(tr("井点资料"), pathRow(m_points, false, tr("井位表和矢量 (*.xlsx *.csv *.tsv *.txt *.dat *.gpkg *.shp *.geojson);;所有文件 (*)")));
  const QList<QPair<QString, QString>> fields{{"sheet", tr("工作表／图层")}, {"well", tr("井名字段")}, {"x", tr("横坐标／经度")}, {"y", tr("纵坐标／纬度")}, {"kb", tr("补心海拔（可选）")}, {"td", tr("完钻井深（可选）")}};
  for (const auto &field : fields) {
    auto *box = new QComboBox(page); box->setObjectName(QStringLiteral("pointField_%1").arg(field.first)); m_fields.insert(field.first, box); form->addRow(field.second, box);
    connect(box, &QComboBox::activated, this, [this, key = field.first] { m_dirty = true; ++m_revision; if (key == QLatin1String("sheet")) { for (auto it = m_fields.begin(); it != m_fields.end(); ++it) if (it.key() != QLatin1String("sheet")) it.value()->clear(); scan(); } });
  }
  m_crs = new QLineEdit(page); m_crs->setPlaceholderText(tr("自动识别；也可输入 EPSG:32649 等，未知时留空")); form->addRow(tr("资料坐标系"), m_crs); layout->addLayout(form);
  auto *scanButton = new QPushButton(tr("识别与预览"), page); connect(scanButton, &QPushButton::clicked, this, &ProjectSetupDialog::scan); layout->addWidget(scanButton);
  m_pointPreview = new QTableWidget(0, 3, page); m_pointPreview->setHorizontalHeaderLabels({tr("井名"), tr("横坐标／经度"), tr("纵坐标／纬度")}); m_pointPreview->setEditTriggers(QAbstractItemView::NoEditTriggers); m_pointPreview->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); layout->addWidget(m_pointPreview, 1);
  return page;
}
QWidget *ProjectSetupDialog::sourcesPage()
{
  auto *page = new QWidget(this); auto *layout = new QVBoxLayout(page); auto *form = new QFormLayout;
  m_wells = new QLineEdit(page); m_seismic = new QLineEdit(page); m_horizons = new QLineEdit(page); m_references = new QLineEdit(page); m_boundary = new QLineEdit(page);
  const QList<QPair<QString, QLineEdit *>> rows{{tr("井数据文件夹"), m_wells}, {tr("地震文件夹"), m_seismic}, {tr("层位文件夹"), m_horizons}, {tr("参考资料文件夹"), m_references}};
  for (const auto &row : rows) { row.second->setPlaceholderText(tr("可选，留空即可跳过")); form->addRow(row.first, pathRow(row.second, true)); }
  m_boundary->setPlaceholderText(tr("可选；未提供时，使用地震数据生成覆盖范围"));
  form->addRow(tr("测区范围矢量"), pathRow(m_boundary, false, tr("面矢量 (*.gpkg *.shp *.geojson);;所有文件 (*)"))); layout->addLayout(form);
  auto *note = new QLabel(tr("自动按井名关联测井、时深、录井及照片；井分层表与地震层位面分别管理。空文件、旧工程和完全重复的分层记录会在确认页说明。"), page); note->setWordWrap(true); layout->addWidget(note); layout->addStretch();
  return page;
}
QWidget *ProjectSetupDialog::reviewPage()
{
  auto *page = new QWidget(this); auto *layout = new QVBoxLayout(page);
  auto *text = new QLabel(tr("核对将要建立的工程内容。资料导入与地震索引在后台任务中执行，完成后自动生成井点和范围图层。"), page); text->setWordWrap(true); layout->addWidget(text);
  m_assignments = new QTableWidget(0, 3, page); m_assignments->setObjectName(QStringLiteral("projectWellAssignments"));
  m_assignments->setHorizontalHeaderLabels({tr("井名冲突的文件"), tr("文件内部井名"), tr("确认导入归属")});
  m_assignments->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_assignments->setEditTriggers(QAbstractItemView::NoEditTriggers); m_assignments->hide();
  layout->addWidget(m_assignments);
  m_review = new QPlainTextEdit(page); m_review->setReadOnly(true); m_review->setObjectName(QStringLiteral("projectSetupReview")); layout->addWidget(m_review, 1); return page;
}
ProjectSetupSources ProjectSetupDialog::sources() const
{
  if (m_empty) return {};
  ProjectSetupSources s; s.root = m_root->text().trimmed(); s.points = m_points->text().trimmed(); s.wells = m_wells->text().trimmed(); s.seismic = m_seismic->text().trimmed(); s.horizons = m_horizons->text().trimmed(); s.references = m_references->text().trimmed(); s.boundary = m_boundary->text().trimmed();
  for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) if (!it.value()->currentText().isEmpty()) s.pointMapping.insert(it.key(), it.value()->currentText());
  if (s.pointMapping.isEmpty()) s.pointMapping = m_initialMapping;
  s.wellTargets = m_preview.wellTargets;
  s.pointMapping.insert("crs", m_crs->text().trimmed()); return s;
}
QString ProjectSetupDialog::projectPath() const
{
  if (!m_existingPath.isEmpty()) return m_existingPath;
  const auto name = m_name->text().trimmed();
  return QDir(m_output->text().trimmed()).filePath(name + '/' + name + QStringLiteral(".qgz"));
}
void ProjectSetupDialog::discover()
{
  if (m_root->text().trimmed().isEmpty()) return;
  if (m_discoveredRoot == m_root->text().trimmed()) return;
  m_discoveredRoot = m_root->text().trimmed();
  const auto s = ProjectSetupWorkflow::discover(m_discoveredRoot);
  m_points->setText(s.points); m_wells->setText(s.wells); m_seismic->setText(s.seismic); m_horizons->setText(s.horizons); m_references->setText(s.references);
  m_lastPointFile = s.points;
  for (auto *box : m_fields) box->clear(); m_crs->clear();
  if (m_name->text() == tr("未命名工程")) m_name->setText(QFileInfo(s.root).fileName());
  m_dirty = true; ++m_revision; scan();
}
void ProjectSetupDialog::setSourceRoot(const QString &root) { m_root->setText(root); discover(); }
void ProjectSetupDialog::setExistingProject(const QString &path, const ProjectSetupSources &s)
{
  m_existingPath = path; setWindowTitle(tr("工程资料设置"));
  m_name->setText(QFileInfo(path).completeBaseName()); m_name->setEnabled(false);
  m_output->setText(QFileInfo(path).absolutePath()); m_output->setEnabled(false);
  findChild<QPushButton *>(QStringLiteral("createEmptyProjectButton"))->hide();
  m_discoveredRoot = s.root; m_root->setText(s.root);
  m_points->setText(s.points); m_lastPointFile = s.points; m_wells->setText(s.wells); m_seismic->setText(s.seismic);
  m_horizons->setText(s.horizons); m_references->setText(s.references); m_boundary->setText(s.boundary);
  m_initialMapping = s.pointMapping; m_preview.wellTargets = s.wellTargets;
  m_crs->setText(s.pointMapping.value("crs").toString()); scan();
}
void ProjectSetupDialog::scan()
{
  m_scanRevision = m_revision;
  m_empty = false; m_busy = true; m_next->setEnabled(false); m_status->setText(tr("正在识别资料，完成前可继续修改或取消。"));
  m_workflow->scan(sources());
}
void ProjectSetupDialog::showPreview(const ProjectSetupPreview &p)
{
  if (m_scanRevision != m_revision) { scan(); return; }
  m_preview = p; m_busy = false; m_dirty = false;
  m_assignments->setRowCount(p.logWellNames.size()); m_assignments->setVisible(!p.logWellNames.isEmpty());
  m_assignments->setMaximumHeight(m_assignments->horizontalHeader()->sizeHint().height() +
      qMin(4, m_assignments->rowCount()) * m_assignments->verticalHeader()->defaultSectionSize() +
      2 * m_assignments->frameWidth() + PaleoTheme::tokens().spacingSm);
  int assignmentRow = 0;
  for (auto it = p.logWellNames.cbegin(); it != p.logWellNames.cend(); ++it, ++assignmentRow) {
    const QString path = it.key();
    m_assignments->setItem(assignmentRow, 0, new QTableWidgetItem(QFileInfo(path).fileName()));
    m_assignments->setItem(assignmentRow, 1, new QTableWidgetItem(it.value()));
    auto *choice = new QComboBox(m_assignments);
    choice->addItem(tr("保留内部井名 %1").arg(it.value()), QString());
    for (const auto &well : p.knownWells) choice->addItem(tr("关联到 %1").arg(well), QStringLiteral("name:") + well);
    choice->setCurrentIndex(qMax(0, choice->findData(p.wellTargets.value(path))));
    m_assignments->setCellWidget(assignmentRow, 2, choice);
    connect(choice, &QComboBox::activated, this, [this, path, choice] { m_preview.wellTargets.insert(path, choice->currentData().toString()); });
  }
  m_initialMapping.clear();
  for (auto it = m_fields.begin(); it != m_fields.end(); ++it) {
    QSignalBlocker block(it.value()); it.value()->clear();
    if (it.key() == QLatin1String("sheet")) it.value()->addItems(p.sheets);
    else { it.value()->addItem(QString()); it.value()->addItems(p.columns); }
    it.value()->setCurrentText(p.pointMapping.value(it.key()).toString());
  }
  m_crs->setText(p.sourceCrs); m_pointPreview->setRowCount(p.pointRows.size());
  for (int row = 0; row < p.pointRows.size(); ++row) for (int col = 0; col < 3; ++col) m_pointPreview->setItem(row, col, new QTableWidgetItem(p.pointRows[row].value(col)));
  QString text = tr("将建立：%1 口井、%2 条井分层、%3 份测井、%4 个地震文件、%5 个层位面、%6 份其他资料。\n生成井点、井名标注和有效地震覆盖范围，并随工程保存。\n共 %7 个文件。\n").arg(p.wells).arg(p.tops).arg(p.logs).arg(p.seismic).arg(p.horizons).arg(p.references).arg(p.paths.size());
  if (!p.sourceCrs.isEmpty()) text += tr("资料坐标系：%1\n").arg(p.sourceCrs);
  if (!p.errors.isEmpty()) text += tr("\n需要调整（也可清空对应来源后跳过）：\n") + p.errors.join('\n');
  if (!p.notes.isEmpty()) text += tr("\n识别说明：\n") + p.notes.join('\n');
  m_review->setPlainText(text); m_status->setText(p.errors.isEmpty() ? tr("识别完成；可继续设置或直接核对创建。") : tr("部分资料需要调整，详细原因见确认页。")); m_next->setEnabled(true);
}
void ProjectSetupDialog::showStep(int step)
{
  m_pages->setCurrentIndex(step); m_previous->setEnabled(step > 0); m_next->setText(step == 3 ? (m_existingPath.isEmpty() ? tr("创建并导入") : tr("保存设置并导入")) : tr("下一步（本步可跳过）"));
  if ((step == 1 || step == 3) && m_dirty) scan();
}
void ProjectSetupDialog::finish()
{
  const QString name = m_name->text().trimmed();
  if (name.isEmpty() || name == "." || name == ".." || name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))) || m_output->text().trimmed().isEmpty()) { m_empty = false; m_status->setText(tr("请填写有效的工程名称和保存位置。")); return; }
  if (!m_empty && (m_busy || m_dirty)) { m_status->setText(tr("请等待本次识别完成后再创建。")); if (!m_busy) scan(); return; }
  if (!m_empty && !m_preview.errors.isEmpty()) { m_status->setText(tr("请调整有问题的资料来源，或将其清空后重新识别。")); return; }
  accept();
}
