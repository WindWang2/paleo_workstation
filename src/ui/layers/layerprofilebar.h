// 层：视图
#pragma once

#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QgisLayerProfileService;

// 图层 dock 头档案工具条：主题下拉（QgsMapThemeCollection::mapThemes 实时）
// +「保存当前可见性为主题…」（名字输入后 captureCurrentAsTheme）
// +「管理主题…」（重命名/删除小对话框，bar 自持）
// + 当前页档案指示（setCurrentPage 后显示「页面档案：<pageId>」）。
// 只渲染 + 调 QgisLayerProfileService；offscreen 下对话框动作退化为无操作。
class LayerProfileBar : public QWidget
{
  Q_OBJECT

  public:
    explicit LayerProfileBar(QgisLayerProfileService *service, QWidget *parent = nullptr);

    // 壳侧 showPage 时同步：更新页档案指示，下拉若有对应 "page:" 主题则选中。
    void setCurrentPage(const QString &pageId);
    QString currentPage() const;

    QComboBox *themeCombo() const; // 测试通道

  signals:
    // 用户在下拉里选了主题（bar 已调 service->applyTheme）
    void themeSelected(const QString &themeName);

  private:
    QgisLayerProfileService *m_service = nullptr;
    QComboBox *m_combo = nullptr;
    QLabel *m_pageLabel = nullptr;
    QString m_currentPage;
};
