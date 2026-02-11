#pragma once

#include <QObject>
#include <QString>

class LicenseProvider : public QObject {
    Q_OBJECT

public:
    explicit LicenseProvider(QObject *parent = nullptr);

    Q_INVOKABLE QString readText(const QString &resourcePath, const QString &fallbackText = QString()) const;
};
