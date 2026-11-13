// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/log.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QUtf8StringView>

#include <chrono>
#include <mutex>

namespace riley::dock::app {

QString homePath(const char *variable, const QString &fallback)
{
    if (qEnvironmentVariableIsSet(variable))
        return qEnvironmentVariable(variable);
    return QDir(qEnvironmentVariable("HOME")).filePath(fallback);
}

void log(const QString &message)
{
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    const QString path = QDir(homePath("XDG_STATE_HOME", QStringLiteral(".local/state")))
                             .filePath(QStringLiteral("riley/dock.log"));
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append))
        file.write(message.toUtf8() + '\n');
}

double monotonicSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

QString readTextOrEmpty(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || !QUtf8StringView(bytes).isValidUtf8())
        return {};
    return QString::fromUtf8(bytes);
}

} // namespace riley::dock::app
