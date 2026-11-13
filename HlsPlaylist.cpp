#include "HlsPlaylist.h"

#include <QFileInfo>
#include <QList>
#include <QRegularExpression>

#include <algorithm>

namespace {

QStringList splitAttributeList(const QString& text)
{
    QStringList attributes;
    QString current;
    bool quoted = false;
    for (const QChar ch : text) {
        if (ch == QChar('"')) {
            quoted = !quoted;
            current.append(ch);
        } else if (ch == QChar(',') && !quoted) {
            attributes.append(current.trimmed());
            current.clear();
        } else {
            current.append(ch);
        }
    }
    if (!current.trimmed().isEmpty()) {
        attributes.append(current.trimmed());
    }
    return attributes;
}

QString attributeValue(const QStringList& attributes, const QString& name)
{
    const QString prefix = name + QChar('=');
    for (const QString& attribute : attributes) {
        if (!attribute.startsWith(prefix, Qt::CaseInsensitive)) {
            continue;
        }
        QString value = attribute.mid(prefix.size()).trimmed();
        if (value.size() >= 2
            && value.startsWith(QChar('"'))
            && value.endsWith(QChar('"'))) {
            value = value.mid(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

QSize parseResolution(const QString& value)
{
    const QRegularExpressionMatch match = QRegularExpression(
        QStringLiteral("^(\\d+)[xX](\\d+)$")).match(value.trimmed());
    if (!match.hasMatch()) {
        return {};
    }
    return QSize(match.captured(1).toInt(), match.captured(2).toInt());
}

} // namespace

QString HlsVariant::label() const
{
    QStringList parts;
    if (width > 0 && height > 0) {
        parts.append(QStringLiteral("%1x%2").arg(width).arg(height));
    }
    if (bandwidth > 0) {
        parts.append(QStringLiteral("%1 kbps").arg(
            static_cast<qint64>(bandwidth / 1000)));
    }
    if (!codecs.isEmpty()) {
        parts.append(codecs);
    }
    return parts.isEmpty() ? url.fileName() : parts.join(QStringLiteral("  "));
}

qint64 HlsVariant::selectionBandwidth() const
{
    return averageBandwidth > 0 ? averageBandwidth : bandwidth;
}

bool HlsPlaylist::isHlsTarget(const QString& target)
{
    const QUrl url = QUrl::fromUserInput(target);
    if (url.isLocalFile()) {
        return QFileInfo(url.toLocalFile()).suffix().compare(
            QStringLiteral("m3u8"), Qt::CaseInsensitive) == 0;
    }
    return url.path().endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive);
}

bool HlsPlaylist::isMasterPlaylist(const QByteArray& data)
{
    return data.contains("#EXT-X-STREAM-INF");
}

QVector<HlsVariant> HlsPlaylist::parseMaster(const QByteArray& data, const QUrl& baseUrl)
{
    QVector<HlsVariant> variants;
    const QString content = QString::fromUtf8(data);
    QStringList pendingAttributes;

    for (const QString& rawLine : content.split(QRegularExpression(QStringLiteral("[\\r\\n]+")))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:"), Qt::CaseInsensitive)) {
            pendingAttributes = splitAttributeList(
                line.mid(QStringLiteral("#EXT-X-STREAM-INF:").size()));
            continue;
        }
        if (line.startsWith(QChar('#'))) {
            continue;
        }
        if (pendingAttributes.isEmpty()) {
            continue;
        }

        HlsVariant variant;
        variant.url = baseUrl.resolved(QUrl(line));
        variant.bandwidth = attributeValue(
            pendingAttributes, QStringLiteral("BANDWIDTH")).toLongLong();
        variant.averageBandwidth = attributeValue(
            pendingAttributes, QStringLiteral("AVERAGE-BANDWIDTH")).toLongLong();
        const QSize resolution = parseResolution(attributeValue(
            pendingAttributes, QStringLiteral("RESOLUTION")));
        variant.width = resolution.width();
        variant.height = resolution.height();
        variant.codecs = attributeValue(
            pendingAttributes, QStringLiteral("CODECS"));
        pendingAttributes.clear();

        if (variant.url.isValid() && variant.selectionBandwidth() > 0) {
            variants.append(variant);
        }
    }

    std::sort(variants.begin(), variants.end(),
              [](const HlsVariant& left, const HlsVariant& right) {
                  if (left.selectionBandwidth() != right.selectionBandwidth()) {
                      return left.selectionBandwidth() < right.selectionBandwidth();
                  }
                  if (left.height != right.height) {
                      return left.height < right.height;
                  }
                  return left.url.toString() < right.url.toString();
              });
    return variants;
}

void HlsAbrController::reset()
{
    estimatedKbps_ = 0.0;
}

void HlsAbrController::observeThroughput(qint64 bytes, qint64 elapsedMs)
{
    if (bytes <= 0 || elapsedMs <= 0) {
        return;
    }
    const double sampleKbps =
        static_cast<double>(bytes) * 8.0 / static_cast<double>(elapsedMs);
    if (estimatedKbps_ <= 0.0) {
        estimatedKbps_ = sampleKbps;
    } else {
        // Favor recent measurements while damping short network bursts.
        estimatedKbps_ = estimatedKbps_ * 0.72 + sampleKbps * 0.28;
    }
}

qint64 HlsAbrController::estimatedKbps() const
{
    return static_cast<qint64>(estimatedKbps_ + 0.5);
}

int HlsAbrController::chooseVariant(
    const QVector<HlsVariant>& variants,
    qint64 bufferedMs,
    bool rebuffering) const
{
    if (variants.isEmpty()) {
        return -1;
    }
    if (estimatedKbps_ <= 0.0) {
        return 0;
    }

    double safety = 0.9;
    if (rebuffering) {
        safety = 0.35;
    } else if (bufferedMs < 1500) {
        safety = 0.55;
    } else if (bufferedMs < 4000) {
        safety = 0.72;
    }

    const qint64 usableKbps =
        static_cast<qint64>(estimatedKbps_ * safety);
    int selected = 0;
    for (int index = 0; index < variants.size(); ++index) {
        if (variants.at(index).selectionBandwidth() / 1000 <= usableKbps) {
            selected = index;
        } else {
            break;
        }
    }
    return selected;
}
