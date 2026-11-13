#pragma once

#include "AiRecognitionService.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class AiContentDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit AiContentDialog(QWidget* parent = nullptr);

    AiRecognitionService::Config config() const;
    void setConfig(const AiRecognitionService::Config& config);
    void setRunning(bool running);
    void setTranscript(const QString& text);
    void setCoreContent(const QString& text);
    void setStatus(const QString& text);
    void showServiceError(const QString& text);

signals:
    void startRequested();
    void stopRequested();

private:
    QLineEdit* baseUrlEdit_ = nullptr;
    QLineEdit* apiKeyEdit_ = nullptr;
    QLineEdit* transcriptionModelEdit_ = nullptr;
    QLineEdit* textModelEdit_ = nullptr;
    QLineEdit* visionModelEdit_ = nullptr;
    QComboBox* transcriptionLanguageCombo_ = nullptr;
    QComboBox* summaryLanguageCombo_ = nullptr;
    QSpinBox* chunkSecondsSpin_ = nullptr;
    QSpinBox* visionIntervalSpin_ = nullptr;
    QCheckBox* audioCheck_ = nullptr;
    QCheckBox* visionCheck_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPlainTextEdit* transcriptView_ = nullptr;
    QPlainTextEdit* coreContentView_ = nullptr;
};
