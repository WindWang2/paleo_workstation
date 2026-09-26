#include <QtTest>
#include <QApplication>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QStyleFactory>

#include "../src/ui/paleotheme.h"

// wave3/ux-consistency T32 — DESIGN.md token 的代码出口（PaleoTheme）契约：
// vendor 字体注册、mono/正文字体、2px #1B73D0 焦点环、状态胶囊 token、
// 渲染回归稳定化（字体 + Fusion 钉死 + 同帧两次渲染逐字节一致）。
class TestUxTheme : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase()
    {
      QVERIFY2(PaleoTheme::ensureApplicationFonts(), "vendored fonts must register");
    }

    // vendor 字体真的进了 QFontDatabase（不是系统字体顶包）。
    void vendoredFontsRegistered()
    {
      const QStringList families = QFontDatabase::families();
      QVERIFY2(families.contains(QStringLiteral("Noto Sans SC")),
               "Noto Sans SC (vendored) missing from QFontDatabase");
      QVERIFY2(families.contains(QStringLiteral("JetBrains Mono")),
               "JetBrains Mono (vendored) missing from QFontDatabase");
    }

    // mono 数字面：JetBrains Mono 9pt（DESIGN.md mono token），解析命中
    // vendor 家族而非系统替换。
    void monoFontIsJetBrainsNinePt()
    {
      const QFont f = PaleoTheme::monoFont();
      QCOMPARE(f.pointSize(), PaleoTheme::kMonoPt);
      const QFontInfo info(f);
      QVERIFY2(info.family() == QStringLiteral("JetBrains Mono"),
               qPrintable(QStringLiteral("resolved family: ") + info.family()));
      // tnum 等宽：数字位宽一致（同一字符串里 1 和 8 宽度相同）。
      QFontMetricsF fm(f);
      const qreal w1 = fm.horizontalAdvance(QStringLiteral("1111"));
      const qreal w8 = fm.horizontalAdvance(QStringLiteral("8888"));
      QCOMPARE(w1, w8);
    }

    // 正文字体：Noto Sans SC 9pt（DESIGN.md body token）。
    void bodyFontIsNotoSansScNinePt()
    {
      const QFont f = PaleoTheme::bodyFont();
      QCOMPARE(f.pointSize(), PaleoTheme::kBodyPt);
      const QFontInfo info(f);
      QVERIFY2(info.family() == QStringLiteral("Noto Sans SC"),
               qPrintable(QStringLiteral("resolved family: ") + info.family()));
    }

    // 焦点环：2px #1B73D0，覆盖可聚焦控件（DESIGN.md focus-ring token）。
    void focusRingCoversFocusableWidgets()
    {
      const QString css = PaleoTheme::focusRingStyleSheet();
      QVERIFY(css.contains(QStringLiteral("2px solid #1B73D0")));
      for (const char *sel : {"QLineEdit:focus", "QComboBox:focus", "QTableView:focus",
                              "QPushButton:focus", "QSpinBox:focus", "QTreeView:focus"})
        QVERIFY2(css.contains(QLatin1String(sel)),
                 qPrintable(QStringLiteral("focus ring misses %1").arg(QString::fromLatin1(sel))));
    }

    // 状态胶囊：DESIGN.md status-tag token 逐色断言（浅底 + 深语义字）。
    void capsuleTokensMatchDesignMd()
    {
      using K = PaleoTheme::CapsuleKind;
      const struct { K kind; const char *bg; const char *fg; } cases[] = {
          {K::Success, "#E8F5E9", "#43A047"},
          {K::Warning, "#FFF4E0", "#F29900"},
          {K::Error, "#FDEBEB", "#E53935"},
          {K::Neutral, "#EDF1F5", "#5D6E80"},
      };
      for (const auto &c : cases)
      {
        const QString css = PaleoTheme::capsuleStyleSheet(c.kind);
        QVERIFY2(css.contains(c.bg),
                 qPrintable(QStringLiteral("capsule bg %1 missing").arg(c.bg)));
        QVERIFY2(css.contains(c.fg),
                 qPrintable(QStringLiteral("capsule fg %1 missing").arg(c.fg)));
        QVERIFY(css.contains(QStringLiteral("border-radius"))); // 胶囊圆角
      }
    }

    void capsuleLabelCarriesKindAndCentering()
    {
      using K = PaleoTheme::CapsuleKind;
      auto *l = PaleoTheme::capsuleLabel(QStringLiteral("通过"), K::Success, nullptr);
      QVERIFY(l);
      QCOMPARE(l->objectName(), QStringLiteral("statusCapsule"));
      QCOMPARE(l->text(), QStringLiteral("通过"));
      QCOMPARE(l->property("capsuleKind").toInt(), static_cast<int>(K::Success));
      QVERIFY(l->styleSheet().contains(QStringLiteral("#E8F5E9")));
      QVERIFY(l->alignment() & Qt::AlignHCenter);
      delete l;
    }

    // 渲染稳定化：Fusion + vendor 字体钉住后，同帧两次渲染逐字节一致
    // （跨平台字体噪点消除的基线条件）。
    void renderEnvironmentIsPinnedAndDeterministic()
    {
      PaleoTheme::pinRenderEnvironment();
      QCOMPARE(qApp->style()->objectName().compare(QLatin1String("Fusion"),
                                                   Qt::CaseInsensitive),
               0);
      QCOMPARE(qApp->font().pointSize(), PaleoTheme::kBodyPt);

      const auto render = [] {
        QLabel l(QStringLiteral("残差 12.5 ms · 井 A1"));
        l.setFont(PaleoTheme::monoFont());
        l.resize(220, 40);
        QImage img(l.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        l.render(&p);
        return img;
      };
      const QImage a = render();
      const QImage b = render();
      QCOMPARE(a, b); // QImage operator== 逐像素
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestUxTheme tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_uxtheme.moc"
