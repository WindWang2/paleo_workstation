// 层：测试壳
#include <QtTest>
#include "workflow/aichatcontroller.h"
#include "workflow/tileoperation.h"
#include "workflow/tileoperationservice.h"
#include "workflow/capabilityhost.h"
#include "services/paleotaskservice.h"

class TestAgentNoOrt:public QObject {
  Q_OBJECT
private slots:
  void localInferenceReadinessFailsBeforeScheduling() {
    AiChatToolRunner runner;
    QCOMPARE(runner.readiness("paleo.horizon_tracking_suggestion").code,QString("ort_unavailable"));
    QCOMPARE(runner.readiness("paleo.tile_classification").code,QString("ort_unavailable"));
    PaleoTaskService tasks;paleo::capability::CapabilityHost host;host.setTaskService(&tasks);
    paleo::TileOperationService tile;tile.configure(&host,&tasks,nullptr,{},{});
    QCOMPARE(tile.readiness({{"publish_intent","prepare"}},false),QString("ort_unavailable"));
    QCOMPARE(tile.readiness({{"publish_intent","compute-only"}},false),QString("ort_unavailable"));
    QVERIFY(tasks.tasks().isEmpty());
  }
  void persistedPublicationIsDiscoverableWithoutOrt() {
    PaleoTaskService tasks;paleo::capability::CapabilityHost host;host.setTaskService(&tasks);
    paleo::TileOperationService tile;tile.configure(&host,&tasks,nullptr,{},{});
    QVERIFY(tile.readiness({{"publish_intent","publish"}},false).isEmpty());
    QVERIFY(tile.readiness({},true).isEmpty());
    paleo::FrozenTilePlan plan;paleo::FrozenRasterInput source;QString error;
    QVERIFY(!paleo::prepareTilePlan(nullptr,"model",source,{},plan,{},&error));QCOMPARE(error,QString("ort_unavailable"));
    QJsonObject summary;const auto result=paleo::computeTilePlan(nullptr,plan,{},summary,{});
    QVERIFY(!result.ok);QCOMPARE(result.error,QString("ort_unavailable"));
  }
};
QTEST_GUILESS_MAIN(TestAgentNoOrt)
#include "tst_agent_noort.moc"
