// tests/tst_wellattachmentpanel.cpp — 方向 79 井附件管理面板 offscreen 验收：
//   · 清单如实：已锚定/未锚定/未决三态全列（未锚定正是补锚价值面）
//   · 行内编辑：合法改值 round-trip（catalog extra + #history/#source 审计）；
//     非法输入拒收列因（状态行文案）并回滚；空输入不是清锚
//   · 清除锚定：显式按钮 → extra 删除、#history 保留
//   · 移除 = 软删意图信号（磁盘文件仍在——断言）
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

#include "../src/catalog/datacatalog.h"
#include "../src/ui/pages/wellattachmentpanel.h"
#include "../src/ui/pages/dataops/dataopsmodel.h"

#include <cmath>

namespace
{

// 一井三附件：deep(锚,来源文件名) / noanchor(无锚) / pend(unresolved——
// 排除探针：unresolved 链接 entityId 为空，不属任何井，面板不列)。
struct Fixture
{
  QTemporaryDir dir;
  DataCatalog cat;
  QString err;
  QString imgDeep, imgNone;

  bool build()
  {
    if (!dir.isValid() || !cat.open(dir.path(), &err))
      return false;
    CatalogEntity e;
    e.id = QStringLiteral("well-1");
    e.entityType = QStringLiteral("well");
    e.name = QStringLiteral("A1");
    if (!cat.addEntity(e, &err))
      return false;
    const auto writePng = [this](const QString &name, QRgb color) {
      QImage img(8, 8, QImage::Format_RGB32);
      img.fill(color);
      const QString p = dir.filePath(name);
      return img.save(p, "PNG") ? p : QString();
    };
    imgDeep = writePng(QStringLiteral("deep.png"), qRgb(200, 30, 30));
    imgNone = writePng(QStringLiteral("none.png"), qRgb(30, 200, 30));
    const QString imgPend = writePng(QStringLiteral("pend.png"), qRgb(30, 30, 200));
    if (imgDeep.isEmpty() || imgNone.isEmpty() || imgPend.isEmpty())
      return false;
    const auto addAttachment =
        [this](const QString &assetId, const QString &path,
               const QVariant &depth, bool unresolved,
               const QString &source) {
          CatalogAsset a;
          a.id = assetId;
          a.type = QStringLiteral("image_reference");
          a.displayName = QFileInfo(path).fileName();
          if (!cat.addAsset(a, &err))
            return false;
          CatalogVersion v;
          v.id = QStringLiteral("v-") + assetId;
          v.assetId = assetId;
          v.managed = false;
          v.path = path;
          v.fileName = a.displayName;
          v.stage = QStringLiteral("RAW");
          if (depth.isValid())
          {
            v.extra.insert(QStringLiteral("depthMd"), depth);
            if (!source.isEmpty())
              v.extra.insert(QStringLiteral("depthMd#source"), source);
          }
          if (!cat.addVersion(v, &err))
            return false;
          EntityAssetLink l;
          l.entityId = unresolved ? QString() : QStringLiteral("well-1");
          l.entityType = QStringLiteral("well");
          l.role = QStringLiteral("core");
          l.assetId = assetId;
          l.isPrimary = false;
          l.unresolved = unresolved;
          return cat.addLink(l, &err);
        };
    return addAttachment(QStringLiteral("ast-deep"), imgDeep, 1849.35, false,
                         QStringLiteral("filename")) &&
           addAttachment(QStringLiteral("ast-none"), imgNone, {}, false,
                         QString()) &&
           addAttachment(QStringLiteral("ast-pend"), imgPend, 1900.0, true,
                         QStringLiteral("filename"));
  }
};

QLabel *statusOf(const WellAttachmentPanel &p)
{
  return p.findChild<QLabel *>(QStringLiteral("wellAttachmentStatus"));
}
QTableWidget *tableOf(const WellAttachmentPanel &p)
{
  return p.findChild<QTableWidget *>(QStringLiteral("wellAttachmentTable"));
}
QComboBox *wellBoxOf(const WellAttachmentPanel &p)
{
  return p.findChild<QComboBox *>(QStringLiteral("wellAttachmentWellBox"));
}

// 模态协作（tst_panels 先例）：offscreen 下 exec 循环照样跑，下一拍抓
// activeModalWidget 点确认角色按钮（accept() 的 result 是 QDialog::Accepted
// ≠ QMessageBox::Yes，ask 会判否——必须真点按钮）。3s 兜底关闭防误伤后续。
void acceptModalNextTick()
{
  QTimer::singleShot(0, [] {
    if (QWidget *m = QApplication::activeModalWidget())
    {
      QPointer<QWidget> guard(m);
      QTimer::singleShot(3000, m, [guard] {
        if (guard && guard->isVisible())
          guard->close();
      });
      if (auto *box = qobject_cast<QMessageBox *>(m))
      {
        for (QPushButton *b : box->findChildren<QPushButton *>())
        {
          const auto role = box->buttonRole(b);
          if (role == QMessageBox::YesRole || role == QMessageBox::AcceptRole)
          {
            b->click();
            return;
          }
        }
      }
      m->close();
    }
  });
}

} // namespace

