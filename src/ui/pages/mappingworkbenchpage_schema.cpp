// 层：视图
#include "mappingworkbenchpage.h"
#include "../../domain/faciescatalog.h"
#include "../../domain/faciestaxonomy.h"
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

void MappingWorkbenchPage::buildFaciesSchema(QWidget *schemaBody) {
  auto *sbl = new QVBoxLayout(schemaBody);
  sbl->setContentsMargins(0, 0, 0, 0);
  auto *tabs = new QTabWidget(schemaBody);
  sbl->addWidget(tabs);
  auto *tree = new QTreeWidget(tabs);
  tree->setObjectName("faciesHierarchyTree");
  tree->setAccessibleName(tr("相、亚相、微相三级图例"));
  tree->setHeaderLabels({tr("相 → 亚相 → 微相"), tr("微相编码")});
  tree->setMinimumHeight(140);
  tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
  tabs->addTab(tree, tr("三级图例"));
  auto *editor = new QWidget(tabs);
  auto *editLayout = new QVBoxLayout(editor);
  editLayout->setContentsMargins(0, 0, 0, 0);
  tabs->addTab(editor, tr("维护微相"));
  m_facies = new QTableWidget(0, 7, editor);
  m_facies->setObjectName("faciesSchema");
  m_facies->setAccessibleName(tr("当前层位相分类"));
  m_facies->setHorizontalHeaderLabels({tr("编码"), tr("微相名称"), tr("颜色"),
                                       tr("相"), tr("亚相"), tr("微相"),
                                       tr("纹理")});
  m_facies->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  m_facies->setMinimumHeight(140);
  editLayout->addWidget(m_facies);
  auto *library = new QComboBox(schemaBody);
  library->setObjectName("faciesTextureLibrary");
  library->setAccessibleName(tr("三级相分类模板"));
  library->setEditable(true);
  library->setInsertPolicy(QComboBox::NoInsert);
  for (const auto &v : FaciesTaxonomy::templates()) {
    auto f = v.toMap();
    library->addItem(
        QIcon(FaciesCatalog::resourcePath(f.value("icon", f.value("texture")).toString())),
        QStringList{f.value("facies").toString(), f.value("subfacies").toString(), f.value("microfacies").toString()}.join(" / "), f);
  }
  editLayout->addWidget(library);
  auto *useTexture = new QPushButton(tr("应用所选微相及父级归属"), schemaBody);
  useTexture->setObjectName("applyFaciesTemplate");
  editLayout->addWidget(useTexture);
  connect(useTexture, &QPushButton::clicked, this, [this, library] {
    int row = m_facies->currentRow();
    if (row < 0)
      return;
    const auto f = library->currentData().toMap();
    if (m_facies->item(row, 0)) m_facies->item(row, 0)->setData(Qt::UserRole, f);
    m_facies->setItem(row, 1, new QTableWidgetItem(f.value("name").toString()));
    m_facies->setItem(row, 2,
                      new QTableWidgetItem(f.value("color").toString()));
    m_facies->setItem(row, 6,
                      new QTableWidgetItem(f.value("texture").toString()));
    int column = 3;
    for (const auto &field : {"facies", "subfacies", "microfacies"})
      m_facies->setItem(row, column++, new QTableWidgetItem(f.value(field).toString()));
  });
  auto *sr = new QHBoxLayout;
  auto *add = new QPushButton(tr("新增微相"), schemaBody);
  auto *remove = new QPushButton(tr("删除微相"), schemaBody);
  sr->addWidget(add);
  sr->addWidget(remove);
  auto *save = new QPushButton(tr("保存分类"), schemaBody);
  save->setObjectName("workbench.schema");
  connect(save, &QPushButton::clicked, this, [this] { issue("schema"); });
  sr->addWidget(save);
  editLayout->addLayout(sr);
  auto *hint =
      new QLabel(tr("分类按层位独立保存；已有图件保留生成时分类。选中分类行后可"
                    "从三级模板应用微相及父级；每个编码必须具有完整的相、亚相、微相归属。"),
                 schemaBody);
  hint->setWordWrap(true);
  sbl->addWidget(hint);
  connect(add, &QPushButton::clicked, this, [this, tabs] {
    tabs->setCurrentIndex(1);
    int i = m_facies->rowCount();
    m_facies->insertRow(i);
    int code = 1;
    for (int r = 0; r < i; ++r)
      if (m_facies->item(r, 0))
        code = qMax(code, m_facies->item(r, 0)->text().toInt() + 1);
    m_facies->setItem(i, 0, new QTableWidgetItem(QString::number(code)));
    m_facies->setItem(i, 1, new QTableWidgetItem(tr("新微相")));
    m_facies->setItem(i, 2, new QTableWidgetItem(FaciesCatalog::defaults().value(0).toMap().value("color").toString()));
    for (int c = 3; c < 7; ++c)
      m_facies->setItem(i, c, new QTableWidgetItem());
    m_facies->setCurrentCell(i, 1);
  });
  connect(remove, &QPushButton::clicked, this,
          [this] { m_facies->removeRow(m_facies->currentRow()); });
  const auto rebuildTree = [this, tree] {
    tree->clear();
    for (int row = 0; row < m_facies->rowCount(); ++row) {
      QTreeWidgetItem *parent = tree->invisibleRootItem();
      for (int column = 3; column <= 5; ++column) {
        const auto *cell = m_facies->item(row, column);
        const auto text = cell && !cell->text().trimmed().isEmpty() ? cell->text().trimmed() : tr("未定义");
        QTreeWidgetItem *node = nullptr;
        for (int i = 0; i < parent->childCount(); ++i)
          if (parent->child(i)->text(0) == text) { node = parent->child(i); break; }
        if (!node) node = new QTreeWidgetItem(parent, {text});
        if (column == 5 && m_facies->item(row, 0)) node->setText(1, m_facies->item(row, 0)->text());
        parent = node;
      }
    }
    tree->expandAll();
  };
  connect(m_facies, &QTableWidget::cellChanged, tree, rebuildTree);
  connect(m_facies->model(), &QAbstractItemModel::rowsRemoved, tree, rebuildTree);
  connect(m_facies->model(), &QAbstractItemModel::rowsInserted, tree, rebuildTree);
}
