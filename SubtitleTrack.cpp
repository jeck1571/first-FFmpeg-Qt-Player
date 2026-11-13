#include "SubtitleTrack.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>

namespace {

bool decodeText(const QByteArray& bytes, QString* text)
{
    if (!text) {
        return false;
    }

    QByteArray content = bytes;
    if (content.startsWith("\xEF\xBB\xBF")) {
        content.remove(0, 3);
    }

    QStringDecoder utf8(QStringDecoder::Utf8);
    QString decoded = utf8.decode(content);
    if (!utf8.hasError()) {
        *text = decoded;
        return true;
    }

    *text = QString::fromLocal8Bit(content);
    return true;
}

QString cleanSubtitleText(QString text)
{
    text.replace(QRegularExpression(QStringLiteral("<[^>]+>")), QString());
    text.replace(QRegularExpression(QStringLiteral("\\{[^}]*\\}")), QString());
    text.replace(QStringLiteral("\\N"), QStringLiteral("\n"));
    text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    text.replace(QStringLiteral("\\h"), QStringLiteral(" "));
    return text.trimmed();
}

qint64 parseClock(const QString& timeText)
{
    static const QRegularExpression pattern(
        QStringLiteral("(?:([0-9]+):)?([0-9]{1,2}):([0-9]{1,2})(?:[\\.:,]([0-9]{1,3}))?"));
    const QRegularExpressionMatch match = pattern.match(timeText.trimmed());
    if (!match.hasMatch()) {
        return -1;
    }

    const qint64 hours = match.captured(1).isEmpty() ? 0 : match.captured(1).toLongLong();
    const qint64 minutes = match.captured(2).toLongLong();
    const qint64 seconds = match.captured(3).toLongLong();
    QString fraction = match.captured(4);
    if (fraction.isEmpty()) {
        fraction = QStringLiteral("0");
    }
    if (fraction.size() == 1) {
        fraction += QStringLiteral("00");
    } else if (fraction.size() == 2) {
        fraction += QStringLiteral("0");
    }
    return ((hours * 60 + minutes) * 60 + seconds) * 1000 + fraction.left(3).toLongLong();
}

} // namespace

bool SubtitleTrack::load(const QString& path, QString* error)
{
    clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QObject::tr("Cannot read subtitle file: %1").arg(path);
        }
        return false;
    }

    QString content;
    if (!decodeText(file.readAll(), &content)) {
        if (error) {
            *error = QObject::tr("Cannot decode subtitle text.");
        }
        return false;
    }

    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool parsed = suffix == QStringLiteral("ass") || suffix == QStringLiteral("ssa")
        ? parseAss(content)
        : parseSrt(content);
    if (!parsed || cues_.isEmpty()) {
        if (error) {
            *error = QObject::tr("No subtitle cues were found in the selected file.");
        }
        clear();
        return false;
    }

    filePath_ = path;
    normalize();
    return true;
}

void SubtitleTrack::clear()
{
    filePath_.clear();
    cues_.clear();
}

bool SubtitleTrack::isEmpty() const
{
    return cues_.isEmpty();
}

QString SubtitleTrack::textAt(qint64 positionMs) const
{
    if (cues_.isEmpty() || positionMs < 0) {
        return {};
    }

    const auto iterator = std::upper_bound(
        cues_.cbegin(),
        cues_.cend(),
        positionMs,
        [](qint64 value, const SubtitleCue& cue) { return value < cue.startMs; });

    if (iterator == cues_.cbegin()) {
        return {};
    }

    const SubtitleCue& cue = *(iterator - 1);
    return positionMs <= cue.endMs ? cue.text : QString();
}

QString SubtitleTrack::filePath() const
{
    return filePath_;
}

bool SubtitleTrack::parseSrt(const QString& content)
{
    static const QRegularExpression timing(
        QStringLiteral("^\\s*([^\\r\\n]+?)\\s*-->\\s*([^\\r\\n]+?)(?:\\s+.*)?$"),
        QRegularExpression::MultilineOption);

    QString normalized = content;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QChar('\r'), QChar('\n'));
    normalized.remove(QStringLiteral("WEBVTT"));

    const QStringList blocks = normalized.split(QRegularExpression(QStringLiteral("\\n\\s*\\n")));
    for (const QString& block : blocks) {
        const QStringList lines = block.split(QChar('\n'));
        int timingLine = -1;
        QRegularExpressionMatch match;
        for (int index = 0; index < lines.size(); ++index) {
            match = timing.match(lines.at(index));
            if (match.hasMatch()) {
                timingLine = index;
                break;
            }
        }
        if (timingLine < 0) {
            continue;
        }

        const qint64 start = parseClock(match.captured(1));
        const qint64 end = parseClock(match.captured(2));
        if (start < 0 || end <= start) {
            continue;
        }

        QStringList textLines;
        for (int index = timingLine + 1; index < lines.size(); ++index) {
            textLines.append(lines.at(index));
        }
        const QString text = cleanSubtitleText(textLines.join(QChar('\n')));
        if (!text.isEmpty()) {
            cues_.append({start, end, text});
        }
    }
    return !cues_.isEmpty();
}

bool SubtitleTrack::parseAss(const QString& content)
{
    bool inEvents = false;
    const QStringList lines = content.split(QRegularExpression(QStringLiteral("[\\r\\n]+")));
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QChar('['))) {
            inEvents = line.compare(QStringLiteral("[Events]"), Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!inEvents || !line.startsWith(QStringLiteral("Dialogue:"), Qt::CaseInsensitive)) {
            continue;
        }

        const QString payload = line.mid(9).trimmed();
        QStringList parts = payload.split(QChar(','));
        if (parts.size() < 10) {
            continue;
        }
        const qint64 start = parseClock(parts.at(1));
        const qint64 end = parseClock(parts.at(2));
        if (start < 0 || end <= start) {
            continue;
        }

        QString text = parts.mid(9).join(QChar(','));
        text = cleanSubtitleText(text);
        if (!text.isEmpty()) {
            cues_.append({start, end, text});
        }
    }
    return !cues_.isEmpty();
}

void SubtitleTrack::normalize()
{
    std::sort(cues_.begin(), cues_.end(), [](const SubtitleCue& left, const SubtitleCue& right) {
        return left.startMs < right.startMs;
    });
}
