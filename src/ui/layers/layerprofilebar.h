// 层：视图
#pragma once

#include <QString>
#include <QWidget>

class QComboBox;
class QDialog;
class QLabel;
class QgisLayerProfileService;

// 图层 dock 头档案工具条：主题下拉（service->themes() 实时，"page:*" 显示
// 「页面·<中文页名>」）+「保存当前可见性为主题…」（名字输入后
// captureCurrentAsTheme）+「管理主题…」（应用/删除小对话框，bar 自持；
// 重命名经「先建新名再删旧名」的记录复制实现（QgsMapThemeCollection 无原生 rename API；主线5）+ 当前页档案
// 指示（setCurrentPage 后显示「页面档案：<中文页名>」；未知 id 原样、
// 空则空文案）。
// 只渲染 + 调 QgisLayerProfileService（不直碰 QgsProject/
// QgsMapThemeCollection）；offscreen 下对话框动作退化为无操作。
class LayerProfileBar : public QWidget
{
  Q_OBJECT

  public:
    explicit LayerProfileBar(QgisLayerProfileService *service, QWidget *parent = nullptr);

    // 壳侧 showPage 时同步：更新页档案指示，下拉若有对应 "page:" 主题则选中
    //（程序化路径，不发 themeSelected）。
    void setCurrentPage(const QString &pageId);
    QString currentPage() const;

    QComboBox *themeCombo() const; // 测试通道

    // 管理主题对话框（bar 自持：dialog 以 this 为父，随 bar 析构）。
    // buildManageDialog 每次新建（列表取当前 themes()），测试通道；
    // showManageDialog 是 exec 包装——offscreen 平台不 exec（直接返回）。
    QDialog *buildManageDialog();
    void showManageDialog();

  signals:
    // 用户在下拉里选了主题（bar 已调 service->applyTheme；程序化
    // setCurrentPage/刷新引起的选中变化不发）。
    void themeSelected(const QString &themeName);
    // 服务调用失败的可见提示（壳可接状态栏/消息条；不弹模态）。
    void statusMessage(const QString &text);

  private slots:
    // 用户激活路径（activated 信号）：applyTheme + themeSelected。
    // private slot：元对象可达（测试经 invokeMethod 驱动用户路径）。
    void onComboActivated(int index);

  private:
    // 主题下拉全量重建：ctor + service mapThemesChanged 触发；
    // "page:xxx" 项显示「页面·<中文页名>」；重建保住当前选中（若仍在列表）。
    void refreshThemeCombo();
    void updatePageLabel();
    // 「保存当前可见性为主题…」：offscreen no-op；否则 QInputDialog 取名
    //（空名拒绝）→ captureCurrentAsTheme，失败 QMessageBox。
    void saveCurrentAsThemeWithDialog();

    QgisLayerProfileService *m_service = nullptr;
    QComboBox *m_combo = nullptr;
    QLabel *m_pageLabel = nullptr;
    QString m_currentPage;
};
