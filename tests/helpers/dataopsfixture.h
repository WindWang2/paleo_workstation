#pragma once

// #503：tst_panels 巨型测试拆分第一步——DataOps 域共享夹具抽到
// tests/helpers/，供 tst_panels.cpp（folderConfirm/uipolish 等页用例）与
// tst_panels_dataops.cpp（D1–D10 dataops 用例族）共同包含。原为 TestPanels
// 的私有成员（嵌套结构 + 3 个 static helper），搬运逐字不变，仅从类私有段
// 提升为命名空间内自由实体 + 补头文件自包含 include。调用方 using 后用例
// 正文零改动。

#include <functional>

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTableWidget>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include <QtTest>

#include "../../src/catalog/datacatalog.h"
#include "../../src/io/dataimportservice.h"
#include "../../src/services/previewdoc.h"
#include "../../src/ui/pages/pagepanels.h"

namespace paleo::tests::dataops {

// 造一套标准工程：well-A1/well-B2，A1.las(已决主)、Z9.las(未决)、
// free.dat(无链接)。返回结构体带全部 id（dir/svc 由调用方持有）。
struct DataOpsFixture
{
    QString wellA = QStringLiteral("well-A1");
    QString wellB = QStringLiteral("well-B2");
    QString astResolved = QStringLiteral("ast-res");
    QString astPending = QStringLiteral("ast-pend");
    QString astFree = QStringLiteral("ast-free");
    void build(DataCatalog *cat)
    {
        CatalogEntity a1;
        a1.id = wellA;
        a1.entityType = QStringLiteral("well");
        a1.name = QStringLiteral("A1");
        QVERIFY2(cat->addEntity(a1), "well A1");
        CatalogEntity b2;
        b2.id = wellB;
        b2.entityType = QStringLiteral("well");
        b2.name = QStringLiteral("B2");
        QVERIFY2(cat->addEntity(b2), "well B2");
        CatalogAsset r;
        r.id = astResolved;
        r.type = QStringLiteral("well_log");
        r.displayName = QStringLiteral("A1.las");
        QVERIFY(cat->addAsset(r));
        CatalogAsset p;
        p.id = astPending;
        p.type = QStringLiteral("tops");
        p.displayName = QStringLiteral("Z9.las");
        QVERIFY(cat->addAsset(p));
        CatalogAsset f;
        f.id = astFree;
        f.type = QStringLiteral("seismic");
        f.displayName = QStringLiteral("free.sgy");
        QVERIFY(cat->addAsset(f));
        EntityAssetLink ok;
        ok.entityType = QStringLiteral("well");
        ok.entityId = wellA;
        ok.assetId = astResolved;
        ok.role = QStringLiteral("well_log");
        QVERIFY(cat->addLink(ok));
        EntityAssetLink un;
        un.entityType = QStringLiteral("well");
        un.assetId = astPending;
        un.role = QStringLiteral("tops");
        un.unresolved = true;
        un.note = QStringLiteral("未匹配井名: Z9x");
        QVERIFY(cat->addLink(un));
    }
    DataPage *makePage(DataImportService *svc)
    {
        auto *page = new DataPage;
        // 门面父挂到 page——同生命周期，唯一测试路径不泄。
        auto *doc = new PreviewDocService(svc, page);
        page->setProperty("paleo.page.importsvc",
                          QVariant::fromValue<QObject *>(doc));
        page->refreshAssetTable();
        return page;
    }
};

// 模态对话框协作器：在下个事件拍抓 activeModalWidget 做动作（offscreen
// 下 exec 循环照样跑，QTimer::singleShot(0) 会进 exec 的事件分发）。
inline void driveModalNextTick(const std::function<void(QWidget *)> &drive)
{
    QTimer::singleShot(0, [drive] {
        if (QWidget *m = QApplication::activeModalWidget())
        {
            // 兜底只绑定本次对话框；不能在 3s 后误关后续用例的模态。
            QPointer<QWidget> guard(m);
            QTimer::singleShot(3000, m, [guard] {
                if (guard && guard->isVisible())
                    guard->close();
            });
            drive(m);
        }
    });
}

inline bool writeFile(const QString &path, const QByteArray &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content);
    return true;
}

inline int tableRowForPath(const QTableWidget *t, const QString &suffix)
{
    for (int r = 0; r < t->rowCount(); ++r)
        if (t->item(r, 0)->data(Qt::UserRole).toString().endsWith(suffix))
            return r;
    return -1;
}

} // namespace paleo::tests::dataops
