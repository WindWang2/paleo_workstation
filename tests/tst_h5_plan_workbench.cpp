// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/projectlock.h"
#include "../src/services/hostcallcontext.h"
#include "../src/ui/ai/aiplanreviewwidget.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/aiplanpresentation.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/capabilityhost.h"
#include "../src/workflow/capabilityplan.h"

using namespace paleo;
using namespace paleo::capability;

// H5 W3/W4：原生计划审阅面板的交互与诚实性。真实 adapter + 真实宿主
//（本地临时工程，Read 门即可预检；数值计算链在 tst_h5_plan_presentation
// 覆盖，本文件不点「计算」）。
//
// oracle：打开即渲染完整结构；提案值/用户修改/算法默认三态来源标注；
// 修改产生新修订并使执行资格失效（重新预检恢复）；按钮 enabled 与
// adapter 同一判定；缺完整图的载荷呈只读拒绝态；主题/窄窗不崩。
namespace {

struct Host {
  QTemporaryDir root{QDir::temp().filePath(QStringLiteral("h5ui-XXXXXX"))};
  DataCatalog catalog;
  ProjectDirLock lock{root.path()};
  CapabilityHost service;
  bool init() {
    if (!lock.tryLock() || !catalog.open(root.path()))
      return false;
    service.setCallerScope(host::Caller::Agent, "agent");
    service.setBindingProvider([this] {
      HostBinding b;
      b.projectDir = root.path();
      b.operationDir = root.path() + "/.paleo/capability-ops";
      b.catalog = &catalog;
      b.writeClaim = lock.claim();
      b.context.documentId = "doc-h5-ui";
      b.context.epoch = 1;
      b.context.open = true;
      b.context.persistentIdentity = true;
      b.context.readOnly = false;
      b.context.ownsWrites = true;
      b.context.allowedEffects =
        host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
      return b;
    });
    return true;
  }
};

QJsonObject validTwoStepGraph() {
  QJsonArray points;
  for (int i = 0; i < 4; ++i)
    points.append(QJsonObject{{"x", (i % 2) * 100.0}, {"y", (i / 2) * 100.0},
                              {"z", (i / 2) * 10.0}, {"well_id", QString::number(i)}});
  const QJsonObject idw{{"id", "idw"},
                        {"capability", "constraint.idw"},
                        {"contract_version", 1},
                        {"output_port", "OUTPUT"},
                        {"intent", "compute-only"},
                        {"parameters", QJsonObject{{"FIELD", "z"}, {"CELL_SIZE", 25.0}}},
                        {"inputs", QJsonObject{{"points", points},
                                               {"subinterval_id", "S1"},
                                               {"points_ref", "v-points-proposal"}}}};
  const QJsonObject contour{{"id", "contours"},
                            {"capability", "singlefactor.contours"},
                            {"contract_version", 1},
                            {"output_port", "LINES"},
                            {"intent", "compute-only"},
                            {"parameters", QJsonObject{{"CONTOUR_LEVEL", 5.0}}},
                            {"inputs", QJsonObject{}}};
  return QJsonObject{{"nodes", QJsonArray{idw, contour}},
                     {"edges", QJsonArray{QJsonObject{{"from", "idw"}, {"from_port", "OUTPUT"},
                                                      {"to", "contours"}, {"to_port", "RASTER"}}}}};
}

QJsonObject previewPayloadFor(const QJsonObject &graph) {
  const auto decoded = paleo::chat::decodePlanPreviewGraph(graph);
  const auto plan = validatePlan(decoded.nodes, decoded.edges);
  return paleo::chat::planPreviewPayload(plan, decoded.nodes, decoded.edges,
                                         QStringLiteral("测试计划"), {});
}
} // namespace

class TestH5PlanWorkbench : public QObject {
  Q_OBJECT
private slots:
  void openRendersStepsParamsPointsAndHonestNotes();
  void preflightEnablesComputeAndEditDisablesIt();
  void invalidNumericEditIsRejectedAndStaysVisible();
  void refusalStateForTruncatedPayload();
  void themeAndNarrowSizesRenderWithoutCrash();
  void panelSurvivesPresenterDetach();

private:
  static QPushButton *button(AiPlanReviewWidget *panel, const char *name) {
    return panel->findChild<QPushButton *>(QLatin1String(name));
  }
};

