// 层：视图
// ribbonpanels — W4 壳瘦身：buildRibbonPanels/addEditingPanel 自
// paleomainwindow.cpp 迁出（仍是 PaleoMainWindow 成员函数，定义在本 TU）。
// 页面板按钮是状态源：ribbon 动作经 PaleoRibbon::mirror 镜像它们。
#include "paleomainwindow.h"

#include "paleotheme.h"
#include "paleoicons.h"
#include "paleoribbon.h"

#include "pages/pagepanels.h"
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "ui/seismic3d/seismic3dviewportwidget.h"
#include "webviewpanel.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../services/previewdoc.h"
#include "../catalog/datacatalog.h"
#include "../linkage/selectioncontext.h"

#include <qgsmapcanvas.h>
#include <qgsmaptoolpan.h>
#include <qgsmaptoolzoom.h>
#include <qgsproject.h>
#include <qgslayertree.h>
#include <qgsvectorlayer.h>
#include <qgsmessagelog.h>
#include "edittools/editingtoolbar.h"


#include <QAbstractItemModel>
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>

// Ribbon 命令组：每个页签按「做事的顺序」排组——数据管理（导入 → 列表 →
// 预览）、预测编图（运行 → 叠加对照 → 编辑 → 地图 → 结果）、单因素图（约束
// → 插值 → 连井 → 编辑 → 地图 → 结果）、智能编图（编图链 → 编辑 → 地图 →
// 图件输出 → 版本）、验证（验证 → 联动定位 → 地图 → 发布）。
// 页面板按钮是状态源：ribbon 动作经 PaleoRibbon::mirror 镜像它们。
// ---------------------------------------------------------------------------
void PaleoMainWindow::buildRibbonPanels(DataPage *data, PredictPage *predict,
                                        ConstraintPage *constraint, ComposePage *compose,
                                        ValidatePage *validate, PaleoEditingToolbar *editTb,
                                        WellCorrelationPanel *corrPanel)
{
  SARibbonBar *bar = ribbonBar();
  if (!bar)
    return;
  bar->beginUpdate();

  const auto icon = [](const char *name) { return PaleoIcons::qgisTheme(QLatin1String(name)); };
  const auto newAction = [this](const QString &text, const QIcon &ic, const QString &tip,
                                const char *name) {
    auto *a = new QAction(ic, text, this);
    a->setObjectName(QLatin1String(name));
    if (!tip.isEmpty())
      a->setToolTip(tip);
    return a;
  };
  // 镜像命令：面板按钮存在才建（空工作流的测试壳照样能跑）。
  const auto mirrored = [&newAction](QWidget *page, const char *buttonName, const QString &text,
                                     const QIcon &ic, const char *actionName,
                                     bool syncText = false) -> QAction * {
    auto *b = page ? page->findChild<QAbstractButton *>(QLatin1String(buttonName)) : nullptr;
    if (!b)
      return nullptr;
    QAction *a = newAction(text, ic, QString(), actionName);
    PaleoRibbon::mirror(a, b, syncText);
    return a;
  };
  const auto large = [](SARibbonPanel *p, QAction *a, bool run = false) {
    if (!p || !a)
      return;
    p->addLargeAction(a);
    if (run)
      PaleoRibbon::markRun(PaleoRibbon::buttonFor(p, a));
  };
  const auto small = [](SARibbonPanel *p, QAction *a) {
    if (p && a)
      p->addSmallAction(a);
  };
  const auto panel = [](SARibbonCategory *cat, const QString &title, const char *name) {
    SARibbonPanel *p = cat->addPanel(title);
    p->setObjectName(QLatin1String(name));
    return p;
  };

  // ---- 共享动作（多个页签复用同一颗 QAction）----
  // 「参数」= 右侧 dock 的 toggleViewAction：文案随页（预测参数/单因素参数…）。
  QAction *paramsAct = m_rightDock ? m_rightDock->toggleViewAction() : nullptr;
  if (paramsAct)
  {
    paramsAct->setIcon(icon("mActionOptions.svg"));
    paramsAct->setToolTip(tr("显示/隐藏右侧参数面板"));
  }
  auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  const auto bottomAction = [this, bottomTabs, &newAction](QWidget *tab, const QString &text,
                                                           const QIcon &ic, const QString &tip,
                                                           const char *name) -> QAction * {
    if (!tab || !bottomTabs)
      return nullptr;
    QAction *a = newAction(text, ic, tip, name);
    connect(a, &QAction::triggered, this, [this, bottomTabs, tab] {
      m_bottomDock->show();
      m_bottomDock->raise();
      bottomTabs->setCurrentWidget(tab);
    });
    return a;
  };
  QAction *corrAct = bottomAction(corrPanel, tr("连井剖面"), icon("mActionElevationProfile.svg"),
                                  tr("在底部面板打开连井剖面（与地图联动）"),
                                  "ribbonCorrelationAction");
  QAction *attrAct = bottomAction(findChild<QWidget *>(QStringLiteral("attributeTablePanel")),
                                  tr("属性表"), icon("mActionOpenTable.svg"),
                                  tr("在底部面板打开图层属性表"), "ribbonAttributeTableAction");
  QAction *releaseAct = bottomAction(findChild<QWidget *>(QStringLiteral("releasePanel")),
                                     tr("发布记录"), icon("mActionHistory.svg"),
                                     tr("在底部面板查看已发布的版本"), "ribbonReleaseAction");
  QAction *toValidate = newAction(tr("送交验证"), icon("mActionSharingExport.svg"),
                                  tr("切到验证页，用井点残差检查当前成果"), "ribbonToValidateAction");
  connect(toValidate, &QAction::triggered, this, [this] { showPage(QStringLiteral("validate")); });

  // 地图导航：原生 QGIS 工具；setAction 让工具激活/停用时自动勾选/取消。
  QAction *panAct = nullptr, *zoomInAct = nullptr, *zoomOutAct = nullptr, *fullAct = nullptr;
  if (m_canvasCtl)
  {
    QgsMapCanvas *cv = m_canvasCtl->canvas();
    const auto toolAction = [this, &newAction](QgsMapTool *tool, const QString &text,
                                               const QIcon &ic, const QString &tip,
                                               const char *name) {
      QAction *a = newAction(text, ic, tip, name);
      a->setCheckable(true);
      tool->setAction(a);
      connect(a, &QAction::triggered, this, [this, tool] { m_canvasCtl->setMapTool(tool); });
      return a;
    };
    panAct = toolAction(new QgsMapToolPan(cv), tr("平移"), icon("mActionPan.svg"),
                        tr("拖动平移地图"), "ribbonPanAction");
    zoomInAct = toolAction(new QgsMapToolZoom(cv, false), tr("放大"), icon("mActionZoomIn.svg"),
                           tr("点击或框选放大"), "ribbonZoomInAction");
    zoomOutAct = toolAction(new QgsMapToolZoom(cv, true), tr("缩小"), icon("mActionZoomOut.svg"),
                            tr("点击或框选缩小"), "ribbonZoomOutAction");
    fullAct = newAction(tr("全图"), icon("mActionZoomFullExtent.svg"), tr("缩放到全部图层"),
                        "ribbonZoomFullAction");
    connect(fullAct, &QAction::triggered, this, [this] { m_canvasCtl->zoomToFullExtent(); });
  }
  const auto navPanel = [&](SARibbonCategory *cat) {
    if (!panAct)
      return;
    SARibbonPanel *p = panel(cat, tr("地图"), "ribbonNavPanel");
    large(p, panAct);
    small(p, zoomInAct);
    small(p, zoomOutAct);
    small(p, fullAct);
  };

  // ================= 数据管理 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("data")))
  {
    SARibbonPanel *p = panel(cat, tr("数据导入"), "ribbonPanel.data.import");
    large(p, mirrored(data, "importFolder", tr("导入工区文件夹"), icon("mIconFolderOpen.svg"),
                      "ribbonImportFolder"));
    small(p, mirrored(data, "importWells", tr("导入井数据"), icon("mIconPointLayer.svg"),
                      "ribbonImportWells"));
    small(p, mirrored(data, "importWellLogs", tr("导入测井数据"), icon("mIconLineLayer.svg"),
                      "ribbonImportWellLogs"));
    small(p, mirrored(data, "importSeismic", tr("导入地震数据"), icon("mIconRasterLayer.svg"),
                      "ribbonImportSeismic"));
    small(p, mirrored(data, "importBoundary", tr("导入边界数据"), icon("mIconPolygonLayer.svg"),
                      "ribbonImportBoundary"));

    if (data)
    {
      SARibbonPanel *lp = panel(cat, tr("数据列表"), "ribbonPanel.data.list");
      QAction *unresolved = newAction(tr("只看未决"), icon("mActionFilter2.svg"),
                                      tr("只显示还有未决关联的数据"), "ribbonUnresolvedFilter");
      unresolved->setCheckable(true);
      connect(unresolved, &QAction::triggered, data, &DataPage::setUnresolvedFilter);
      // 过滤条显隐（「清除过滤」、导入确认框的「查看未决」）回写勾选态。
      if (auto *filterBar = data->findChild<QWidget *>(QStringLiteral("unresolvedFilterBar")))
        PaleoRibbon::followVisibility(unresolved, filterBar);
      large(lp, unresolved);
      QAction *refresh = newAction(tr("刷新列表"), icon("mActionRefresh.svg"),
                                   tr("按数据目录重建列表"), "ribbonRefreshList");
      connect(refresh, &QAction::triggered, data, &DataPage::refreshAssetTable);
      large(lp, refresh);
    }

    auto *maxBtn = m_previewTabs
                       ? m_previewTabs->findChild<QToolButton *>(QStringLiteral("previewMaxButton"))
                       : nullptr;
    auto *inner = m_previewTabs
                      ? m_previewTabs->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"))
                      : nullptr;
    if (maxBtn && inner)
    {
      SARibbonPanel *vp = panel(cat, tr("数据预览"), "ribbonPanel.data.preview");
      QAction *maxAct = newAction(tr("最大化预览"), PaleoIcons::maximize(), QString(),
                                  "ribbonPreviewMaximize");
      maxAct->setCheckable(true);
      connect(maxAct, &QAction::triggered, maxBtn, [maxBtn](bool on) { maxBtn->setChecked(on); });
      connect(maxBtn, &QAbstractButton::toggled, maxAct, [maxAct](bool on) {
        maxAct->setChecked(on);
        maxAct->setText(on ? tr("还原预览") : tr("最大化预览"));
        maxAct->setIcon(on ? PaleoIcons::restore() : PaleoIcons::maximize());
      });
      // 没有打开的预览时不可用（禁用必带原因，§35）。
      const auto syncMax = [maxAct, inner] {
        const bool any = inner->count() > 0;
        maxAct->setEnabled(any);
        maxAct->setToolTip(any ? tr("预览占满数据面（列表留一行）")
                               : tr("先在数据列表里选一条数据打开预览"));
      };
      connect(inner, &QTabWidget::currentChanged, maxAct, syncMax);
      syncMax();
      large(vp, maxAct);
    }

    SARibbonPanel *sp = panel(cat, tr("地震视口"), "ribbonPanel.data.seismic");
    QAction *seismic3dAct = newAction(tr("三维视口"), icon("mIcon3D.svg"),
                                      tr("打开三维地震体立体视口"), "ribbonActionSeismic3D");
    connect(seismic3dAct, &QAction::triggered, this, [this] {
      syncSeismicVolumeToDocks();
      if (m_seismic3dDock) {
        m_seismic3dDock->show();
        m_seismic3dDock->raise();
        if (m_seismic3dPanel && m_seismic3dPanel->viewport()) {
          m_seismic3dPanel->viewport()->fitToBounds();
          m_seismic3dPanel->viewport()->update();
        }
      }
    });
    large(sp, seismic3dAct);

    QAction *seismic2dAct = newAction(tr("地震剖面"), icon("mIconRasterLayer.svg"),
                                      tr("打开二维地震与井震标定剖面"), "ribbonActionSeismic2D");
    connect(seismic2dAct, &QAction::triggered, this, [this] {
      syncSeismicVolumeToDocks();
      if (m_seismicSectionDock) {
        m_seismicSectionDock->show();
        m_seismicSectionDock->raise();
      }
    });
    large(sp, seismic2dAct);
  }

  // ================= 预测编图 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("predict")))
  {
    SARibbonPanel *p = panel(cat, tr("预测运行"), "ribbonPanel.predict.run");
    large(p, mirrored(predict, "runButton", tr("运行预测"), icon("mActionStart.svg"),
                      "ribbonRunPrediction"),
          true);
    large(p, paramsAct);
    if (corrAct || attrAct)
    {
      SARibbonPanel *cp = panel(cat, tr("叠加对照"), "ribbonPanel.predict.compare");
      large(cp, corrAct);
      large(cp, attrAct);
      QAction *pSeismic3d = newAction(tr("三维地震"), icon("mIcon3D.svg"),
                                      tr("打开三维地震立体视口"), "ribbonPredictSeismic3D");
      connect(pSeismic3d, &QAction::triggered, this, [this] {
        syncSeismicVolumeToDocks();
        if (m_seismic3dDock) {
          m_seismic3dDock->show();
          m_seismic3dDock->raise();
          if (m_seismic3dPanel && m_seismic3dPanel->viewport()) {
            m_seismic3dPanel->viewport()->fitToBounds();
            m_seismic3dPanel->viewport()->update();
          }
        }
      });
      large(cp, pSeismic3d);
      QAction *pSeismic2d = newAction(tr("地震剖面"), icon("mIconRasterLayer.svg"),
                                      tr("打开地震与井震剖面"), "ribbonPredictSeismic2D");
      connect(pSeismic2d, &QAction::triggered, this, [this] {
        syncSeismicVolumeToDocks();
        if (m_seismicSectionDock) {
          m_seismicSectionDock->show();
          m_seismicSectionDock->raise();
        }
      });
      large(cp, pSeismic2d);
    }
    addEditingPanel(cat, editTb);
    navPanel(cat);
    large(panel(cat, tr("结果"), "ribbonPanel.predict.result"), toValidate);
  }

  // ================= 单因素图 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("constraint")))
  {
    SARibbonPanel *p = panel(cat, tr("约束编辑"), "ribbonPanel.constraint.draw");
    auto *drawBtn = constraint ? constraint->findChild<QPushButton *>(QStringLiteral("drawButton"))
                               : nullptr;
    auto *shape = constraint ? constraint->findChild<QComboBox *>(QStringLiteral("shapeCombo"))
                             : nullptr;
    if (QAction *draw = mirrored(constraint, "drawButton", tr("绘制约束"),
                                 icon("mActionCaptureLine.svg"), "ribbonDrawConstraint"))
    {
      // 拆分按钮：主体按参数面板当前形状画；下拉直接挑形状开画（改的是
      // 同一个 shapeCombo，参数面板随之显示）。
      if (shape && drawBtn)
      {
        auto *menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("ribbonConstraintShapeMenu"));
        for (int i = 0; i < shape->count(); ++i)
          menu->addAction(shape->itemText(i), this, [shape, drawBtn, i] {
            shape->setCurrentIndex(i);
            drawBtn->click();
          });
        draw->setMenu(menu);
        p->addLargeAction(draw, QToolButton::MenuButtonPopup);
      }
      else
        large(p, draw);
    }
    large(p, paramsAct);
    SARibbonPanel *ip = panel(cat, tr("插值计算"), "ribbonPanel.constraint.idw");
    large(ip, mirrored(constraint, "runIdwButton", tr("计算单因素"), icon("mActionStart.svg"),
                       "ribbonRunIdw"),
          true);
    if (corrAct)
      large(panel(cat, tr("连井分析"), "ribbonPanel.constraint.correlation"), corrAct);
    addEditingPanel(cat, editTb);
    navPanel(cat);
    large(panel(cat, tr("结果"), "ribbonPanel.constraint.result"), toValidate);
  }

  // ================= 智能编图 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("compose")))
  {
    SARibbonPanel *p = panel(cat, tr("编图链"), "ribbonPanel.compose.chain");
    large(p, mirrored(compose, "thicknessChainButton", tr("生成等厚图"),
                      icon("processingAlgorithm.svg"), "ribbonThicknessChain", true),
          true);
    small(p, mirrored(compose, "fuseButton", tr("合成编图"), icon("processingModel.svg"),
                      "ribbonFuse"));
    small(p, mirrored(compose, "polygonizeButton", tr("转为相多边形"),
                      icon("mActionCapturePolygon.svg"), "ribbonPolygonize"));
    large(p, paramsAct);
    addEditingPanel(cat, editTb);
    navPanel(cat);

    SARibbonPanel *op = panel(cat, tr("图件输出"), "ribbonPanel.compose.output");
    large(op, mirrored(compose, "exportPdfButton", tr("导出 PDF"), icon("mActionSaveAsPDF.svg"),
                       "ribbonExportPdf"));
    if (QAction *designer = findChild<QAction *>(QStringLiteral("ribbonDesignerAction")))
    {
      large(op, designer);
      if (auto *b = PaleoRibbon::buttonFor(op, designer))
      {
        b->setObjectName(QStringLiteral("designerButton"));
        b->setAccessibleName(tr("打开图件设计器"));
      }
    }

    SARibbonPanel *vp = panel(cat, tr("版本"), "ribbonPanel.compose.version");
    large(vp, mirrored(compose, "saveVersionButton", tr("保存版本"), icon("mActionFileSaveAs.svg"),
                       "ribbonSaveVersion", true));
    large(vp, mirrored(compose, "publishButton", tr("发布"), icon("mActionSharing.svg"),
                       "ribbonPublish"));
    small(vp, toValidate);
  }

  // ================= 验证 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("validate")))
  {
    SARibbonPanel *p = panel(cat, tr("验证"), "ribbonPanel.validate.run");
    large(p, mirrored(validate, "runButton", tr("运行验证"), icon("mActionStart.svg"),
                      "ribbonRunValidation"),
          true);
    large(p, paramsAct);
    QAction *section = mirrored(validate, "openSeismicSectionButton", tr("看这条剖面"),
                                icon("mIconRasterLayer.svg"), "ribbonOpenSection");
    if (section || corrAct)
    {
      SARibbonPanel *lp = panel(cat, tr("联动定位"), "ribbonPanel.validate.locate");
      large(lp, section);
      large(lp, corrAct);
    }
    navPanel(cat);
    if (releaseAct)
      large(panel(cat, tr("发布"), "ribbonPanel.validate.release"), releaseAct);
  }

  bar->endUpdate();
  bar->updateRibbonGeometry();
}

