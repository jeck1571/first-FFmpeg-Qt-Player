#pragma once

#include <QString>
#include <QVector>

struct SubtitleCue
{
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString text;
};

class SubtitleTrack
{
public:
    bool load(const QString& path, QString* error = nullptr);
    void clear();

    bool isEmpty() const;
    QString textAt(qint64 positionMs) const;
    QString filePath() const;

private:
    bool parseSrt(const QString& content);
    bool parseAss(const QString& content);
    void normalize();

    QString filePath_;
    QVector<SubtitleCue> cues_;
};
