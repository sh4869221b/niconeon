#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTextStream>

namespace {
bool canReadResource(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly | QIODevice::Text) && !file.readAll().isEmpty();
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QStringList paths{
        QStringLiteral(":/licenses/LICENSE"),
        QStringLiteral(":/licenses/COPYING"),
        QStringLiteral(":/licenses/THIRD_PARTY_NOTICES.txt"),
    };
    for (const QString &path : paths) {
        if (!canReadResource(path)) {
            out << "missing resource: " << path << '\n';
            return 1;
        }
        out << "readable resource: " << path << '\n';
    }

    return 0;
}
