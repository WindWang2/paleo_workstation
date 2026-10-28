// 层：视图
#include "projectsetupdialog.h"
#include "../../workflow/projectsetup.h"
#include "../paleotheme.h"
#include "../../domain/mappinghorizons.h"
#include "qgis/crsselector.h"
#include <QCheckBox>
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
  connect(empty, &QPushButton::clicked, this, [this] {
    m_empty = true;
    m_blankLocked = true; // 先锁住，避免 finish/accept 重入事件时在途识别改写意图。
    finish();
    if (result() != QDialog::Accepted) { m_blankLocked = false; m_empty = false; }
  });
  connect(m_workflow, &ProjectSetupWorkflow::previewReady, this, &ProjectSetupDialog::showPreview);
  for (auto *edit : {m_points, m_wells, m_seismic, m_horizons, m_references, m_boundary, m_paleomaps})
    connect(edit, &QLineEdit::textEdited, this, [this] { m_dirty = true; ++m_revision; });
  for (auto *selector : {m_crs, m_projectCrs})
    connect(selector, &PaleoCrsSelector::definitionChanged, this, [this](const QString &) { m_dirty = true; ++m_revision; });
  showStep(0);
}
QWidget *ProjectSetupDialog::pathRow(QLineEdit *edit, bool directory, const QString &filter)
{
  auto *row = new QWidget(this); auto *layout = new QHBoxLayout(row); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(PaleoTheme::tokens().spacingSm); layout->addWidget(edit);
  auto *button = new QPushButton(tr("浏览…"), row); layout->addWidget(button);
  connect(button, &QPushButton::clicked, this, [this, edit, directory, filter] {
    const QString path = directory ? QFileDialog::getExistingDirectory(this, tr("选择文件夹"), edit->text())
                                  : QFileDialog::getOpenFileName(this, tr("选择资料"), edit->text(), filter);
    if (path.isEmpty()) return;
    // 归一化后再回填，m_lastPointFile 与编辑框文本同源——否则 editingFinished
    // 的「文本未变则不重扫」判等因分隔符差异误判为已修改，白扫一次井点。
    const QString normalized = QDir::fromNativeSeparators(path);
    edit->setText(normalized); m_dirty = true; ++m_revision;
    if (edit == m_root) discover();
    if (edit == m_paleomaps) scan();
    if (edit == m_points) { m_lastPointFile = normalized; for (auto *box : m_fields) box->clear(); m_crs->clear(); scan(); }
  });
  return row;
}
QWidget *ProjectSetupDialog::projectPage()
{
  auto *page = new QWidget(this); auto *layout = new QVBoxLayout(page);
  auto *text = new QLabel(tr("选择资料总目录可自动填写各项。支持分井资料目录，也支持“坐标统计表＋测井 XML＋参考资料”的集中组织方式；创建后可继续补充资料。"), page); text->setWordWrap(true); layout->addWidget(text);
  auto *form = new QFormLayout;
  m_name = new QLineEdit(tr("未命名工程"), page); m_name->setObjectName(QStringLiteral("projectNameEdit"));
  m_output = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Paleo"), page); m_output->setObjectName(QStringLiteral("projectDirectoryEdit"));
  m_root = new QLineEdit(page); m_root->setObjectName(QStringLiteral("projectSourceRootEdit")); m_root->setPlaceholderText(tr("可选：选择工区资料总目录，自动识别坐标表和测井文件夹"));
  form->addRow(tr("工程名称"), m_name); form->addRow(tr("工程保存位置"), pathRow(m_output, true)); form->addRow(tr("资料总目录（可选）"), pathRow(m_root, true));
  m_projectCrs = new PaleoCrsSelector(true, page);
  m_projectCrs->setObjectName(QStringLiteral("projectBaselineCrsEdit"));
  m_projectCrs->setNotSetText(tr("自动沿用资料坐标（不指定）"));
  m_projectCrs->widget()->setToolTip(tr("统一井点、地震、层位和相图的计算基准。仅接受米制投影 CRS；未知坐标资料需先声明来源，不能仅修改标签。"));
  form->addRow(tr("工程基准坐标系"), m_projectCrs->widget());
  layout->addLayout(form);
  auto *note = new QLabel(tr("将在保存位置下新建同名工程文件夹。原资料保留，工程默认登记资料索引并保存生成图层；移动资料后可重新定位。"), page); note->setWordWrap(true); layout->addWidget(note); layout->addStretch();
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
  m_crs = new PaleoCrsSelector(true, page);
  m_crs->setObjectName(QStringLiteral("pointSourceCrsEdit"));
  m_crs->widget()->setToolTip(tr("选择井点资料的真实来源 CRS。自动识别只读取资料声明，不按坐标数值猜带号；未知来源必须明确选择后再转换。"));
  form->addRow(tr("资料坐标系"), m_crs->widget()); layout->addLayout(form);
  auto *scanButton = new QPushButton(tr("识别与预览"), page); scanButton->setObjectName(QStringLiteral("projectSetupScanButton")); connect(scanButton, &QPushButton::clicked, this, &ProjectSetupDialog::scan); layout->addWidget(scanButton);
  m_pointPreview = new QTableWidget(0, 3, page); m_pointPreview->setObjectName(QStringLiteral("projectPointPreview")); m_pointPreview->setHorizontalHeaderLabels({tr("井名"), tr("横坐标／经度"), tr("纵坐标／纬度")}); m_pointPreview->setEditTriggers(QAbstractItemView::NoEditTriggers); m_pointPreview->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); layout->addWidget(m_pointPreview, 1);
  return page;
}
QWidget *ProjectSetupDialog::sourcesPage()
{
  auto *page = new QWidget(this); auto *layout = new QVBoxLayout(page); auto *form = new QFormLayout;
  m_wells = new QLineEdit(page); m_seismic = new QLineEdit(page); m_horizons = new QLineEdit(page); m_references = new QLineEdit(page); m_boundary = new QLineEdit(page);
  m_wells->setObjectName(QStringLiteral("projectWellsEdit")); m_references->setObjectName(QStringLiteral("projectReferencesEdit"));
  const QList<QPair<QString, QLineEdit *>> rows{{tr("井数据文件夹"), m_wells}, {tr("地震文件夹"), m_seismic}, {tr("层位文件夹"), m_horizons}, {tr("参考资料文件夹"), m_references}};
  for (const auto &row : rows) { row.second->setPlaceholderText(tr("可选，留空即可跳过")); form->addRow(row.first, pathRow(row.second, true)); }
  m_wells->setPlaceholderText(tr("可选：分井目录，或集中存放 XML／LAS 的测井资料目录"));
  m_boundary->setPlaceholderText(tr("可选；未提供时，使用地震数据生成覆盖范围"));
  m_paleomaps = new QLineEdit(page); m_paleomaps->setObjectName(QStringLiteral("projectPaleomapsEdit"));
  m_paleomaps->setPlaceholderText(tr("可选：现有古地理图目录，也可填写单个图件路径"));
  connect(m_paleomaps, &QLineEdit::editingFinished, this, [this] { if (m_dirty) scan(); });
  form->addRow(tr("现有古地理图"), pathRow(m_paleomaps, true));
  form->addRow(tr("测区范围矢量"), pathRow(m_boundary, false, tr("面矢量 (*.gpkg *.shp *.geojson);;所有文件 (*)"))); layout->addLayout(form);
  m_normalizeMaps = new QCheckBox(tr("自动识别微相、整理三级相与系统纹理（保留原图）"), page); m_normalizeMaps->setChecked(true); layout->addWidget(m_normalizeMaps);
  m_maps = new QTableWidget(0, 3, page); m_maps->setObjectName(QStringLiteral("projectPaleomapChoices"));
  m_maps->setHorizontalHeaderLabels({tr("导入图件（配套文件按套）"), tr("所属层位"), tr("格式／配套文件")});
  m_maps->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_maps->setEditTriggers(QAbstractItemView::NoEditTriggers); m_maps->hide();
  layout->addWidget(m_maps, 1);
  connect(m_maps, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
    if (item->column() != 0 || item->row() >= m_preview.maps.size()) return;
    auto &map = m_preview.maps[item->row()]; map.included = item->checkState() == Qt::Checked; ++m_revision;
    m_mapChoices.insert(map.path, QVariantMap{{"included", map.included}, {"horizon", map.horizon}});
  });
  auto *note = new QLabel(tr("井资料按井名关联。古地理图默认优先同名 GPKG、取消勾选预览图，可改选 SHP；SHP、SHX、DBF 必须齐全，投影、编码和样式一并保存。空文件、旧工程和重复分层会在确认页说明。"), page); note->setWordWrap(true); layout->addWidget(note);
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
  if (m_empty || m_blankLocked) return {};
  ProjectSetupSources s; s.root = m_root->text().trimmed(); s.points = m_points->text().trimmed(); s.wells = m_wells->text().trimmed(); s.seismic = m_seismic->text().trimmed(); s.horizons = m_horizons->text().trimmed(); s.references = m_references->text().trimmed(); s.boundary = m_boundary->text().trimmed();
  s.paleomaps = m_paleomaps->text().trimmed(); s.normalizeMaps = m_normalizeMaps->isChecked(); s.mapChoices = m_mapChoices;
  for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) if (!it.value()->currentText().isEmpty()) s.pointMapping.insert(it.key(), it.value()->currentText());
  if (s.pointMapping.isEmpty()) s.pointMapping = m_initialMapping;
  s.wellTargets = m_preview.wellTargets;
  s.pointMapping.insert("crs", m_crs->text().trimmed()); s.projectCrs = m_projectCrs->text().trimmed(); return s;
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
  m_paleomaps->setText(s.paleomaps); m_mapChoices.clear();
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
  m_paleomaps->setText(s.paleomaps); m_normalizeMaps->setChecked(s.normalizeMaps); m_mapChoices = s.mapChoices;
  m_initialMapping = s.pointMapping; m_preview.wellTargets = s.wellTargets;
  {
    QSignalBlocker sourceBlock(m_crs), baselineBlock(m_projectCrs);
    m_crs->setText(s.pointMapping.value("crs").toString());
    m_projectCrs->setText(s.projectCrs);
  }
  m_projectCrs->widget()->setEnabled(false);
  m_projectCrs->widget()->setToolTip(tr("已有工程的计算基准不可在此更改；如需另一基准，请新建工程并重新导入，避免既有成果失配。"));
  scan();
}
void ProjectSetupDialog::scan()
{
  if (m_blankLocked || (m_busy && m_scanRevision == m_revision)) return;
  QString sourceError, baselineError;
  if (!m_crs->validate(&sourceError) || !m_projectCrs->validate(&baselineError, true))
  {
    m_status->setText(!sourceError.isEmpty() ? sourceError : baselineError);
    return;
  }
  // 上一轮创建失败（位置被占/拒访问）到此作废：否则 showPreview 仍把它当
  // 最新状态压在识别结果之上，用户改完资料来源也看不到新的识别状态。
  setProperty("paleo.creationError", QVariant());
  m_scanRevision = m_revision;
  m_empty = false; m_busy = true; m_next->setEnabled(false); m_status->setText(tr("正在识别资料，完成前可继续修改或取消。"));
  m_workflow->scan(sources());
}
void ProjectSetupDialog::showPreview(const ProjectSetupPreview &p)
{
  if (m_blankLocked) return;
  if (m_scanRevision != m_revision) { scan(); return; }
  m_preview = p; m_busy = false; m_dirty = false;
  showMaps(p);
  m_assignments->setRowCount(p.logWellNames.size()); m_assignments->setVisible(!p.logWellNames.isEmpty());
  m_assignments->setMaximumHeight(m_assignments->horizontalHeader()->sizeHint().height() +
      qMin(4, m_assignments->rowCount()) * m_assignments->verticalHeader()->defaultSectionSize() +
      2 * m_assignments->frameWidth() + PaleoTheme::tokens().spacingSm);
  int assignmentRow = 0;
  for (auto it = p.logWellNames.cbegin(); it != p.logWellNames.cend(); ++it, ++assignmentRow) {
    const QString &path = it.key();
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
    const auto selected = p.pointMapping.value(it.key()).toString();
    if (!selected.isEmpty() && it.value()->findText(selected) < 0) it.value()->addItem(selected);
    it.value()->setCurrentText(p.pointMapping.value(it.key()).toString());
  }
  {
    QSignalBlocker block(m_crs);
    m_crs->setText(p.pointSourceCrs);
  }
  m_pointPreview->setRowCount(p.pointRows.size());
  for (int row = 0; row < p.pointRows.size(); ++row) for (int col = 0; col < 3; ++col) m_pointPreview->setItem(row, col, new QTableWidgetItem(p.pointRows[row].value(col)));
  refreshReview();
  const QString creationError = property("paleo.creationError").toString();
  // 创建失败只压一次状态：读过即清，避免旧错误继续盖在后续识别结果之上
  // （scan() 启动时已清过一次，这里是「扫描在途时又报创建失败」的兜底）。
  if (!creationError.isEmpty()) setProperty("paleo.creationError", QVariant());
  m_status->setText(!creationError.isEmpty() ? creationError : !p.errors.isEmpty() ? tr("工程配置需要调整，详细原因见确认页。")
      : !p.sourceProblems.isEmpty() ? tr("部分资料未通过检查；仍可创建工程并导入其余资料，详情见确认页。")
      : p.paths.isEmpty() && p.maps.isEmpty() ? tr("未找到可导入资料；请选择资料来源，或创建空白工程。")
      : tr("快速识别完成；已展示预览样本，完整校验将在导入时执行。")); m_next->setEnabled(true);
}
void ProjectSetupDialog::showMaps(const ProjectSetupPreview &p)
{
  QSignalBlocker block(m_maps); m_mapChoices.clear();
  m_maps->setRowCount(p.maps.size()); m_maps->setVisible(!p.maps.isEmpty());
  for (int row = 0; row < p.maps.size(); ++row) {
    const auto &map = p.maps[row];
    m_mapChoices.insert(map.path, QVariantMap{{"included", map.included}, {"horizon", map.horizon}});
    auto *item = new QTableWidgetItem(QFileInfo(map.path).fileName()); item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(map.included ? Qt::Checked : Qt::Unchecked); item->setToolTip(map.path + '\n' + map.problem); m_maps->setItem(row, 0, item);
    auto *horizon = new QComboBox(m_maps); horizon->addItem(tr("请选择层位"), QString());
    for (const auto &name : mappingHorizons()) horizon->addItem(name, name);
    horizon->setCurrentIndex(qMax(0, horizon->findData(map.horizon))); m_maps->setCellWidget(row, 1, horizon);
    connect(horizon, &QComboBox::activated, this, [this, row, horizon] {
      auto &map = m_preview.maps[row]; map.horizon = horizon->currentData().toString(); ++m_revision;
      m_mapChoices.insert(map.path, QVariantMap{{"included", map.included}, {"horizon", map.horizon}});
    });
    m_maps->setItem(row, 2, new QTableWidgetItem(map.problem.isEmpty()
        ? tr("%1 · %2 个文件").arg(QFileInfo(map.path).suffix().toUpper()).arg(map.members.size()) : map.problem));
  }
}
void ProjectSetupDialog::refreshReview()
{
  const auto &p = m_preview;
  QString text = tr("以下为识别样本统计，完整数量与校验以导入结果为准。\n");
  text += tr("将建立：%1 口井、%2 条井分层、%3 份测井、%4 个地震文件、%5 个层位面、%6 份其他资料。\n生成井点、井名标注和有效地震覆盖范围，并随工程保存。\n共 %7 个资料文件，另按套导入下列古地理图：\n").arg(p.wells).arg(p.tops).arg(p.logs).arg(p.seismic).arg(p.horizons).arg(p.references).arg(p.paths.size());
  int maps = 0;
  for (const auto &map : p.maps) if (map.included) {
    if (!map.problem.isEmpty() || !isMappingHorizon(map.horizon)) {
      text += tr("暂不导入 %1：%2\n").arg(map.path, map.problem.isEmpty() ? tr("尚未选择有效层位") : map.problem); continue;
    }
    ++maps; text += tr("%1 · %2 · 配套 %3 个文件\n").arg(map.horizon, QFileInfo(map.path).fileName()).arg(map.members.size());
  }
  text += tr("已选 %1 套古地理图。\n").arg(maps);
  if (!p.sourceCrs.isEmpty()) text += tr("资料坐标系：%1\n").arg(p.sourceCrs);
  if (!p.errors.isEmpty()) text += tr("\n需要调整（也可清空对应来源后跳过）：\n") + p.errors.join('\n');
  if (!p.sourceProblems.isEmpty()) {
    text += tr("\n以下资料暂不导入，其余资料继续；修正后可通过工程资料设置重试：\n");
    for (auto it = p.sourceProblems.cbegin(); it != p.sourceProblems.cend(); ++it) text += it.key() + QStringLiteral("：") + it.value() + '\n';
  }
  if (!p.notes.isEmpty()) text += tr("\n识别说明：\n") + p.notes.join('\n');
  m_review->setPlainText(text);
}
void ProjectSetupDialog::showStep(int step)
{
  QString locationError;
  if (step > 0 && m_existingPath.isEmpty() &&
      !ProjectSetupWorkflow::checkProjectLocation(projectPath(), &locationError)) {
    showCreationError(locationError); return;
  }
  if (step > 0) setProperty("paleo.creationError", QVariant());
  m_pages->setCurrentIndex(step); m_previous->setEnabled(step > 0); m_next->setText(step == 3 ? (m_existingPath.isEmpty() ? tr("创建并导入") : tr("保存设置并导入")) : tr("下一步（本步可跳过）"));
  if ((step == 1 || step == 3) && m_dirty) scan();
  else if (step == 3) refreshReview();
}
ProjectSetupPreview ProjectSetupDialog::preview() const {
  if (m_empty || m_blankLocked) {
    ProjectSetupPreview result; result.projectCrs = m_projectCrs->text().trimmed(); return result;
  }
  return m_preview;
}
void ProjectSetupDialog::finish()
{
  const QString name = m_name->text().trimmed();
  if (name.isEmpty() || name == "." || name == ".." || name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))) || m_output->text().trimmed().isEmpty()) { m_empty = false; m_status->setText(tr("请填写有效的工程名称和保存位置。")); return; }
  QString locationError;
  if (m_existingPath.isEmpty() && !ProjectSetupWorkflow::checkProjectLocation(projectPath(), &locationError)) {
    showCreationError(locationError); return;
  }
  setProperty("paleo.creationError", QVariant());
  if (m_empty) { m_blankLocked = true; accept(); return; }
  if (m_busy || m_dirty) { m_status->setText(tr("请等待本次识别完成后再创建。")); if (!m_busy) scan(); return; }
  if (!m_empty && !m_preview.errors.isEmpty()) { m_status->setText(tr("请调整有问题的资料来源，或将其清空后重新识别。")); return; }
  accept();
}

void ProjectSetupDialog::showCreationError(const QString &error)
{
  m_blankLocked = false;
  setProperty("paleo.creationError", error);
  m_steps->setCurrentIndex(0);
  m_status->setText(error);
}