void TestH5PlanWorkbench::openRendersStepsParamsPointsAndHonestNotes() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  AiPlanReviewWidget panel;
  panel.attachPresenter(&presenter);
  panel.openProposal(previewPayloadFor(validTwoStepGraph()), "call-ui-1");
  QVERIFY(panel.reviewing());

  auto *steps = panel.findChild<QListWidget *>(QStringLiteral("aiPlanReviewSteps"));
  QVERIFY(steps);
  QCOMPARE(steps->count(), 2);
  QVERIFY(steps->item(0)->text().contains(QStringLiteral("constraint.idw")));
  auto *params = panel.findChild<QTableWidget *>(QStringLiteral("aiPlanReviewParams"));
  QVERIFY(params);
  // FIELD/CELL_SIZE 来自提案；ANISO_RATIO 由算法补默认（来源三态可辨）。
  QStringList keys, sources;
  for (int row = 0; row < params->rowCount(); ++row) {
    keys.append(params->item(row, 0)->text());
    sources.append(params->item(row, 3)->text());
  }
  QVERIFY(keys.contains(QStringLiteral("FIELD")));
  QVERIFY(keys.contains(QStringLiteral("CELL_SIZE")));
  QVERIFY(keys.contains(QStringLiteral("ANISO_RATIO")));
  QCOMPARE(sources.at(keys.indexOf(QStringLiteral("ANISO_RATIO"))),
           QStringLiteral("算法默认"));
  QCOMPARE(sources.at(keys.indexOf(QStringLiteral("FIELD"))), QStringLiteral("提案"));
  auto *points = panel.findChild<QTableWidget *>(QStringLiteral("aiPlanReviewPoints"));
  QVERIFY(points);
  QCOMPARE(points->rowCount(), 4);
  QCOMPARE(points->item(0, 0)->text(), QStringLiteral("0"));
  QCOMPARE(points->item(3, 2)->text(), QStringLiteral("10"));
  auto *subinterval = panel.findChild<QLineEdit *>(QStringLiteral("aiPlanReviewSubinterval"));
  QVERIFY(subinterval);
  QCOMPARE(subinterval->text(), QStringLiteral("S1"));
  // 来源诚实性：points_ref 标注「未核验」，不升格为宿主核验事实。
  auto *notes = panel.findChild<QLabel *>(QStringLiteral("aiPlanReviewInputNotes"));
  QVERIFY(notes);
  QVERIFY2(notes->text().contains(QStringLiteral("points_ref")),
           qPrintable(notes->text()));
  QVERIFY2(notes->text().contains(QStringLiteral("未核验")), qPrintable(notes->text()));
  QVERIFY2(notes->text().contains(QStringLiteral("未知")), qPrintable(notes->text()));
  auto *terminal = panel.findChild<QLabel *>(QStringLiteral("aiPlanReviewTerminal"));
  QVERIFY(terminal->text().contains(QStringLiteral("contours")));
  QVERIFY(terminal->text().contains(QStringLiteral("私有计算产物")));
  auto *edges = panel.findChild<QTableWidget *>(QStringLiteral("aiPlanReviewEdges"));
  QCOMPARE(edges->rowCount(), 1);
  QVERIFY(edges->item(0, 0)->text().contains(QStringLiteral("idw.OUTPUT")));
  // 初始态：计算禁用（打开提案不构成执行资格）。
  QVERIFY(!button(&panel, "aiPlanReviewCompute")->isEnabled());
}

void TestH5PlanWorkbench::preflightEnablesComputeAndEditDisablesIt() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  AiPlanReviewWidget panel;
  panel.attachPresenter(&presenter);
  const auto graph = validTwoStepGraph();
  const QString draftId = presenter.openProposal(previewPayloadFor(graph), "call-ui-2");
  panel.openProposal(previewPayloadFor(graph), "call-ui-2");
  auto *compute = button(&panel, "aiPlanReviewCompute");
  auto *preflight = button(&panel, "aiPlanReviewPreflight");
  QVERIFY(!compute->isEnabled());
  QTest::mouseClick(preflight, Qt::LeftButton);
  QVERIFY2(compute->isEnabled(), "重新预检通过后计算必须可用（同一判定）");
  QCOMPARE(presenter.draft(draftId).executionEligible, true);

  // 修改点集 → 新修订 → 执行资格失效；重新预检恢复。
  auto *points = panel.findChild<QTableWidget *>(QStringLiteral("aiPlanReviewPoints"));
  points->item(0, 2)->setText(QStringLiteral("42"));
  QTRY_COMPARE(presenter.draft(draftId).revision, 1);
  QVERIFY(!compute->isEnabled());
  // 修改已进入当前修订（原始提案保留可对照）。
  const auto edited = presenter.draft(draftId).currentGraph;
  const auto row0 = edited.value(QStringLiteral("nodes")).toArray().at(0).toObject()
                        .value(QStringLiteral("inputs")).toObject()
                        .value(QStringLiteral("points")).toArray().at(0).toObject();
  QCOMPARE(row0.value(QStringLiteral("z")).toDouble(), 42.0);
  QTest::mouseClick(preflight, Qt::LeftButton);
  QVERIFY(compute->isEnabled());
  // 原始提案未被改写（对照显示的事实来源）。
  const auto origin = presenter.draft(draftId).originGraph;
  const auto originRow = origin.value(QStringLiteral("nodes")).toArray().at(0).toObject()
                             .value(QStringLiteral("inputs")).toObject()
                             .value(QStringLiteral("points")).toArray().at(0).toObject();
  QCOMPARE(originRow.value(QStringLiteral("z")).toDouble(), 0.0);
}

