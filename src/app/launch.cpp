// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/launch.h"

#include "app/log.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>

namespace riley::dock::app {

namespace {

// g_shell_quote: single quotes, with each ' written as '\''.
QString shellQuote(const QString &text)
{
    QString quoted = QStringLiteral("'");
    for (const QChar c : text) {
        if (c == u'\'')
            quoted += QStringLiteral("'\\''");
        else
            quoted += c;
    }
    return quoted + u'\'';
}

// g_shell_parse_argv: words split at unquoted blanks and newlines; single quotes keep everything; double quotes
// keep everything but a backslash before $ ` " \ or newline; an unquoted backslash escapes the next character (a
// backslash-newline is removed); '#' at the start of a word starts a comment. An unterminated quote fails.
std::optional<QStringList> shellParse(const QString &line)
{
    QStringList words;
    QString word;
    bool inWord = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (c == u' ' || c == u'\t' || c == u'\n') {
            if (inWord)
                words.append(word);
            word.clear();
            inWord = false;
        } else if (c == u'#' && !inWord) {
            while (i < line.size() && line[i] != u'\n')
                ++i;
        } else if (c == u'\'') {
            const qsizetype end = line.indexOf(u'\'', i + 1);
            if (end < 0)
                return std::nullopt;
            word += line.mid(i + 1, end - i - 1);
            i = end;
            inWord = true;
        } else if (c == u'"') {
            ++i;
            for (;; ++i) {
                if (i >= line.size())
                    return std::nullopt;
                const QChar d = line[i];
                if (d == u'"')
                    break;
                if (d == u'\\' && i + 1 < line.size()) {
                    const QChar e = line[i + 1];
                    if (e == u'$' || e == u'`' || e == u'"' || e == u'\\') {
                        word += e;
                        ++i;
                        continue;
                    }
                    if (e == u'\n') {
                        ++i;
                        continue;
                    }
                }
                word += d;
            }
            inWord = true;
        } else if (c == u'\\') {
            if (i + 1 < line.size()) {
                if (line[i + 1] != u'\n')
                    word += line[i + 1];
                ++i;
            }
            inWord = true;
        } else {
            word += c;
            inWord = true;
        }
    }
    if (inWord)
        words.append(word);
    return words;
}

} // namespace

std::optional<QStringList> launchArguments(const desktop::AppInfo &info)
{
    QString expanded;
    const QString &exec = info.exec;
    for (qsizetype i = 0; i < exec.size(); ++i) {
        const QChar c = exec[i];
        if (c != u'%' || i + 1 >= exec.size()) {
            expanded += c;
            continue;
        }
        const QChar code = exec[++i];
        switch (code.unicode()) {
        case u'%': expanded += u'%'; break;
        case u'i':
            if (!info.icon.isEmpty())
                expanded += QStringLiteral("--icon ") + shellQuote(info.icon);
            break;
        case u'c': expanded += shellQuote(info.name); break;
        case u'k': expanded += shellQuote(info.filename); break;
        default: break; // %f %F %u %U with no files, and the deprecated codes: nothing
        }
    }
    const auto words = shellParse(expanded);
    if (!words || words->isEmpty())
        return std::nullopt;
    return words;
}

bool launchApp(const desktop::AppInfo &info, QString *error)
{
    const auto words = launchArguments(info);
    if (!words) {
        if (error)
            *error = QStringLiteral("%1 has no usable Exec line").arg(info.id);
        return false;
    }
    QProcess process;
    process.setProgram(words->front());
    process.setArguments(words->mid(1));
    process.setStandardInputFile(QProcess::nullDevice());
    if (!process.startDetached()) {
        if (error)
            *error = QStringLiteral("cannot start %1").arg(words->front());
        return false;
    }
    return true;
}

void runLogged(const QString &program, const QStringList &args, const QString &what, bool logExit)
{
    const QString label = what.isEmpty() ? program : what;
    auto *process = new QProcess(QCoreApplication::instance());
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    QObject::connect(process, &QProcess::finished, process, [process, label, logExit](int code, QProcess::ExitStatus status) {
        if (logExit && (status != QProcess::NormalExit || code != 0))
            log(QStringLiteral("%1 failed").arg(label));
        process->deleteLater();
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, label](QProcess::ProcessError failure) {
        if (failure != QProcess::FailedToStart)
            return;
        log(QStringLiteral("%1: %2").arg(label, process->errorString()));
        process->deleteLater();
    });
    process->start(program, args);
}

} // namespace riley::dock::app