void PaleoMainWindow::addEditingPanel(SARibbonCategory *category, PaleoEditingToolbar *editTb)
{
  if (!category || !editTb)
    return;
  SARibbonPanel *p = category->addPanel(tr("要素编辑"));
  p->setObjectName(QStringLiteral("ribbonEditPanel"));

  // 目标图层：共享编辑条下拉的 model（●标记编辑中图层，同一份数据）；用户
  // 选择经 setCurrentLayer 回写——与编辑条自身 activated 同一路径。
  QComboBox *master = editTb->layerCombo();
  auto *combo = new QComboBox(p);
  combo->setObjectName(QStringLiteral("ribbonEditLayerCombo"));
  combo->setAccessibleName(tr("编辑图层"));
  combo->setToolTip(tr("要编辑的矢量图层"));
  combo->setPlaceholderText(tr("选择可编辑图层"));
  combo->setModel(master->model());
  auto *state = new QLabel(p);
  state->setObjectName(QStringLiteral("ribbonEditState"));
  connect(combo, &QComboBox::activated, editTb, [editTb, combo](int i) {
    editTb->setCurrentLayer(qvariant_cast<QgsVectorLayer *>(combo->itemData(i)));
  });
  const auto sync = [combo, state, master, editTb] {
    combo->setCurrentIndex(master->currentIndex());
    state->setText(editTb->stateLabel()->text());
  };
  connect(master, &QComboBox::currentIndexChanged, combo, sync);
  // 编辑条重建下拉时屏蔽了自身信号——model 变化与会话切换后排队再对一次。
  QAbstractItemModel *model = master->model();
  connect(model, &QAbstractItemModel::modelReset, combo, sync, Qt::QueuedConnection);
  connect(model, &QAbstractItemModel::rowsInserted, combo, sync, Qt::QueuedConnection);
  connect(model, &QAbstractItemModel::rowsRemoved, combo, sync, Qt::QueuedConnection);
  connect(editTb, &PaleoEditingToolbar::editingStarted, combo, sync, Qt::QueuedConnection);
  connect(editTb, &PaleoEditingToolbar::editingStopped, combo, sync, Qt::QueuedConnection);
  sync();
  p->addSmallWidget(combo);
  p->addSmallWidget(state);

  p->addLargeAction(editTb->actionSelect());
  p->addLargeAction(editTb->actionAddFeature(), QToolButton::InstantPopup);
  p->addSmallAction(editTb->actionReshape());
  p->addSmallAction(editTb->actionMove());
  p->addSmallAction(editTb->actionDeleteFeatures());
  p->addSmallAction(editTb->actionVertexEdit());
  p->addSmallAction(editTb->actionTopological()); // 共边节点联动开关
  p->addSmallAction(editTb->actionSave());
  p->addSmallAction(editTb->actionCancel());
}