class TestWellAttachmentPanel : public QObject
{
  Q_OBJECT
  private slots:
    void crudRoundTrip()
    {
      Fixture fx;
      QVERIFY2(fx.build(), qPrintable(fx.err));
      paleo::dataops::RecycleBin recycle;
      recycle.load(&fx.cat);

      WellAttachmentPanel panel(&fx.cat, fx.dir.path(), &recycle);
      panel.setWell(QStringLiteral("well-1"));
      QTableWidget *table = tableOf(panel);
      QVERIFY(table != nullptr);
      QCOMPARE(wellBoxOf(panel)->currentData().toString(),
               QStringLiteral("well-1"));

      // ---- R：两行全列（已锚定 + 未锚定——管理面如实）。第三张未决附件
      // 不在本面板：unresolved 链接 entityId 为空，不属任何井（归未决
      // 链接归位流程），linksForEntity 永不返回。
      QCOMPARE(table->rowCount(), 2);
      // 行序契约：有锚在前，未锚定殿后。
      const auto rowOfAsset = [table](const QString &fileName) {
        for (int r = 0; r < table->rowCount(); ++r)
          if (table->item(r, 1)->text() == fileName)
            return r;
        return -1;
      };
      const int rDeep0 = rowOfAsset(QStringLiteral("deep.png"));
      const int rNone0 = rowOfAsset(QStringLiteral("none.png"));
      QVERIFY(rDeep0 >= 0 && rNone0 >= 0);
      QCOMPARE(table->item(rDeep0, 2)->text(), QStringLiteral("1849.35"));
      QCOMPARE(table->item(rNone0, 2)->text(), QString()); // 未锚定空文本
      // 状态两态。
      QCOMPARE(table->item(rDeep0, 6)->text(), QStringLiteral("已锚定"));
      QCOMPARE(table->item(rNone0, 6)->text(), QStringLiteral("未锚定"));

      // ---- U：行内合法改值 → catalog round-trip + 审计。编辑经 0ms deferral
      // 落库（避免 itemChanged 再入），断言前泵事件循环。每次成功编辑触发
      // refresh（catalog changed）重建表——行号每步重新定位。
      const auto pump = [] { QApplication::processEvents(); };
      const auto rowOfFile = [table](const QString &fileName) {
        for (int r = 0; r < table->rowCount(); ++r)
          if (table->item(r, 1)->text() == fileName)
            return r;
        return -1;
      };
      table->item(rowOfFile(QStringLiteral("deep.png")), 2)
          ->setText(QStringLiteral("2000.5"));
      pump();
      {
        const CatalogVersion v =
            fx.cat.versionById(QStringLiteral("v-ast-deep"));
        QCOMPARE(v.extra.value(QStringLiteral("depthMd")).toDouble(), 2000.5);
        QCOMPARE(v.extra.value(QStringLiteral("depthMd#source")).toString(),
                 QStringLiteral("manual"));
        const QVariantList hist =
            v.extra.value(QStringLiteral("depthMd#history")).toList();
        QCOMPARE(hist.size(), 1);
        QCOMPARE(hist.at(0).toMap().value(QStringLiteral("v")).toDouble(),
                 1849.35);
      }
      QVERIFY(statusOf(panel)->text().contains(QStringLiteral("2000.5")));

      // ---- U：非法输入拒收列因 + 回滚（拒收不触发 refresh，行号稳定）。
      const int rDeep = rowOfFile(QStringLiteral("deep.png"));
      QVERIFY(rDeep >= 0);
      table->item(rDeep, 2)->setText(QStringLiteral("abc"));
      pump();
      QCOMPARE(table->item(rDeep, 2)->text(), QStringLiteral("2000.50"));
      QVERIFY(!statusOf(panel)->text().isEmpty());
      table->item(rDeep, 2)->setText(QStringLiteral("-3"));
      pump();
      QCOMPARE(table->item(rDeep, 2)->text(), QStringLiteral("2000.50"));
      table->item(rDeep, 2)->setText(QStringLiteral("100ft"));
      pump();
      QCOMPARE(table->item(rDeep, 2)->text(), QStringLiteral("2000.50"));
      {
        const CatalogVersion v =
            fx.cat.versionById(QStringLiteral("v-ast-deep"));
        QCOMPARE(v.extra.value(QStringLiteral("depthMd")).toDouble(), 2000.5);
      }

      // ---- U：空输入不是清锚（提示 + 回滚）。
      table->item(rDeep, 2)->setText(QString());
      pump();
      QCOMPARE(table->item(rDeep, 2)->text(), QStringLiteral("2000.50"));
      QVERIFY(statusOf(panel)->text().contains(QStringLiteral("清除锚定")));

      // ---- U：未锚定行补锚（本方向价值面）。
      table->item(rowOfFile(QStringLiteral("none.png")), 2)
          ->setText(QStringLiteral("31868.62"));
      pump();
      {
        const CatalogVersion v =
            fx.cat.versionById(QStringLiteral("v-ast-none"));
        QCOMPARE(v.extra.value(QStringLiteral("depthMd")).toDouble(), 31868.62);
        QCOMPARE(v.extra.value(QStringLiteral("depthMd#source")).toString(),
                 QStringLiteral("manual"));
      }

      // ---- D 面：清除锚定按钮（offscreen 确认自动应答 true）。清除后
      // 行序变化（deep 降为未锚定），移除步重新定位。
      table->selectRow(rowOfFile(QStringLiteral("deep.png")));
      // 选中后按钮可用性。
      auto *clearBtn = panel.findChild<QPushButton *>(
          QStringLiteral("wellAttachmentClearBtn"));
      QVERIFY(clearBtn != nullptr && clearBtn->isEnabled());
      acceptModalNextTick(); // 确认框（offscreen 模态协作）
      clearBtn->click();
      {
        const CatalogVersion v =
            fx.cat.versionById(QStringLiteral("v-ast-deep"));
        QVERIFY(!v.extra.contains(QStringLiteral("depthMd")));
        QVERIFY(!v.extra.contains(QStringLiteral("depthMd#source")));
        QVERIFY(v.extra.contains(QStringLiteral("depthMd#history")));
      }

      // ---- D 面：移除 = 软删意图信号（文件不删——断言）。
      QSignalSpy removeSpy(&panel, &WellAttachmentPanel::removeRequested);
      table->selectRow(rowOfFile(QStringLiteral("none.png")));
      auto *removeBtn = panel.findChild<QPushButton *>(
          QStringLiteral("wellAttachmentRemoveBtn"));
      QVERIFY(removeBtn != nullptr && removeBtn->isEnabled());
      removeBtn->click();
      QCOMPARE(removeSpy.size(), 1);
      QCOMPARE(removeSpy.at(0).at(0).toStringList(),
               QStringList{QStringLiteral("ast-none")});
      QVERIFY(QFile::exists(fx.imgNone)); // 版本级操作，磁盘文件保留

      // ---- 重开 round-trip：catalog 落盘后重开，编辑值仍在。
      QVERIFY2(fx.cat.open(fx.dir.path(), &fx.err), qPrintable(fx.err));
      const CatalogVersion v2 = fx.cat.versionById(QStringLiteral("v-ast-none"));
      QCOMPARE(v2.extra.value(QStringLiteral("depthMd")).toDouble(), 31868.62);
      QVERIFY(!fx.cat.versionById(QStringLiteral("v-ast-deep"))
                   .extra.contains(QStringLiteral("depthMd")));
    }
};

QTEST_MAIN(TestWellAttachmentPanel)
#include "tst_wellattachmentpanel.moc"
