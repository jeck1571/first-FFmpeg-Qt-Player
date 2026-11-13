#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVector>

struct HlsVariant
{
    QUrl url;
    qint64 bandwidth = 0;
    qint64 averageBandwidth = 0;
    int width = 0;
    int height = 0;
    QString codecs;

    QString label() const;
    qint64 selectionBandwidth() const;
};

class HlsPlaylist final
{
public:
    static bool isHlsTarget(const QString& target);
    static bool isMasterPlaylist(const QByteArray& data);
    static QVector<HlsVariant> parseMaster(const QByteArray& data, const QUrl& baseUrl);
};

class HlsAbrController final
{
public:
    void reset();
    void observeThroughput(qint64 bytes, qint64 elapsedMs);

    qint64 estimatedKbps() const;
    int chooseVariant(const QVector<HlsVariant>& variants,
                      qint64 bufferedMs,
                      bool rebuffering = false) const;

private:
    double estimatedKbps_ = 0.0;
};
