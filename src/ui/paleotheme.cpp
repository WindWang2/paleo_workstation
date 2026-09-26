#include "paleotheme.h"

#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

// qrc 对象住在静态库 paleo_core 里——链接器不会自动拉入无引用的目标文件，
// 显式引用其初始化符号把字体资源钉进每个最终二进制。
static const bool kFontsResourceLinked = [] {
  Q_INIT_RESOURCE(paleo_fonts);
  return true;
}();

// DESIGN.md 唯一视觉权威；这里的字面量就是 token 的落地处，别处引用本头。
namespace
{
  // qrc 前缀 :/paleo/fonts（resources/paleo_fonts.qrc）。
  struct FontSpec
  {
    const char *path;
    const char *family;
  };
  const FontSpec kVendoredFonts[] = {
      {":/paleo/fonts/NotoSansSC-Regular.otf", "Noto Sans SC"},
      {":/paleo/fonts/NotoSansSC-Bold.otf", "Noto Sans SC"},
      {":/paleo/fonts/JetBrainsMono-Regular.ttf", "JetBrains Mono"},
      {":/paleo/fonts/JetBrainsMono-Bold.ttf", "JetBrains Mono"},
  };

  // DESIGN.md status-tag：浅色底 + 深色字。Neutral（未计算）用 surface-alt
  // 底 + text-muted 字 + border 描边——中性状态不占语义色。
  struct CapsuleColors
  {
    const char *bg;
    const char *fg;
    const char *border; // 空串 = 无描边
  };
  CapsuleColors capsuleColors(PaleoTheme::CapsuleKind kind)
  {
    switch (kind)
    {
      case PaleoTheme::CapsuleKind::Success: return {"#E8F5E9", "#43A047", ""};
      case PaleoTheme::CapsuleKind::Warning: return {"#FFF4E0", "#F29900", ""};
      case PaleoTheme::CapsuleKind::Error: return {"#FDEBEB", "#E53935", ""};
      case PaleoTheme::CapsuleKind::Neutral:
      default: return {"#EDF1F5", "#5D6E80", "#DFE5EC"};
    }
  }
} // namespace

namespace PaleoTheme
{
  bool ensureApplicationFonts()
  {
    bool all = true;
    for (const FontSpec &spec : kVendoredFonts)
    {
      const int id = QFontDatabase::addApplicationFont(QLatin1String(spec.path));
      if (id < 0)
      {
        all = false;
        qWarning("paleo: vendored font %s failed to register — falling back to "
                 "system fonts (Windows: Microsoft YaHei UI, Linux: Noto Sans CJK SC)",
                 spec.path);
      }
    }
    if (all)
      qInfo("paleo: vendored fonts registered (Noto Sans SC + JetBrains Mono)");
    return all;
  }

  QFont bodyFont()
  {
    QFont f;
    // DESIGN.md 退化链：vendor → 平台 CJK → 微软雅黑。
    f.setFamilies({QStringLiteral("Noto Sans SC"),
                   QStringLiteral("Noto Sans CJK SC"),
                   QStringLiteral("Microsoft YaHei UI")});
    f.setPointSize(kBodyPt);
    return f;
  }

  QFont monoFont()
  {
    QFont f;
    f.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
    f.setPointSize(kMonoPt);
    f.setStyleHint(QFont::TypeWriter);
    return f;
  }

  QString focusRingStyleSheet()
  {
    // 2px #1B73D0 键盘焦点环（DESIGN.md focus-ring = primary，替代 Fusion
    // 虚线框）。QTableView/QTreeView 的环画在视口框上，避免与选区混淆。
    return QStringLiteral(
        "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus,"
        "QTableView:focus, QTreeView:focus, QListView:focus, QListWidget:focus,"
        "QPushButton:focus, QToolButton:focus, QTabBar:focus,"
        "QTextEdit:focus, QPlainTextEdit:focus { border: 2px solid #1B73D0; }");
  }

  QString capsuleStyleSheet(CapsuleKind kind)
  {
    const CapsuleColors c = capsuleColors(kind);
    return QStringLiteral(
               "background: %1; color: %2; border-radius: 4px; padding: 1px 8px;")
               .arg(QLatin1String(c.bg), QLatin1String(c.fg)) +
           (c.border[0] ? QStringLiteral(" border: 1px solid %1;").arg(QLatin1String(c.border))
                        : QString());
  }

  QLabel *capsuleLabel(const QString &text, CapsuleKind kind, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("statusCapsule"));
    l->setProperty("capsuleKind", static_cast<int>(kind));
    l->setStyleSheet(capsuleStyleSheet(kind));
    l->setAlignment(Qt::AlignCenter);
    return l;
  }

  // DESIGN.md 浅色 token 的 QPalette 落地：surface/surface-alt/text/
  // text-muted/primary。显式写 Active+Inactive+Disabled 三组——不写的组
  // 会继承系统 palette（暗色系统里浅色面板上出浅色字）。
  QPalette designLightPalette()
  {
    const QColor surface(QStringLiteral("#FFFFFF"));
    const QColor surfaceAlt(QStringLiteral("#EDF1F5"));
    const QColor text(QStringLiteral("#24303E"));
    const QColor muted(QStringLiteral("#5D6E80"));
    const QColor primary(QStringLiteral("#1B73D0"));

    QPalette p;
    const auto set = [&p](QPalette::ColorRole role, const QColor &active,
                          const QColor &disabled) {
      p.setColor(QPalette::Active, role, active);
      p.setColor(QPalette::Inactive, role, active);
      p.setColor(QPalette::Disabled, role, disabled);
    };
    set(QPalette::Window, surface, surface);
    set(QPalette::WindowText, text, muted);
    set(QPalette::Base, surface, surface);
    set(QPalette::AlternateBase, surfaceAlt, surfaceAlt);
    set(QPalette::Text, text, muted);
    set(QPalette::Button, surfaceAlt, surfaceAlt);
    set(QPalette::ButtonText, text, muted);
    set(QPalette::ToolTipBase, surface, surface);
    set(QPalette::ToolTipText, text, muted);
    set(QPalette::PlaceholderText, muted, muted);
    set(QPalette::Highlight, primary, primary.darker(115));
    set(QPalette::HighlightedText, surface, surface);
    set(QPalette::Link, primary, muted);
    set(QPalette::LinkVisited, primary.darker(110), muted);
    set(QPalette::BrightText, QColor(QStringLiteral("#E53935")), muted);
    return p;
  }

  void applyLightTheme()
  {
    ensureApplicationFonts();
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setPalette(designLightPalette());
    QApplication::setFont(bodyFont());
  }

  void pinRenderEnvironment()
  {
    // 渲染回归（TODOS P1）：跨平台噪点 = 字体替换 + 平台样式 + palette 差异。
    // 与应用同一条路径钉死（Fusion + 浅色 palette + vendor 字体）。
    applyLightTheme();
  }
} // namespace PaleoTheme
