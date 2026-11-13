// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: read one of the dock's XML files (config.xml, style.xml) and validate it against its schema, which is
// compiled into the binary as a Qt resource (docs/files.md section 4, "Reading the XML files"). libxml2 is used here
// only; the core never sees XML.
#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

namespace riley::dock::app {

// One element of a validated file: its local name, its text (for an element without child elements) and its child
// elements in file order.
struct XmlElement {
    QString name;
    QString text;
    QList<XmlElement> children;
};

struct XmlError {
    int line = 0;     // 1-based line in the file; 0 when the error has no line (the file could not be read)
    QString message;  // libxml2's message, or the dock's own for a read error or a DOCTYPE
};

struct XmlRead {
    enum class Status { Missing, Error, Ok };
    Status status = Status::Missing;
    XmlElement root;  // Ok only
    XmlError error;   // Error only
};

// Read `path` (Missing when it does not exist) and parse it from memory with libxml2: no network
// (XML_PARSE_NONET), no external entities or DTD loading (XML_PARSE_NO_XXE, no XML_PARSE_NOENT or XML_PARSE_DTDLOAD,
// a resource loader that refuses every load), and no DOCTYPE at all. Then validate it against the schema in the Qt
// resource `schemaResource`. Any error, including a file over 1 MiB, gives Error with the first error libxml2
// reported. GUI thread only.
XmlRead readValidated(const QString &path, const QString &schemaResource);

// The XML Schema whitespace "collapse" rule (xs:token, xs:boolean, numbers): runs of space, tab, CR and LF become one
// space, and leading and trailing ones are dropped.
QString collapseWhitespace(const QString &text);

// The one error line of docs/files.md section 4, "Errors", written to stderr and to the dock's log:
//   <program>: <what> error: <file>:<line>: <message>
// `what` is "config" or "style". The same line for the same file is written once: it is written again only after
// that file was read without an error (or found missing) in between. The line is one line (control characters become
// spaces) and never holds the words the capture gates look for: WARNING, CRITICAL and panicked are written as
// warning, critical and PANICKED.
void reportFileError(const char *what, const QString &path, const XmlError &error);
// The file was read without an error, or it is missing: its next error is reported again.
void clearFileError(const QString &path);

} // namespace riley::dock::app