void TestH5PlanWorkbench::invalidNumericEditIsRejectedAndStaysVisible() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  AiPlanReviewWidget panel;
  panel.attachPresenter(&presenter);
  const auto graph = validTwoStepGraph();
  const QString draftId = presenter.openProposal(previewPayloadFor(graph), "call-ui-3");
  panel.openProposal(previewPayloadFor(graph), "call-ui-3");
  QTest::mouseClick(button(&panel, "aiPlanReviewPreflight"), Qt::LeftButton);
  auto *points = panel.findChild<QTableWidget *>(QStringLiteral("aiPlanReviewPoints"));
  points->item(1, 0)->setText(QStringLiteral("not-a-number"));
  QTRY_VERIFY(points->item(1, 0)->text() == QStringLiteral("not-a-number"));
  // 非有限数值被拒：修订不变、状态行留原因（不 toast 后消失）；此前通过
  // 的预检仍然有效（被拒的编辑没有进入图）。
  QCOMPARE(presenter.draft(draftId).revision, 0);
  auto *status = panel.findChild<QLabel *>(QStringLiteral("aiPlanReviewStatus"));
  QVERIFY2(status->text().contains(QStringLiteral("修改未应用")),
           qPrintable(status->text()));
  QVERIFY(button(&panel, "aiPlanReviewCompute")->isEnabled());
}

void TestH5PlanWorkbench::refusalStateForTruncatedPayload() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  AiPlanReviewWidget panel;
  panel.attachPresenter(&presenter);
  auto payload = previewPayloadFor(validTwoStepGraph());
  payload.remove(QStringLiteral("graph")); // 截断 envelope 形态
  panel.openProposal(payload, "call-ui-4");
  QVERIFY(!panel.reviewing());
  QVERIFY(panel.isVisible()); // 只读拒绝态仍可见
  QVERIFY(!button(&panel, "aiPlanReviewCompute")->isEnabled());
  QVERIFY(!button(&panel, "aiPlanReviewPreflight")->isEnabled());
  auto *note = panel.findChild<QLabel *>(QStringLiteral("aiPlanReviewOperationNote"));
  QVERIFY(note->text().contains(QStringLiteral("plan_graph_unavailable")));
}

void TestH5PlanWorkbench::themeAndNarrowSizesRenderWithoutCrash() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  AiPlanReviewWidget panel;
  panel.resize(900, 700);
  panel.attachPresenter(&presenter);
  panel.openProposal(previewPayloadFor(validTwoStepGraph()), "call-ui-5");
  QTest::mouseClick(button(&panel, "aiPlanReviewPreflight"), Qt::LeftButton);
  PaleoTheme::applyTheme(PaleoTheme::Theme::Dark);
  // 主题切换重建 token 依赖样式：审阅态与内容保留（不是只查控件存在）。
  QVERIFY(panel.reviewing());
  QCOMPARE(panel->findChildren<QListWidget *>(QStringLiteral("aiPlanReviewSteps")).first()->count(), 2);
  PaleoTheme::applyTheme(PaleoTheme::Theme::Light);
  panel.resize(320, 480); // 窄 Dock 形态
  QVERIFY(panel.grab().size().width() > 0);
  QVERIFY(panel.reviewing());
}

void TestH5PlanWorkbench::panelSurvivesPresenterDetach() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  AiPlanReviewWidget panel;
  panel.attachPresenter(&presenter);
  const auto payload = previewPayloadFor(validTwoStepGraph());
  panel.openProposal(payload, "call-ui-6");
  QTest::mouseClick(button(&panel, "aiPlanReviewPreflight"), Qt::LeftButton);
  // 解绑 adapter（换绑/null）：面板不崩、动作按钮如实禁用。
  panel.attachPresenter(nullptr);
  QVERIFY(!button(&panel, "aiPlanReviewCompute")->isEnabled());
  QVERIFY(!button(&panel, "aiPlanReviewPreflight")->isEnabled());
  panel.attachPresenter(&presenter);
  QVERIFY(button(&panel, "aiPlanReviewPreflight")->isEnabled());
}

QTEST_MAIN(TestH5PlanWorkbench)
#include "tst_h5_plan_workbench.moc"
