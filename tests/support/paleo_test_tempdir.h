// 层：测试壳
#pragma once
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

// #641：测试夹具把文件写成零权限/只读（setPermissions）验证「不可读/不可写」
// 行为后，Windows 上 QTemporaryDir 的递归删除对只读文件会失败——临时目录
// 整树残留（实测 build/paleo-tmp 堆积 2.7k+ 目录，单 tst_import 留 561 个），
// 每个残留再刷一行 QWARN。守卫在 QTemporaryDir 析构前把属主写位补回，
// 夹具内的权限清零语义不受影响（恢复只发生在析构期）。POSIX 侧删除本就
// 能成功，守卫是无害幂等。
//
// 用法：把 `QTemporaryDir tmp;` 换成 `PaleoTempDir tmp;` 即可——隐式转换
// 到 const QTemporaryDir& 兼容 stageFixture(tmp, …) 一类签名。
class PaleoTempDir
{
  public:
    PaleoTempDir() = default;

    bool isValid() const { return m_dir.isValid(); }
    QString path() const { return m_dir.path(); }
    QString errorString() const { return m_dir.errorString(); }
    QString filePath(const QString &fileName) const
    {
      return m_dir.filePath(fileName);
    }
    bool remove()
    {
      restoreWritePermissions(m_dir.path());
      return m_dir.remove();
    }
    operator const QTemporaryDir &() const { return m_dir; }

    ~PaleoTempDir()
    {
      restoreWritePermissions(m_dir.path());
      // m_dir 随后析构删除目录
    }

  private:
    static void restoreWritePermissions(const QString &root)
    {
      if (root.isEmpty() || !QFileInfo::exists(root))
        return;
      QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot,
                      QDirIterator::Subdirectories);
      while (it.hasNext()) {
        const QFileInfo info(it.next());
        if (!info.isWritable())
          QFile::setPermissions(info.absoluteFilePath(),
                                info.permissions() | QFileDevice::WriteOwner);
      }
    }

    QTemporaryDir m_dir;
};
