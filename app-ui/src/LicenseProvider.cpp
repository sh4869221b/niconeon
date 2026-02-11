#include "LicenseProvider.hpp"

#include <QFile>

LicenseProvider::LicenseProvider(QObject *parent) : QObject(parent) {}

QString LicenseProvider::readText(const QString &resourcePath, const QString &fallbackText) const {
    if (!resourcePath.startsWith(":/")) {
        return fallbackText;
    }

    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return fallbackText;
    }

    const QByteArray bytes = file.readAll();
    return QString::fromUtf8(bytes.constData(), bytes.size());
}
