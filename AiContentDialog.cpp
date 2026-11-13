#include "AiContentDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QVector>

AiContentDialog::AiContentDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("AI Speech and Video Recognition"));
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    resize(980, 680);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto* serviceGroup = new QGroupBox(tr("AI Service"), this);
    auto* serviceGrid = new QGridLayout(serviceGroup);
    serviceGrid->setColumnStretch(1, 1);
    serviceGrid->setColumnStretch(3, 1);

    baseUrlEdit_ = new QLineEdit(serviceGroup);
    baseUrlEdit_->setPlaceholderText(QStringLiteral("https://api.openai.com/v1"));
    apiKeyEdit_ = new QLineEdit(serviceGroup);
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    transcriptionModelEdit_ = new QLineEdit(serviceGroup);
    textModelEdit_ = new QLineEdit(serviceGroup);
    visionModelEdit_ = new QLineEdit(serviceGroup);
    transcriptionLanguageCombo_ = new QComboBox(serviceGroup);
    transcriptionLanguageCombo_->addItem(tr("Auto Detect"), QStringLiteral("auto"));
    transcriptionLanguageCombo_->addItem(tr("Chinese"), QStringLiteral("zh"));
    transcriptionLanguageCombo_->addItem(tr("English"), QStringLiteral("en"));
    summaryLanguageCombo_ = new QComboBox(serviceGroup);
    summaryLanguageCombo_->addItem(tr("Simplified Chinese"), QStringLiteral("zh"));
    summaryLanguageCombo_->addItem(tr("English"), QStringLiteral("en"));

    chunkSecondsSpin_ = new QSpinBox(serviceGroup);
    chunkSecondsSpin_->setRange(5, 30);
    chunkSecondsSpin_->setSuffix(tr(" s"));
    visionIntervalSpin_ = new QSpinBox(serviceGroup);
    visionIntervalSpin_->setRange(5, 120);
    visionIntervalSpin_->setSuffix(tr(" s"));
    audioCheck_ = new QCheckBox(tr("Speech recognition"), serviceGroup);
    visionCheck_ = new QCheckBox(tr("Video frame recognition"), serviceGroup);
    audioCheck_->setChecked(true);
    visionCheck_->setChecked(true);

    serviceGrid->addWidget(new QLabel(tr("API Base URL"), serviceGroup), 0, 0);
    serviceGrid->addWidget(baseUrlEdit_, 0, 1, 1, 3);
    serviceGrid->addWidget(new QLabel(tr("API Key"), serviceGroup), 1, 0);
    serviceGrid->addWidget(apiKeyEdit_, 1, 1, 1, 3);
    serviceGrid->addWidget(new QLabel(tr("Transcription Model"), serviceGroup), 2, 0);
    serviceGrid->addWidget(transcriptionModelEdit_, 2, 1);
    serviceGrid->addWidget(new QLabel(tr("Core Content Model"), serviceGroup), 2, 2);
    serviceGrid->addWidget(textModelEdit_, 2, 3);
    serviceGrid->addWidget(new QLabel(tr("Audio Language"), serviceGroup), 3, 0);
    serviceGrid->addWidget(transcriptionLanguageCombo_, 3, 1);
    serviceGrid->addWidget(new QLabel(tr("Output Language"), serviceGroup), 3, 2);
    serviceGrid->addWidget(summaryLanguageCombo_, 3, 3);
    serviceGrid->addWidget(new QLabel(tr("Vision Model"), serviceGroup), 4, 0);
    serviceGrid->addWidget(visionModelEdit_, 4, 1);
    serviceGrid->addWidget(new QLabel(tr("Audio Chunk"), serviceGroup), 4, 2);
    serviceGrid->addWidget(chunkSecondsSpin_, 4, 3);
    serviceGrid->addWidget(new QLabel(tr("Frame Interval"), serviceGroup), 5, 0);
    serviceGrid->addWidget(visionIntervalSpin_, 5, 1);
    auto* modes = new QHBoxLayout();
    modes->addWidget(audioCheck_);
    modes->addWidget(visionCheck_);
    modes->addStretch();
    serviceGrid->addLayout(modes, 6, 0, 1, 4);
    auto* privacyNotice = new QLabel(
        tr("Audio segments and sampled video frames are sent to the configured service; "
           "usage may incur API charges. The API key is saved locally in plain text."),
        serviceGroup);
    privacyNotice->setWordWrap(true);
    privacyNotice->setObjectName(QStringLiteral("mutedText"));
    serviceGrid->addWidget(privacyNotice, 7, 0, 1, 4);
    layout->addWidget(serviceGroup);

    auto* results = new QGridLayout();
    results->setColumnStretch(0, 1);
    results->setColumnStretch(1, 1);
    results->addWidget(new QLabel(tr("Live Transcript"), this), 0, 0);
    results->addWidget(new QLabel(tr("Core Content"), this), 0, 1);
    transcriptView_ = new QPlainTextEdit(this);
    transcriptView_->setReadOnly(true);
    transcriptView_->setPlaceholderText(tr("No transcript yet"));
    coreContentView_ = new QPlainTextEdit(this);
    coreContentView_->setReadOnly(true);
    coreContentView_->setPlaceholderText(tr("Core content will appear here"));
    results->addWidget(transcriptView_, 1, 0);
    results->addWidget(coreContentView_, 1, 1);
    layout->addLayout(results, 1);

    auto* footer = new QHBoxLayout();
    statusLabel_ = new QLabel(tr("Not started"), this);
    statusLabel_->setWordWrap(true);
    startButton_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay),
                                   tr("Start Recognition"), this);
    stopButton_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop),
                                  tr("Stop"), this);
    stopButton_->setEnabled(false);
    footer->addWidget(statusLabel_, 1);
    footer->addWidget(startButton_);
    footer->addWidget(stopButton_);
    layout->addLayout(footer);

    connect(startButton_, &QPushButton::clicked, this, &AiContentDialog::startRequested);
    connect(stopButton_, &QPushButton::clicked, this, &AiContentDialog::stopRequested);
}

