// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/xmlfile.h"

#include "app/log.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlschemas.h>

#include <cstdio>
#include <memory>
#include <mutex>

#ifndef RILEY_DOCK_PROGRAM
#define RILEY_DOCK_PROGRAM "riley-dock-qt"
#endif

namespace riley::dock::app {

namespace {

constexpr qint64 MAX_BYTES = 1 << 20; // docs/files.md section 4, "Reading the XML files": 1 MiB

// The first error libxml2 reports; warnings are not errors.
struct FirstError {
    bool set = false;
    XmlError error;
};

void collect(void *user, const xmlError *error)
{
    auto *first = static_cast<FirstError *>(user);
    if (first->set || !error || error->level < XML_ERR_ERROR)
        return;
    first->set = true;
    first->error.line = error->line > 0 ? error->line : 0;
    first->error.message = QString::fromUtf8(error->message ? error->message : "unknown error").trimmed();
}

// No document may load anything from outside itself: no external DTD, entity or XInclude.
xmlParserErrors refuseLoad(void *, const char *, const char *, xmlResourceType, xmlParserInputFlags,
                           xmlParserInput **out)
{
    *out = nullptr;
    return XML_IO_EACCES;
}

struct DocFree {
    void operator()(xmlDoc *doc) const { xmlFreeDoc(doc); }
};
struct CtxtFree {
    void operator()(xmlParserCtxt *ctxt) const { xmlFreeParserCtxt(ctxt); }
};
struct ValidFree {
    void operator()(xmlSchemaValidCtxt *ctxt) const { xmlSchemaFreeValidCtxt(ctxt); }
};

// The compiled schemas, one per resource, kept for the life of the process.
xmlSchema *schemaFor(const QString &resource)
{
    static QHash<QString, xmlSchema *> schemas;
    if (const auto found = schemas.constFind(resource); found != schemas.constEnd())
        return *found;
    xmlSchema *schema = nullptr;
    QFile file(resource);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.readAll();
        if (xmlSchemaParserCtxt *parser = xmlSchemaNewMemParserCtxt(bytes.constData(), int(bytes.size()))) {
            schema = xmlSchemaParse(parser);
            xmlSchemaFreeParserCtxt(parser);
        }
    }
    schemas.insert(resource, schema); // a schema that did not compile stays null: every file then fails
    return schema;
}

XmlElement convert(const xmlNode *node)
{
    XmlElement element;
    element.name = QString::fromUtf8(reinterpret_cast<const char *>(node->name));
    bool leaf = true;
    for (const xmlNode *child = node->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE) {
            leaf = false;
            element.children.append(convert(child));
        }
    }
    if (leaf) {
        if (xmlChar *content = xmlNodeGetContent(node)) {
            element.text = QString::fromUtf8(reinterpret_cast<const char *>(content));
            xmlFree(content);
        }
    }
    return element;
}

XmlRead failed(int line, const QString &message)
{
    XmlRead read;
    read.status = XmlRead::Status::Error;
    read.error = {line, message};
    return read;
}

QHash<QString, QString> &reported()
{
    static QHash<QString, QString> lines; // file path -> the last error line written for it
    return lines;
}

} // namespace

QString collapseWhitespace(const QString &text)
{
    QString out;
    out.reserve(text.size());
    bool space = false;
    for (const QChar c : text) {
        if (c == u' ' || c == u'\t' || c == u'\r' || c == u'\n') {
            space = !out.isEmpty();
            continue;
        }
        if (space)
            out.append(u' ');
        space = false;
        out.append(c);
    }
    return out;
}

XmlRead readValidated(const QString &path, const QString &schemaResource)
{
    static std::once_flag initialised;
    std::call_once(initialised, [] { xmlInitParser(); });

    if (!QFileInfo::exists(path))
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return failed(0, file.errorString());
    if (file.size() > MAX_BYTES)
        return failed(0, QStringLiteral("the file is larger than 1 MiB"));
    const QByteArray bytes = file.read(MAX_BYTES + 1);
    if (file.error() != QFileDevice::NoError)
        return failed(0, file.errorString());
    if (bytes.size() > MAX_BYTES)
        return failed(0, QStringLiteral("the file is larger than 1 MiB"));

    FirstError first;
    const std::unique_ptr<xmlParserCtxt, CtxtFree> ctxt(xmlNewParserCtxt());
    if (!ctxt)
        return failed(0, QStringLiteral("out of memory"));
    xmlCtxtSetErrorHandler(ctxt.get(), collect, &first);
    xmlCtxtSetResourceLoader(ctxt.get(), refuseLoad, nullptr);
    // Not XML_PARSE_NOENT (no entity substitution), XML_PARSE_DTDLOAD, XML_PARSE_DTDATTR or XML_PARSE_XINCLUDE.
    const int options = XML_PARSE_NONET | XML_PARSE_NO_XXE | XML_PARSE_NO_SYS_CATALOG | XML_PARSE_BIG_LINES;
    const QByteArray url = QFile::encodeName(path);
    const std::unique_ptr<xmlDoc, DocFree> doc(
        xmlCtxtReadMemory(ctxt.get(), bytes.constData(), int(bytes.size()), url.constData(), nullptr, options));
    if (first.set)
        return failed(first.error.line, first.error.message);
    if (!doc)
        return failed(0, QStringLiteral("the file could not be parsed"));
    if (doc->intSubset || doc->extSubset) {
        // libxml2 keeps no line for a DTD node: count the lines before the declaration.
        const qsizetype at = bytes.indexOf("<!DOCTYPE");
        const int line = at < 0 ? 1 : int(bytes.first(at).count('\n')) + 1;
        return failed(line, QStringLiteral("a DOCTYPE is not allowed"));
    }

    xmlSchema *schema = schemaFor(schemaResource);
    if (!schema)
        return failed(0, QStringLiteral("the built-in schema %1 did not load").arg(schemaResource));
    const std::unique_ptr<xmlSchemaValidCtxt, ValidFree> valid(xmlSchemaNewValidCtxt(schema));
    if (!valid)
        return failed(0, QStringLiteral("out of memory"));
    xmlSchemaSetValidStructuredErrors(valid.get(), collect, &first);
    const int result = xmlSchemaValidateDoc(valid.get(), doc.get());
    if (first.set)
        return failed(first.error.line, first.error.message);
    if (result != 0)
        return failed(0, QStringLiteral("the file does not match its schema"));

    XmlRead read;
    read.status = XmlRead::Status::Ok;
    read.root = convert(xmlDocGetRootElement(doc.get()));
    return read;
}

void reportFileError(const char *what, const QString &path, const XmlError &error)
{
    QString line = QStringLiteral("%1: %2 error: %3:%4: %5")
                       .arg(QLatin1StringView(RILEY_DOCK_PROGRAM), QLatin1StringView(what), path)
                       .arg(error.line)
                       .arg(error.message);
    for (QChar &c : line) {
        if (c.category() == QChar::Other_Control || c == QChar::LineSeparator || c == QChar::ParagraphSeparator)
            c = u' ';
    }
    line.replace(QStringLiteral("WARNING"), QStringLiteral("warning"));
    line.replace(QStringLiteral("CRITICAL"), QStringLiteral("critical"));
    line.replace(QStringLiteral("panicked"), QStringLiteral("PANICKED"));
    QString &last = reported()[path];
    if (last == line)
        return;
    last = line;
    std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
    std::fflush(stderr);
    log(line);
}

void clearFileError(const QString &path)
{
    reported().remove(path);
}

} // namespace riley::dock::app
