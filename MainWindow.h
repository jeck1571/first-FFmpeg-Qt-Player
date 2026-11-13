#pragma once

#include "AiRecognitionService.h"
#include "FFmpegPlayer.h"
#include "Language.h"
#include "VideoCanvas.h"

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QVector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QProgressBar;
class QPushButton;
class QResizeEvent;
class QSlider;
class QSplitter;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QWidget;
class AiContentDialog;
class SubtitleTrack;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openCommandLineMedia(const QStringList& paths);

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct PlaylistEntry
    {
        QString target;
        QString title;
        bool isUrl = false;
    };

    struct UiText
    {
        QPointer<QObject> object;
        int field = 0;
        QString english;
    };

    enum class OpenMode {
        Replace,
        Append
    };

    enum class PlaylistSort {
        Title,
        Path
    };

    void buildInterface();
    void buildMenus();
    void connectPlayer();
    void connectAiRecognition();
    void configureStyle();
    void applyTheme();
    void loadSettings();
    void saveSettings();

    void recordUiTexts();
    void retranslateUi();
    void switchLanguage(UiLanguage language);
    void setCanvasPlaceholder(const QString& english);

    void openFiles();
    void addFiles();
    void openFolder();
    void openUrl();
    void savePlaylist();
    void openPlaylistFile();
    void openSubtitle();
    void takeSnapshot();
    void toggleFullscreen();
    void toggleTopmost();
    void rotateVideo(int deltaDegrees);
    void resetVideoRotation();
    void setMiniMode(bool enabled);
    void showSleepTimerDialog();
    void setSleepTimer(int minutes);
    void cancelSleepTimer();
    void sleepTimerTick();
    void showAiContentDialog();
    void startAiRecognition();
    void stopAiRecognition();
    void captureAiVideoFrame();
    void showMediaInfo();
    void exportDiagnostics();
    void showAbout();
    void showShortcuts();
    void revealCurrentFile();
    void showVideoContextMenu(const QPoint& globalPosition);

    void setRepeatPointA();
    void setRepeatPointB();
    void clearRepeat();
    bool repeatActive() const;
    void goToTime();
    void copyCurrentSubtitle();
    void showSubtitleStyleDialog();
    void applySubtitleStyle();
    void setDarkTheme(bool dark);

    void filterPlaylist(const QString& text);
    void focusPlaylistSearch();
    void sortPlaylist(PlaylistSort mode);
    void removeDuplicateEntries();
    void rebuildPlaylistView();
    void rememberCurrentPosition();

    void rebuildTrackMenus();
    void rebuildHlsMenu();
    void rebuildChapterMenu();
    void rebuildRecentMenu();
    void addRecentTarget(const QString& target);
    void setFitMode(VideoCanvas::FitMode mode);
    void seekToSliderPosition(int x);
    void adjustSubtitleDelay(int deltaMs);
    void adjustSubtitleScale(double delta);
    void adjustSubtitleMargin(double delta);
    void setEmbeddedSubtitlesVisible(bool visible);
    int currentChapterIndex() const;
    void playNextChapter();
    void playPreviousChapter();

    void enterFullscreenChrome();
    void exitFullscreenChrome();
    void updateFullscreenChromeGeometry();
    void updateCompactChrome();
    void showFullscreenChrome();
    void hideFullscreenChrome();
    void restartFullscreenChromeTimer();

    void addPaths(const QStringList& paths, OpenMode mode);
    void expandPath(const QString& path, QVector<PlaylistEntry>* entries) const;
    bool loadPlaylist(const QString& path, QVector<PlaylistEntry>* entries, QString* error) const;
    void appendPlaylistEntry(const PlaylistEntry& entry, bool playWhenAdded);
    void syncPlaylistOrder();
    void playIndex(int index, qint64 startMs = 0, bool startPaused = false);
    void playNext();
    void playPrevious();
    void removeSelected();
    void clearPlaylist();
    void updatePlaylistButtons();
    int nextPlaylistIndex(bool forward) const;

    void togglePlayPause();
    void stepOneFrame();
    void stepPreviousFrame();
    void stopPlayback();
    void cycleRate();
    void adjustVolume(int delta);
    void toggleMute();
    void updateUi();
    void updateDuration(qint64 durationMs);
    void updateState(FFmpegPlayer::State state);
    void updateSubtitle(qint64 positionMs);
    void updateStatus();
    void updateWindowTitle();
    void showError(const QString& title, const QString& detail);
    void showCanvasOsd(const QString& text);

    void loadSidecarSubtitle(const QString& mediaTarget);
    void setSubtitle(const QString& path);
    QString currentMediaTitle() const;
    static QString makeTitle(const QString& target);
    static QString formatTime(qint64 milliseconds);
    static bool isUrl(const QString& target);
    static bool isPlaylistFile(const QString& path);
    static bool isSupportedMedia(const QString& path);
    static QStringList mediaNameFilters();

    FFmpegPlayer* player_ = nullptr;
    VideoCanvas* videoCanvas_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QVBoxLayout* centralLayout_ = nullptr;
    QWidget* playlistPanel_ = nullptr;
    QWidget* controlBar_ = nullptr;
    QListWidget* playlistView_ = nullptr;
    QLineEdit* playlistFilter_ = nullptr;
    QLabel* playlistCountLabel_ = nullptr;
    QSlider* seekSlider_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* mediaLabel_ = nullptr;
    QComboBox* speedCombo_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QLabel* volumeLabel_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* muteButton_ = nullptr;
    QPushButton* loopButton_ = nullptr;
    QPushButton* shuffleButton_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QTimer* updateTimer_ = nullptr;
    QTimer* fullscreenChromeTimer_ = nullptr;
    QTimer* sleepTimer_ = nullptr;
    QTimer* aiVisionTimer_ = nullptr;

    QAction* playlistAction_ = nullptr;
    QAction* fullscreenAction_ = nullptr;
    QAction* topmostAction_ = nullptr;
    QAction* loopAction_ = nullptr;
    QAction* shuffleAction_ = nullptr;
    QAction* snapshotSubtitlesAction_ = nullptr;
    QAction* showSubtitlesAction_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* audioTrackMenu_ = nullptr;
    QMenu* subtitleTrackMenu_ = nullptr;
    QMenu* hlsQualityMenu_ = nullptr;
    QMenu* chapterMenu_ = nullptr;
    QAction* cancelSleepTimerAction_ = nullptr;
    QVector<QAction*> chapterEntryActions_;
    QActionGroup* audioTrackGroup_ = nullptr;
    QActionGroup* subtitleTrackGroup_ = nullptr;
    QActionGroup* hlsQualityGroup_ = nullptr;
    QActionGroup* fitModeGroup_ = nullptr;
    QAction* englishLanguageAction_ = nullptr;
    QAction* chineseLanguageAction_ = nullptr;
    QAction* repeatPointAAction_ = nullptr;
    QAction* repeatPointBAction_ = nullptr;
    QAction* repeatClearAction_ = nullptr;
    QAction* lightThemeAction_ = nullptr;
    QAction* miniModeAction_ = nullptr;
    QAction* aiRecognitionAction_ = nullptr;

    QVector<UiText> uiTexts_;
    QString placeholderEnglish_;
    UiLanguage uiLanguage_ = UiLanguage::English;

    QVector<PlaylistEntry> playlist_;
    int currentIndex_ = -1;
    bool loopPlaylist_ = false;
    bool shufflePlaylist_ = false;
    bool muted_ = false;
    int lastVolume_ = 100;
    int savedVolume_ = 100;
    bool sliderDragging_ = false;
    bool seeking_ = false;
    bool closing_ = false;
    bool fullscreen_ = false;
    bool wasMaximized_ = false;
    bool topmost_ = false;
    bool controlBarOverlay_ = false;
    bool uiReady_ = false;
    bool syncingPlaylistOrder_ = false;
    bool embeddedSubtitlesVisible_ = true;
    bool snapshotWithSubtitles_ = false;
    int subtitleDelayMs_ = 0;
    double subtitleScale_ = 1.0;
    double subtitleMargin_ = 0.055;
    VideoCanvas::SubtitleStyle subtitleStyle_;
    bool darkTheme_ = true;
    bool miniMode_ = false;
    bool miniRestoreTopmost_ = false;
    QByteArray miniRestoreGeometry_;
    qint64 repeatStartMs_ = -1;
    qint64 repeatEndMs_ = -1;
    bool repeatJumping_ = false;
    qint64 sleepRemainingMs_ = 0;
    int metadataRotation_ = 0;
    int manualRotation_ = 0;
    qint64 pendingResumeMs_ = -1;
    QHash<QString, qint64> resumePositions_;
    qint64 durationMs_ = 0;
    qint64 sessionPosition_ = 0;
    int sessionIndex_ = -1;
    QStringList recentFiles_;
    QVector<QWidget*> normalChrome_;
    SubtitleTrack* subtitleTrack_ = nullptr;
    QString subtitleText_;
    QString mediaTitle_;
    QString lastInfo_;
    AiRecognitionService* aiService_ = nullptr;
    AiContentDialog* aiContentDialog_ = nullptr;
    AiRecognitionService::Config aiConfig_;
    QString aiTranscript_;
    QString aiCoreContent_;
    QString aiMediaTarget_;
    qint64 lastAiVisionPositionMs_ = -1;
};
