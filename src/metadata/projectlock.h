#pragma once
#include <QString>
#include <memory>

class QLockFile;

// metadata/projectlock — 两实例同开一工程的写互斥（wave3/model-hardening；
// docs/SCHEMA_MIGRATION.md §6 决议：拒绝第二个写实例）。
//
// 机制：QLockFile 适配锁 <projectDir>/artifacts/metadata/.project.lock。
//   · 原子创建（O_EXCL）；staleLockTime=0 —— 锁文件里记录的持有者进程
//     已不存在时 tryLock 自动回收陈旧锁，崩溃不留死锁；
//   · 拒绝时错误带持有者（pid@hostname、application name）——用户能看懂
//     「另一个 Paleo 正在编辑这个工程」。
// 接线点（集成时一行接入）：AppContext 在 projectOpened 绑定写路径处
// tryLock，失败走现有错误面拒绝作为写实例打开；只读浏览不取锁不受影响。
class ProjectDirLock
{
  public:
    explicit ProjectDirLock( const QString &projectDir );
    ~ProjectDirLock(); // 释放锁（若持有）

    ProjectDirLock( const ProjectDirLock & ) = delete;
    ProjectDirLock &operator=( const ProjectDirLock & ) = delete;

    // 尝试取锁。已在别的实例手里 → false + *error（含持有者信息）。
    bool tryLock( QString *error = nullptr );

    // 显式释放；未持有时为空操作。
    void unlock();

    bool isHeld() const { return m_held; }
    QString lockPath() const { return m_lockPath; }

  private:
    QString m_lockPath;
    std::unique_ptr<QLockFile> m_lockFile;
    bool m_held = false;
};
