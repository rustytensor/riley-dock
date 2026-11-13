// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/filewatch.h"

#include "app/log.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <sys/stat.h>

namespace riley::dock::app {

FileWatch::FileWatch(QString path, QObject *parent) : QObject(parent), m_path(std::move(path))
{
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &FileWatch::check);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &FileWatch::check);
    m_signature = signature();
    rearm();
}

QByteArray FileWatch::signature() const
{
    struct stat st {};
    if (::stat(QFile::encodeName(m_path).constData(), &st) != 0)
        return QByteArrayLiteral("missing");
    return QByteArray::number(quint64(st.st_dev)) + ':' + QByteArray::number(quint64(st.st_ino)) + ':'
           + QByteArray::number(qint64(st.st_size)) + ':' + QByteArray::number(qint64(st.st_mtim.tv_sec)) + '.'
           + QByteArray::number(qint64(st.st_mtim.tv_nsec)) + ':' + QByteArray::number(qint64(st.st_ctim.tv_sec))
           + '.' + QByteArray::number(qint64(st.st_ctim.tv_nsec));
}

void FileWatch::rearm()
{
    const QStringList old = m_watcher.files() + m_watcher.directories();
    if (!old.isEmpty())
        m_watcher.removePaths(old);
    if (QFileInfo(m_path).isFile())
        m_watcher.addPath(m_path);
    // The nearest existing folder above the file: a replace by rename, or the folder being created, shows there.
    QDir folder = QFileInfo(m_path).absoluteDir();
    while (!folder.exists() && !folder.isRoot())
        folder.setPath(QFileInfo(folder.absolutePath()).path());
    if (folder.exists())
        m_watcher.addPath(folder.absolutePath());
}

void FileWatch::check()
{
    rearm();
    const QByteArray now = signature();
    if (now == m_signature)
        return;
    m_signature = now;
    Q_EMIT changed();
}

DesktopEntriesWatch::DesktopEntriesWatch(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        rearm();
        Q_EMIT changed();
    });
    rearm();
}

void DesktopEntriesWatch::rearm()
{
    QStringList folders{QDir(homePath("XDG_DATA_HOME", QStringLiteral(".local/share")))
                            .filePath(QStringLiteral("applications"))};
    QString dataDirs = qEnvironmentVariable("XDG_DATA_DIRS");
    if (dataDirs.isEmpty())
        dataDirs = QStringLiteral("/usr/local/share:/usr/share");
    for (const QString &dir : dataDirs.split(u':', Qt::SkipEmptyParts))
        folders.append(QDir(dir).filePath(QStringLiteral("applications")));
    const QStringList old = m_watcher.directories();
    QStringList wanted;
    for (const QString &folder : folders) {
        if (QFileInfo(folder).isDir() && !old.contains(folder) && !wanted.contains(folder))
            wanted.append(folder);
    }
    if (!wanted.isEmpty())
        m_watcher.addPaths(wanted);
}

} // namespace riley::dock::app