AiRecognitionService::Config AiContentDialog::config() const
{
    AiRecognitionService::Config value;
    value.baseUrl = baseUrlEdit_->text().trimmed();
    value.apiKey = apiKeyEdit_->text().trimmed();
    value.transcriptionModel = transcriptionModelEdit_->text().trimmed();
    value.textModel = textModelEdit_->text().trimmed();
    value.visionModel = visionModelEdit_->text().trimmed();
    value.transcriptionLanguage = transcriptionLanguageCombo_->currentData().toString();
    value.summaryLanguage = summaryLanguageCombo_->currentData().toString();
    value.audioEnabled = audioCheck_->isChecked();
    value.visionEnabled = visionCheck_->isChecked();
    value.audioChunkSeconds = chunkSecondsSpin_->value();
    value.visionIntervalSeconds = visionIntervalSpin_->value();
    return value;
}

void AiContentDialog::setConfig(const AiRecognitionService::Config& config)
{
    baseUrlEdit_->setText(config.baseUrl);
    apiKeyEdit_->setText(config.apiKey);
    transcriptionModelEdit_->setText(config.transcriptionModel);
    textModelEdit_->setText(config.textModel);
    visionModelEdit_->setText(config.visionModel);

    const int audioLanguage = transcriptionLanguageCombo_->findData(
        config.transcriptionLanguage);
    transcriptionLanguageCombo_->setCurrentIndex(audioLanguage >= 0 ? audioLanguage : 0);
    const int summaryLanguage = summaryLanguageCombo_->findData(config.summaryLanguage);
    summaryLanguageCombo_->setCurrentIndex(summaryLanguage >= 0 ? summaryLanguage : 0);
    audioCheck_->setChecked(config.audioEnabled);
    visionCheck_->setChecked(config.visionEnabled);
    chunkSecondsSpin_->setValue(config.audioChunkSeconds);
    visionIntervalSpin_->setValue(config.visionIntervalSeconds);
}

void AiContentDialog::setRunning(bool running)
{
    const QVector<QWidget*> settings = {
        baseUrlEdit_, apiKeyEdit_, transcriptionModelEdit_, textModelEdit_,
        visionModelEdit_, transcriptionLanguageCombo_, summaryLanguageCombo_,
        chunkSecondsSpin_, visionIntervalSpin_, audioCheck_, visionCheck_
    };
    for (QWidget* widget : settings) {
        widget->setEnabled(!running);
    }
    startButton_->setEnabled(!running);
    stopButton_->setEnabled(running);
}

void AiContentDialog::setTranscript(const QString& text)
{
    if (transcriptView_->toPlainText() == text) {
        return;
    }
    transcriptView_->setPlainText(text);
    transcriptView_->moveCursor(QTextCursor::End);
}

void AiContentDialog::setCoreContent(const QString& text)
{
    if (coreContentView_->toPlainText() == text) {
        return;
    }
    coreContentView_->setPlainText(text);
    coreContentView_->moveCursor(QTextCursor::End);
}

void AiContentDialog::setStatus(const QString& text)
{
    statusLabel_->setText(text);
}

void AiContentDialog::showServiceError(const QString& text)
{
    statusLabel_->setText(tr("Error: %1").arg(text));
}
