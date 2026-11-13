#include "MainWindow.h"

#include "AiContentDialog.h"
#include "CrashReporter.h"
#include "SubtitleTrack.h"
#include "VideoCanvas.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>
#include <limits>

extern "C" {
#include <libavcodec/version.h>
#include <libavformat/version.h>
#include <libavutil/version.h>
}

namespace {

bool decodePlaylistText(const QByteArray& bytes, QString* text)
{
    QByteArray content = bytes;
    if (content.startsWith("\xEF\xBB\xBF")) {
        content.remove(0, 3);
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(content);
    if (!decoder.hasError()) {
        *text = decoded;
        return true;
    }
    *text = QString::fromLocal8Bit(content);
    return true;
}

QString resolvePlaylistTarget(const QString& baseDirectory, const QString& target)
{
    if (target.contains(QStringLiteral("://"))) {
        return target;
    }
    QFileInfo info(target);
    return info.isAbsolute()
        ? info.absoluteFilePath()
        : QDir(baseDirectory).absoluteFilePath(target);
}

bool parseTimeText(const QString& text, qint64* milliseconds)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    bool ok = false;
    if (!trimmed.contains(QChar(':'))) {
        const double seconds = trimmed.toDouble(&ok);
        if (!ok || seconds < 0) {
            return false;
        }
        *milliseconds = static_cast<qint64>(std::llround(seconds * 1000.0));
        return true;
    }
    const QStringList parts = trimmed.split(QChar(':'));
    if (parts.size() > 3) {
        return false;
    }
    double total = 0.0;
    for (const QString& part : parts) {
        const double value = part.trimmed().toDouble(&ok);
        if (!ok || value < 0) {
            return false;
        }
        total = total * 60.0 + value;
    }
    *milliseconds = static_cast<qint64>(std::llround(total * 1000.0));
    return true;
}

void styleColorButton(QPushButton* button, const QColor& color)
{
    const bool lightText = color.lightnessF() < 0.5;
    button->setText(color.name(QColor::HexRgb).toUpper());
    button->setStyleSheet(QStringLiteral("background: %1; color: %2; border: 1px solid #6a7080;"
                                         " border-radius: 4px; padding: 3px 8px;")
                              .arg(color.name(),
                                   lightText ? QStringLiteral("#f5f6f8")
                                             : QStringLiteral("#101216")));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , player_(new FFmpegPlayer(this))
    , subtitleTrack_(new SubtitleTrack())
    , aiService_(new AiRecognitionService(this))
{
    qRegisterMetaType<FFmpegPlayer::State>("FFmpegPlayer::State");
    setWindowTitle(tr("FFmpeg Qt Player"));
    setAcceptDrops(true);
    setMinimumSize(640, 400);
    const QRect available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800);
    const int initialWidth = qMin(1180, qMax(640, available.width() - 40));
    const int initialHeight = qMin(720, qMax(400, available.height() - 80));
    resize(initialWidth, initialHeight);
    move(available.center() - QPoint(initialWidth / 2, initialHeight / 2));

    buildInterface();
    buildMenus();
    configureStyle();
    connectPlayer();
    connectAiRecognition();
    loadSettings();

    recordUiTexts();
    setCanvasPlaceholder(QStringLiteral("Open media to begin"));
    if (uiLanguage_ == UiLanguage::Chinese) {
        UiLanguageSupport::setCurrent(UiLanguage::Chinese);
        retranslateUi();
    }

    player_->setVideoCanvas(videoCanvas_);
    player_->setVolume(savedVolume_);
    player_->setPlaybackRate(speedCombo_->currentData().toDouble());
    {
        const QSignalBlocker blocker(volumeSlider_);
        volumeSlider_->setValue(savedVolume_);
    }
    volumeLabel_->setText(QStringLiteral("%1%").arg(savedVolume_));
    player_->setMuted(muted_);
    muteButton_->setIcon(style()->standardIcon(
        muted_ ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));

    updateTimer_ = new QTimer(this);
    updateTimer_->setInterval(100);
    connect(updateTimer_, &QTimer::timeout, this, &MainWindow::updateUi);
    updateTimer_->start();

    fullscreenChromeTimer_ = new QTimer(this);
    fullscreenChromeTimer_->setSingleShot(true);
    fullscreenChromeTimer_->setInterval(2500);
    connect(fullscreenChromeTimer_, &QTimer::timeout, this, &MainWindow::hideFullscreenChrome);

    sleepTimer_ = new QTimer(this);
    sleepTimer_->setInterval(1000);
    connect(sleepTimer_, &QTimer::timeout, this, &MainWindow::sleepTimerTick);

    aiVisionTimer_ = new QTimer(this);
    connect(aiVisionTimer_, &QTimer::timeout, this, &MainWindow::captureAiVideoFrame);

    updateState(FFmpegPlayer::State::Idle);
    updatePlaylistButtons();
    updateStatus();
    uiReady_ = true;

    if (sessionIndex_ >= 0 && sessionIndex_ < playlist_.size()) {
        const PlaylistEntry& entry = playlist_.at(sessionIndex_);
        if (entry.isUrl || QFileInfo::exists(entry.target)) {
            playIndex(sessionIndex_, sessionPosition_, true);
        } else {
            currentIndex_ = sessionIndex_;
            playlistView_->setCurrentRow(sessionIndex_);
            updateWindowTitle();
            updatePlaylistButtons();
        }
    }
}

MainWindow::~MainWindow()
{
    aiService_->stop();
    player_->setRecognitionAudioEnabled(false);
    player_->setVideoCanvas(nullptr);
    player_->stop();
    delete subtitleTrack_;
    subtitleTrack_ = nullptr;
}

void MainWindow::openCommandLineMedia(const QStringList& paths)
{
    if (!paths.isEmpty()) {
        addPaths(paths, OpenMode::Replace);
    }
}

void MainWindow::buildInterface()
{
    auto* central = new QWidget(this);
    centralLayout_ = new QVBoxLayout(central);
    centralLayout_->setContentsMargins(0, 0, 0, 0);
    centralLayout_->setSpacing(0);

    splitter_ = new QSplitter(Qt::Horizontal, central);
    splitter_->setChildrenCollapsible(false);
    videoCanvas_ = new VideoCanvas(splitter_);
    splitter_->addWidget(videoCanvas_);

    playlistPanel_ = new QWidget(splitter_);
    playlistPanel_->setMinimumWidth(250);
    playlistPanel_->setMaximumWidth(390);
    auto* playlistLayout = new QVBoxLayout(playlistPanel_);
    playlistLayout->setContentsMargins(12, 12, 12, 12);
    playlistLayout->setSpacing(8);

    auto* playlistHeader = new QHBoxLayout();
    auto* playlistTitle = new QLabel(tr("Playlist"), playlistPanel_);
    playlistTitle->setObjectName(QStringLiteral("panelTitle"));
    playlistCountLabel_ = new QLabel(playlistPanel_);
    playlistCountLabel_->setObjectName(QStringLiteral("mutedText"));
    playlistHeader->addWidget(playlistTitle);
    playlistHeader->addStretch();
    playlistHeader->addWidget(playlistCountLabel_);
    playlistLayout->addLayout(playlistHeader);

    playlistFilter_ = new QLineEdit(playlistPanel_);
    playlistFilter_->setPlaceholderText(tr("Search playlist..."));
    playlistFilter_->setClearButtonEnabled(true);
    playlistFilter_->setFocusPolicy(Qt::ClickFocus);
    playlistLayout->addWidget(playlistFilter_);
    connect(playlistFilter_, &QLineEdit::textChanged, this, &MainWindow::filterPlaylist);

    playlistView_ = new QListWidget(playlistPanel_);
    playlistView_->setUniformItemSizes(false);
    playlistView_->setAlternatingRowColors(false);
    playlistView_->setSelectionMode(QAbstractItemView::SingleSelection);
    playlistView_->setContextMenuPolicy(Qt::ActionsContextMenu);
    playlistView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    playlistView_->setDragDropMode(QAbstractItemView::InternalMove);
    playlistView_->setDefaultDropAction(Qt::MoveAction);
    playlistView_->setDragEnabled(true);
    playlistView_->setDropIndicatorShown(true);
    playlistLayout->addWidget(playlistView_, 1);

    auto* playlistButtons = new QHBoxLayout();
    auto* playSelected = new QToolButton(playlistPanel_);
    playSelected->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    playSelected->setToolTip(tr("Play selected item"));
    auto* removeSelectedButton = new QToolButton(playlistPanel_);
    removeSelectedButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    removeSelectedButton->setToolTip(tr("Remove selected item"));
    auto* saveButton = new QToolButton(playlistPanel_);
    saveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    saveButton->setToolTip(tr("Save playlist"));
    playlistButtons->addWidget(playSelected);
    playlistButtons->addWidget(removeSelectedButton);
    playlistButtons->addStretch();
    playlistButtons->addWidget(saveButton);
    playlistLayout->addLayout(playlistButtons);

    connect(playSelected, &QToolButton::clicked, this, [this] {
        if (playlistView_->currentRow() >= 0) {
            playIndex(playlistView_->currentRow());
        }
    });
    connect(removeSelectedButton, &QToolButton::clicked, this, &MainWindow::removeSelected);
    connect(saveButton, &QToolButton::clicked, this, &MainWindow::savePlaylist);
    connect(playlistView_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (item) {
            playIndex(playlistView_->row(item));
        }
    });

    auto* removeAction = new QAction(tr("Remove"), playlistView_);
    auto* clearAction = new QAction(tr("Clear"), playlistView_);
    connect(removeAction, &QAction::triggered, this, &MainWindow::removeSelected);
    connect(clearAction, &QAction::triggered, this, &MainWindow::clearPlaylist);
    playlistView_->addAction(removeAction);
    playlistView_->addAction(clearAction);
    splitter_->addWidget(playlistPanel_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 0);
    splitter_->setSizes({850, 300});
    centralLayout_->addWidget(splitter_, 1);

    controlBar_ = new QWidget(central);
    controlBar_->setObjectName(QStringLiteral("controlBar"));
    auto* controlsLayout = new QVBoxLayout(controlBar_);
    controlsLayout->setContentsMargins(14, 8, 14, 10);
    controlsLayout->setSpacing(6);

    auto* seekLayout = new QHBoxLayout();
    seekSlider_ = new QSlider(Qt::Horizontal, controlBar_);
    seekSlider_->setRange(0, 0);
    seekSlider_->setSingleStep(1000);
    seekSlider_->setPageStep(5000);
    seekSlider_->setFocusPolicy(Qt::NoFocus);
    seekSlider_->installEventFilter(this);
    timeLabel_ = new QLabel(QStringLiteral("00:00 / 00:00"), controlBar_);
    timeLabel_->setObjectName(QStringLiteral("timeText"));
    timeLabel_->setMinimumWidth(126);
    timeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    seekLayout->addWidget(seekSlider_, 1);
    seekLayout->addWidget(timeLabel_);
    controlsLayout->addLayout(seekLayout);

    auto* transportLayout = new QHBoxLayout();
    transportLayout->setSpacing(6);
    auto* previousButton = new QToolButton(controlBar_);
    previousButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    previousButton->setToolTip(tr("Previous (P)"));
    playButton_ = new QToolButton(controlBar_);
    playButton_->setObjectName(QStringLiteral("primaryButton"));
    playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    playButton_->setToolTip(tr("Play / pause (Space)"));
    auto* backButton = new QToolButton(controlBar_);
    backButton->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
    backButton->setToolTip(tr("Back 5 seconds (Left)"));
    auto* forwardButton = new QToolButton(controlBar_);
    forwardButton->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
    forwardButton->setToolTip(tr("Forward 5 seconds (Right)"));
    auto* nextButton = new QToolButton(controlBar_);
    nextButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    nextButton->setToolTip(tr("Next (N)"));
    auto* stopButton = new QToolButton(controlBar_);
    stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    stopButton->setToolTip(tr("Stop (S)"));
    transportLayout->addWidget(previousButton);
    transportLayout->addWidget(playButton_);
    transportLayout->addWidget(backButton);
    transportLayout->addWidget(forwardButton);
    transportLayout->addWidget(nextButton);
    transportLayout->addWidget(stopButton);

    speedCombo_ = new QComboBox(controlBar_);
    const QList<double> rates = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
    for (double rate : rates) {
        speedCombo_->addItem(QStringLiteral("%1x").arg(rate, 0, 'g', 3), rate);
    }
    speedCombo_->setToolTip(tr("Playback speed"));
    speedCombo_->setMinimumWidth(78);
    transportLayout->addSpacing(6);
    transportLayout->addWidget(speedCombo_);

    loopButton_ = new QPushButton(tr("Loop"), controlBar_);
    loopButton_->setCheckable(true);
    loopButton_->setToolTip(tr("Loop playlist"));
    shuffleButton_ = new QPushButton(tr("Shuffle"), controlBar_);
    shuffleButton_->setCheckable(true);
    shuffleButton_->setToolTip(tr("Shuffle playlist"));
    transportLayout->addWidget(loopButton_);
    transportLayout->addWidget(shuffleButton_);
    transportLayout->addStretch();

    muteButton_ = new QToolButton(controlBar_);
    muteButton_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    muteButton_->setToolTip(tr("Mute (M)"));
    volumeSlider_ = new QSlider(Qt::Horizontal, controlBar_);
    volumeSlider_->setRange(0, 125);
    volumeSlider_->setValue(100);
    volumeSlider_->setFixedWidth(112);
    volumeSlider_->setToolTip(tr("Volume: 0-125%"));
    volumeLabel_ = new QLabel(QStringLiteral("100%"), controlBar_);
    volumeLabel_->setObjectName(QStringLiteral("mutedText"));
    volumeLabel_->setFixedWidth(40);
    transportLayout->addWidget(muteButton_);
    transportLayout->addWidget(volumeSlider_);
    transportLayout->addWidget(volumeLabel_);

    mediaLabel_ = new QLabel(tr("No media loaded"), controlBar_);
    mediaLabel_->setObjectName(QStringLiteral("mutedText"));
    mediaLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mediaLabel_->setMinimumWidth(0);
    transportLayout->addSpacing(12);
    transportLayout->addWidget(mediaLabel_);
    controlsLayout->addLayout(transportLayout);

    centralLayout_->addWidget(controlBar_);
    setCentralWidget(central);

    statusLabel_ = new QLabel(tr("Ready"), this);
    statusBar()->addWidget(statusLabel_, 1);
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName(QStringLiteral("statusProgress"));
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    progressBar_->setFixedWidth(150);
    progressBar_->setFixedHeight(10);
    statusBar()->addPermanentWidget(progressBar_);
    normalChrome_ = {menuBar(), controlBar_, playlistPanel_, statusBar()};

    videoCanvas_->installEventFilter(this);
    controlBar_->installEventFilter(this);

    // Keyboard focus must not stay on controls: otherwise arrow keys and Space
    // get consumed by sliders, combos and checkable buttons instead of the
    // player shortcuts.
    for (QWidget* widget : {static_cast<QWidget*>(previousButton),
                            static_cast<QWidget*>(playButton_),
                            static_cast<QWidget*>(backButton),
                            static_cast<QWidget*>(forwardButton),
                            static_cast<QWidget*>(nextButton),
                            static_cast<QWidget*>(stopButton),
                            static_cast<QWidget*>(speedCombo_),
                            static_cast<QWidget*>(loopButton_),
                            static_cast<QWidget*>(shuffleButton_),
                            static_cast<QWidget*>(muteButton_),
                            static_cast<QWidget*>(volumeSlider_)}) {
        widget->setFocusPolicy(Qt::NoFocus);
    }

    connect(playlistView_->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int, int, const QModelIndex&, int) {
                syncPlaylistOrder();
            });

    connect(playlistView_, &QListWidget::currentRowChanged, this, [this](int) {
        updatePlaylistButtons();
    });
    connect(seekSlider_, &QSlider::sliderPressed, this, [this] {
        sliderDragging_ = true;
    });
    connect(seekSlider_, &QSlider::sliderReleased, this, [this] {
        sliderDragging_ = false;
        seeking_ = true;
        player_->setPosition(seekSlider_->value());
        QTimer::singleShot(150, this, [this] { seeking_ = false; });
    });
    connect(seekSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (!sliderDragging_) {
            return;
        }
        timeLabel_->setText(
            QStringLiteral("%1 / %2").arg(formatTime(value), formatTime(durationMs_)));
    });
    connect(playButton_, &QToolButton::clicked, this, &MainWindow::togglePlayPause);
    connect(backButton, &QToolButton::clicked, this, [this] { player_->seekRelative(-5000); });
    connect(forwardButton, &QToolButton::clicked, this, [this] { player_->seekRelative(5000); });
    connect(previousButton, &QToolButton::clicked, this, &MainWindow::playPrevious);
    connect(nextButton, &QToolButton::clicked, this, &MainWindow::playNext);
    connect(stopButton, &QToolButton::clicked, this, &MainWindow::stopPlayback);
    connect(speedCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!player_) {
            return;
        }
        const double rate = speedCombo_->currentData().toDouble();
        player_->setPlaybackRate(rate);
        statusLabel_->setText(tr("Playback speed: %1x").arg(rate, 0, 'g', 3));
        if (uiReady_) {
            videoCanvas_->showOsd(tr("Speed %1x").arg(rate, 0, 'g', 3));
        }
    });
    connect(loopButton_, &QPushButton::toggled, this, [this](bool checked) {
        loopPlaylist_ = checked;
        if (loopAction_) {
            loopAction_->setChecked(checked);
        }
        updateStatus();
    });
    connect(shuffleButton_, &QPushButton::toggled, this, [this](bool checked) {
        shufflePlaylist_ = checked;
        if (shuffleAction_) {
            shuffleAction_->setChecked(checked);
        }
        updateStatus();
    });
    connect(muteButton_, &QToolButton::clicked, this, &MainWindow::toggleMute);
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        savedVolume_ = value;
        player_->setVolume(value);
        if (value > 0 && muted_) {
            muted_ = false;
            player_->setMuted(false);
            muteButton_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
        }
        volumeLabel_->setText(QStringLiteral("%1%").arg(value));
        if (value > 0) {
            lastVolume_ = value;
        }
        if (uiReady_) {
            videoCanvas_->showOsd(tr("Volume %1%").arg(value));
        }
    });
    connect(videoCanvas_, &VideoCanvas::doubleClicked, this, &MainWindow::toggleFullscreen);
    connect(videoCanvas_, &VideoCanvas::volumeWheelSteps, this, [this](int steps) {
        if (steps != 0) {
            adjustVolume(steps * 5);
        }
    });
    connect(videoCanvas_, &VideoCanvas::contextMenuRequested,
            this, &MainWindow::showVideoContextMenu);
    connect(playlistView_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item) {
            playIndex(playlistView_->row(item));
        }
    });
    connect(videoCanvas_, &VideoCanvas::mediaDropped, this, [this](const QStringList& paths) {
        addPaths(paths, playlist_.isEmpty() ? OpenMode::Replace : OpenMode::Append);
    });
}

void MainWindow::buildMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("File"));
    QAction* openFilesAction = fileMenu->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton),
                                                   tr("Open Files..."));
    openFilesAction->setShortcut(QKeySequence::Open);
    QAction* addFilesAction = fileMenu->addAction(tr("Add Files..."));
    addFilesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    QAction* openFolderAction = fileMenu->addAction(tr("Open Folder..."));
    QAction* openUrlAction = fileMenu->addAction(tr("Open Network Stream..."));
    openUrlAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    fileMenu->addSeparator();
    recentMenu_ = fileMenu->addMenu(tr("Recent Files"));
    connect(recentMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    fileMenu->addSeparator();
    QAction* openPlaylistAction = fileMenu->addAction(tr("Open Playlist..."));
    QAction* savePlaylistAction = fileMenu->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton),
                                                      tr("Save Playlist..."));
    savePlaylistAction->setShortcut(QKeySequence::Save);
    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(tr("Exit"));
    exitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));

    QMenu* playbackMenu = menuBar()->addMenu(tr("Playback"));
    QAction* playPauseAction = playbackMenu->addAction(tr("Play / Pause"));
    playPauseAction->setShortcut(QKeySequence(Qt::Key_Space));
    QAction* nextFrameAction = playbackMenu->addAction(tr("Next Video Frame"));
    nextFrameAction->setShortcut(QKeySequence(Qt::Key_E));
    QAction* previousFrameAction = playbackMenu->addAction(tr("Previous Video Frame"));
    previousFrameAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    QAction* goToTimeAction = playbackMenu->addAction(tr("Go to Time..."));
    goToTimeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    chapterMenu_ = playbackMenu->addMenu(tr("Chapters"));
    QAction* previousChapterAction = chapterMenu_->addAction(tr("Previous Chapter"));
    previousChapterAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_PageUp));
    QAction* nextChapterAction = chapterMenu_->addAction(tr("Next Chapter"));
    nextChapterAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_PageDown));
    chapterMenu_->addSeparator();
    QAction* stopAction = playbackMenu->addAction(tr("Stop"));
    stopAction->setShortcut(QKeySequence(Qt::Key_S));
    QAction* previousAction = playbackMenu->addAction(tr("Previous"));
    previousAction->setShortcut(QKeySequence(Qt::Key_P));
    QAction* nextAction = playbackMenu->addAction(tr("Next"));
    nextAction->setShortcut(QKeySequence(Qt::Key_N));
    playbackMenu->addSeparator();
    QAction* backAction = playbackMenu->addAction(tr("Back 5 seconds"));
    backAction->setShortcut(QKeySequence(Qt::Key_Left));
    QAction* forwardAction = playbackMenu->addAction(tr("Forward 5 seconds"));
    forwardAction->setShortcut(QKeySequence(Qt::Key_Right));
    playbackMenu->addSeparator();

    QMenu* speedMenu = playbackMenu->addMenu(tr("Playback Speed"));
    QAction* slowerAction = speedMenu->addAction(tr("Slower"));
    slowerAction->setShortcut(QKeySequence(Qt::Key_BracketLeft));
    QAction* fasterAction = speedMenu->addAction(tr("Faster"));
    fasterAction->setShortcut(QKeySequence(Qt::Key_BracketRight));
    QAction* normalSpeedAction = speedMenu->addAction(tr("Normal Speed"));

    playbackMenu->addSeparator();
    loopAction_ = playbackMenu->addAction(tr("Loop Playlist"));
    loopAction_->setCheckable(true);
    shuffleAction_ = playbackMenu->addAction(tr("Shuffle Playlist"));
    shuffleAction_->setCheckable(true);

    playbackMenu->addSeparator();
    QMenu* repeatMenu = playbackMenu->addMenu(tr("A-B Repeat"));
    repeatPointAAction_ = repeatMenu->addAction(tr("Set A Point"));
    repeatPointAAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    repeatPointBAction_ = repeatMenu->addAction(tr("Set B Point"));
    repeatPointBAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    repeatClearAction_ = repeatMenu->addAction(tr("Clear A-B Repeat"));
    repeatClearAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X));

    QMenu* playlistMenu = menuBar()->addMenu(tr("Playlist"));
    QAction* playSelectedAction = playlistMenu->addAction(tr("Play Selected"));
    QAction* removeSelectedAction = playlistMenu->addAction(tr("Remove Selected"));
    removeSelectedAction->setShortcut(QKeySequence(Qt::Key_Delete));
    removeSelectedAction->setShortcutContext(Qt::WindowShortcut);
    addAction(removeSelectedAction);
    QAction* clearPlaylistAction = playlistMenu->addAction(tr("Clear Playlist"));
    playlistMenu->addSeparator();
    QAction* searchPlaylistAction = playlistMenu->addAction(tr("Search Playlist"));
    searchPlaylistAction->setShortcut(QKeySequence::Find);
    QMenu* sortMenu = playlistMenu->addMenu(tr("Sort Playlist"));
    QAction* sortTitleAction = sortMenu->addAction(tr("Sort by Title"));
    QAction* sortPathAction = sortMenu->addAction(tr("Sort by Path"));
    QAction* removeDuplicatesAction = playlistMenu->addAction(tr("Remove Duplicates"));

    QMenu* audioMenu = menuBar()->addMenu(tr("Audio"));
    QAction* muteAction = audioMenu->addAction(tr("Mute"));
    muteAction->setShortcut(QKeySequence(Qt::Key_M));
    QAction* volumeUpAction = audioMenu->addAction(tr("Volume Up"));
    volumeUpAction->setShortcut(QKeySequence(Qt::Key_Up));
    QAction* volumeDownAction = audioMenu->addAction(tr("Volume Down"));
    volumeDownAction->setShortcut(QKeySequence(Qt::Key_Down));
    audioMenu->addSeparator();
    QAction* volume50Action = audioMenu->addAction(tr("Volume 50%"));
    QAction* volume100Action = audioMenu->addAction(tr("Volume 100%"));
    QAction* volume125Action = audioMenu->addAction(tr("Volume 125%"));
    audioMenu->addSeparator();
    audioTrackMenu_ = audioMenu->addMenu(tr("Audio Track"));

    QMenu* videoMenu = menuBar()->addMenu(tr("Video"));
    fullscreenAction_ = videoMenu->addAction(tr("Fullscreen"));
    fullscreenAction_->setCheckable(true);
    fullscreenAction_->setShortcuts({QKeySequence(Qt::Key_F11), QKeySequence(Qt::Key_F)});
    topmostAction_ = videoMenu->addAction(tr("Always on Top"));
    topmostAction_->setCheckable(true);
    QAction* snapshotAction = videoMenu->addAction(tr("Take Snapshot"));
    snapshotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    videoMenu->addSeparator();
    QAction* rotateClockwiseAction = videoMenu->addAction(tr("Rotate Clockwise"));
    rotateClockwiseAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    QAction* rotateCounterclockwiseAction = videoMenu->addAction(tr("Rotate Counterclockwise"));
    rotateCounterclockwiseAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    QAction* resetRotationAction = videoMenu->addAction(tr("Reset Rotation"));
    videoMenu->addSeparator();
    QMenu* aspectMenu = videoMenu->addMenu(tr("Aspect Ratio"));
    fitModeGroup_ = new QActionGroup(this);
    fitModeGroup_->setExclusive(true);
    auto addFitMode = [this, aspectMenu](const QString& label, VideoCanvas::FitMode mode) {
        QAction* action = aspectMenu->addAction(label);
        action->setCheckable(true);
        action->setData(static_cast<int>(mode));
        fitModeGroup_->addAction(action);
        if (mode == videoCanvas_->fitMode()) {
            action->setChecked(true);
        }
        connect(action, &QAction::triggered, this, [this, mode] { setFitMode(mode); });
    };
    addFitMode(tr("Fit to Window"), VideoCanvas::FitMode::Fit);
    addFitMode(tr("Fill and Crop"), VideoCanvas::FitMode::Fill);
    addFitMode(tr("Stretch"), VideoCanvas::FitMode::Stretch);
    addFitMode(tr("16:9"), VideoCanvas::FitMode::Ratio16x9);
    addFitMode(tr("4:3"), VideoCanvas::FitMode::Ratio4x3);
    addFitMode(tr("Original Size"), VideoCanvas::FitMode::Original);
    videoMenu->addSeparator();
    hlsQualityMenu_ = videoMenu->addMenu(tr("HLS Quality"));
    rebuildHlsMenu();

    QMenu* subtitleMenu = menuBar()->addMenu(tr("Subtitle"));
    QAction* subtitleAction = subtitleMenu->addAction(tr("Load Subtitle File..."));
    QAction* subtitleStyleAction = subtitleMenu->addAction(tr("Subtitle Style..."));
    subtitleStyleAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    QAction* copySubtitleAction = subtitleMenu->addAction(tr("Copy Current Subtitle"));
    copySubtitleAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    subtitleAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    showSubtitlesAction_ = subtitleMenu->addAction(tr("Show Subtitles"));
    showSubtitlesAction_->setCheckable(true);
    showSubtitlesAction_->setChecked(true);
    showSubtitlesAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    subtitleMenu->addSeparator();
    subtitleTrackMenu_ = subtitleMenu->addMenu(tr("Embedded Subtitle Track"));
    subtitleMenu->addSeparator();

    QMenu* delayMenu = subtitleMenu->addMenu(tr("Subtitle Delay"));
    QAction* delayBackAction = delayMenu->addAction(tr("Delay -100 ms"));
    delayBackAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Left));
    QAction* delayResetAction = delayMenu->addAction(tr("Reset Delay"));
    QAction* delayForwardAction = delayMenu->addAction(tr("Delay +100 ms"));
    delayForwardAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right));

    QMenu* sizeMenu = subtitleMenu->addMenu(tr("Subtitle Size"));
    QAction* sizeSmallerAction = sizeMenu->addAction(tr("Smaller"));
    QAction* sizeResetAction = sizeMenu->addAction(tr("Default Size"));
    QAction* sizeBiggerAction = sizeMenu->addAction(tr("Larger"));

    QMenu* positionMenu = subtitleMenu->addMenu(tr("Subtitle Position"));
    QAction* positionLowerAction = positionMenu->addAction(tr("Lower"));
    QAction* positionResetAction = positionMenu->addAction(tr("Default Position"));
    QAction* positionHigherAction = positionMenu->addAction(tr("Higher"));

    subtitleMenu->addSeparator();
    snapshotSubtitlesAction_ = subtitleMenu->addAction(tr("Include Subtitles in Snapshot"));
    snapshotSubtitlesAction_->setCheckable(true);

    QMenu* toolsMenu = menuBar()->addMenu(tr("Tools"));
    QAction* infoAction = toolsMenu->addAction(tr("Media Information"));
    QAction* exportDiagnosticsAction = toolsMenu->addAction(tr("Export Diagnostics..."));
    QAction* crashFolderAction = toolsMenu->addAction(tr("Open Crash Reports Folder"));
    aiRecognitionAction_ = toolsMenu->addAction(tr("AI Speech and Video Recognition..."));
    aiRecognitionAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    QMenu* sleepMenu = toolsMenu->addMenu(tr("Sleep Timer"));
    for (int minutes : {15, 30, 45, 60, 90}) {
        QAction* action = sleepMenu->addAction(tr("%1 minutes").arg(minutes));
        connect(action, &QAction::triggered, this, [this, minutes] { setSleepTimer(minutes); });
    }
    QAction* customSleepAction = sleepMenu->addAction(tr("Custom Timer..."));
    sleepMenu->addSeparator();
    cancelSleepTimerAction_ = sleepMenu->addAction(tr("Cancel Sleep Timer"));
    cancelSleepTimerAction_->setEnabled(false);
    QAction* revealAction = toolsMenu->addAction(tr("Reveal Current File"));
    toolsMenu->addSeparator();
    playlistAction_ = toolsMenu->addAction(tr("Playlist"));
    playlistAction_->setCheckable(true);
    playlistAction_->setChecked(true);
    playlistAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
    lightThemeAction_ = toolsMenu->addAction(tr("Light Theme"));
    lightThemeAction_->setCheckable(true);
    miniModeAction_ = toolsMenu->addAction(tr("Mini Player"));
    miniModeAction_->setCheckable(true);
    miniModeAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));

    QMenu* languageMenu = menuBar()->addMenu(tr("Language"));
    auto* languageGroup = new QActionGroup(this);
    languageGroup->setExclusive(true);
    englishLanguageAction_ = languageMenu->addAction(QStringLiteral("English"));
    englishLanguageAction_->setCheckable(true);
    englishLanguageAction_->setChecked(true);
    chineseLanguageAction_ = languageMenu->addAction(QStringLiteral("简体中文"));
    chineseLanguageAction_->setCheckable(true);
    for (QAction* action : {englishLanguageAction_, chineseLanguageAction_}) {
        // Language names stay in their own script regardless of UI language.
        action->setProperty("noTranslate", true);
        languageGroup->addAction(action);
    }
    connect(englishLanguageAction_, &QAction::triggered, this,
            [this] { switchLanguage(UiLanguage::English); });
    connect(chineseLanguageAction_, &QAction::triggered, this,
            [this] { switchLanguage(UiLanguage::Chinese); });

    QMenu* helpMenu = menuBar()->addMenu(tr("Help"));
    QAction* shortcutsAction = helpMenu->addAction(tr("Keyboard Shortcuts"));
    shortcutsAction->setShortcut(QKeySequence::HelpContents);
    QAction* aboutAction = helpMenu->addAction(tr("About"));

    QAction* escapeFullscreen = new QAction(this);
    escapeFullscreen->setShortcut(QKeySequence(Qt::Key_Escape));
    addAction(escapeFullscreen);

    connect(openFilesAction, &QAction::triggered, this, &MainWindow::openFiles);
    connect(addFilesAction, &QAction::triggered, this, &MainWindow::addFiles);
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::openFolder);
    connect(openUrlAction, &QAction::triggered, this, &MainWindow::openUrl);
    connect(openPlaylistAction, &QAction::triggered, this, &MainWindow::openPlaylistFile);
    connect(savePlaylistAction, &QAction::triggered, this, &MainWindow::savePlaylist);
    connect(exportDiagnosticsAction, &QAction::triggered, this, &MainWindow::exportDiagnostics);
    connect(crashFolderAction, &QAction::triggered, this, [] {
        const QString directory = CrashReporter::crashDirectory();
        if (!directory.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
        }
    });
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    connect(playPauseAction, &QAction::triggered, this, &MainWindow::togglePlayPause);
    connect(nextFrameAction, &QAction::triggered, this, &MainWindow::stepOneFrame);
    connect(previousFrameAction, &QAction::triggered, this, &MainWindow::stepPreviousFrame);
    connect(goToTimeAction, &QAction::triggered, this, &MainWindow::goToTime);
    connect(stopAction, &QAction::triggered, this, &MainWindow::stopPlayback);
    connect(previousAction, &QAction::triggered, this, &MainWindow::playPrevious);
    connect(nextAction, &QAction::triggered, this, &MainWindow::playNext);
    connect(backAction, &QAction::triggered, this, [this] { player_->seekRelative(-5000); });
    connect(forwardAction, &QAction::triggered, this, [this] { player_->seekRelative(5000); });
    connect(slowerAction, &QAction::triggered, this, [this] {
        speedCombo_->setCurrentIndex(qMax(0, speedCombo_->currentIndex() - 1));
    });
    connect(fasterAction, &QAction::triggered, this, [this] {
        speedCombo_->setCurrentIndex(qMin(speedCombo_->count() - 1, speedCombo_->currentIndex() + 1));
    });
    connect(normalSpeedAction, &QAction::triggered, this, [this] {
        const int index = speedCombo_->findData(1.0);
        if (index >= 0) {
            speedCombo_->setCurrentIndex(index);
        }
    });
    connect(loopAction_, &QAction::toggled, this, [this](bool checked) {
        loopPlaylist_ = checked;
        loopButton_->setChecked(checked);
        updateStatus();
    });
    connect(shuffleAction_, &QAction::toggled, this, [this](bool checked) {
        shufflePlaylist_ = checked;
        shuffleButton_->setChecked(checked);
        updateStatus();
    });
    connect(playSelectedAction, &QAction::triggered, this, [this] {
        if (playlistView_->currentRow() >= 0) {
            playIndex(playlistView_->currentRow());
        }
    });
    connect(removeSelectedAction, &QAction::triggered, this, &MainWindow::removeSelected);
    connect(clearPlaylistAction, &QAction::triggered, this, &MainWindow::clearPlaylist);
    connect(searchPlaylistAction, &QAction::triggered, this, &MainWindow::focusPlaylistSearch);
    connect(sortTitleAction, &QAction::triggered, this, [this] {
        sortPlaylist(PlaylistSort::Title);
    });
    connect(sortPathAction, &QAction::triggered, this, [this] {
        sortPlaylist(PlaylistSort::Path);
    });
    connect(removeDuplicatesAction, &QAction::triggered, this,
            &MainWindow::removeDuplicateEntries);
    connect(repeatPointAAction_, &QAction::triggered, this, &MainWindow::setRepeatPointA);
    connect(repeatPointBAction_, &QAction::triggered, this, &MainWindow::setRepeatPointB);
    connect(repeatClearAction_, &QAction::triggered, this, &MainWindow::clearRepeat);
    connect(previousChapterAction, &QAction::triggered, this, &MainWindow::playPreviousChapter);
    connect(nextChapterAction, &QAction::triggered, this, &MainWindow::playNextChapter);
    connect(muteAction, &QAction::triggered, this, &MainWindow::toggleMute);
    connect(volumeUpAction, &QAction::triggered, this, [this] { adjustVolume(5); });
    connect(volumeDownAction, &QAction::triggered, this, [this] { adjustVolume(-5); });
    connect(volume50Action, &QAction::triggered, this, [this] { volumeSlider_->setValue(50); });
    connect(volume100Action, &QAction::triggered, this, [this] { volumeSlider_->setValue(100); });
    connect(volume125Action, &QAction::triggered, this, [this] { volumeSlider_->setValue(125); });
    connect(fullscreenAction_, &QAction::triggered, this, &MainWindow::toggleFullscreen);
    connect(topmostAction_, &QAction::triggered, this, &MainWindow::toggleTopmost);
    connect(rotateClockwiseAction, &QAction::triggered, this, [this] { rotateVideo(90); });
    connect(rotateCounterclockwiseAction, &QAction::triggered, this, [this] { rotateVideo(-90); });
    connect(resetRotationAction, &QAction::triggered, this, &MainWindow::resetVideoRotation);
    connect(snapshotAction, &QAction::triggered, this, &MainWindow::takeSnapshot);
    connect(subtitleAction, &QAction::triggered, this, &MainWindow::openSubtitle);
    connect(subtitleStyleAction, &QAction::triggered, this, &MainWindow::showSubtitleStyleDialog);
    connect(copySubtitleAction, &QAction::triggered, this, &MainWindow::copyCurrentSubtitle);
    connect(showSubtitlesAction_, &QAction::toggled, this, &MainWindow::setEmbeddedSubtitlesVisible);
    connect(delayBackAction, &QAction::triggered, this, [this] { adjustSubtitleDelay(-100); });
    connect(delayForwardAction, &QAction::triggered, this, [this] { adjustSubtitleDelay(100); });
    connect(delayResetAction, &QAction::triggered, this, [this] {
        subtitleDelayMs_ = 0;
        statusLabel_->setText(tr("Subtitle delay reset"));
        showCanvasOsd(tr("Subtitle delay 0 ms"));
    });
    connect(sizeSmallerAction, &QAction::triggered, this, [this] { adjustSubtitleScale(-0.15); });
    connect(sizeBiggerAction, &QAction::triggered, this, [this] { adjustSubtitleScale(0.15); });
    connect(sizeResetAction, &QAction::triggered, this, [this] {
        subtitleScale_ = 1.0;
        videoCanvas_->setSubtitleScale(subtitleScale_);
        showCanvasOsd(tr("Subtitle size 100%"));
    });
    connect(positionHigherAction, &QAction::triggered, this, [this] { adjustSubtitleMargin(0.04); });
    connect(positionLowerAction, &QAction::triggered, this, [this] { adjustSubtitleMargin(-0.04); });
    connect(positionResetAction, &QAction::triggered, this, [this] {
        subtitleMargin_ = 0.055;
        videoCanvas_->setSubtitleBottomMargin(subtitleMargin_);
        showCanvasOsd(tr("Subtitle position reset"));
    });
    connect(snapshotSubtitlesAction_, &QAction::toggled, this, [this](bool checked) {
        snapshotWithSubtitles_ = checked;
    });
    connect(infoAction, &QAction::triggered, this, &MainWindow::showMediaInfo);
    connect(aiRecognitionAction_, &QAction::triggered, this, &MainWindow::showAiContentDialog);
    connect(customSleepAction, &QAction::triggered, this, &MainWindow::showSleepTimerDialog);
    connect(cancelSleepTimerAction_, &QAction::triggered, this, &MainWindow::cancelSleepTimer);
    connect(revealAction, &QAction::triggered, this, &MainWindow::revealCurrentFile);
    connect(playlistAction_, &QAction::toggled, this, [this](bool checked) {
        playlistPanel_->setVisible(checked && !miniMode_ && !fullscreen_);
    });
    connect(lightThemeAction_, &QAction::toggled, this, [this](bool checked) {
        setDarkTheme(!checked);
    });
    connect(miniModeAction_, &QAction::toggled, this, [this](bool checked) {
        setMiniMode(checked);
    });
    connect(shortcutsAction, &QAction::triggered, this, &MainWindow::showShortcuts);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    connect(escapeFullscreen, &QAction::triggered, this, [this] {
        if (fullscreen_) {
            toggleFullscreen();
        } else if (miniMode_) {
            setMiniMode(false);
            miniModeAction_->setChecked(false);
        }
    });

    rebuildTrackMenus();
}

void MainWindow::configureStyle()
{
    if (darkTheme_) {
        setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget {
            background: #15171c;
            color: #eef0f3;
            font-family: "Segoe UI";
            font-size: 13px;
        }
        QMenuBar {
            background: #1d2027;
            border-bottom: 1px solid #30343d;
            padding: 3px 6px;
        }
        QMenuBar::item:selected, QMenu::item:selected {
            background: #343944;
        }
        QMenu {
            background: #22252c;
            border: 1px solid #3a3f49;
            padding: 5px;
        }
        QMenu::separator {
            height: 1px;
            background: #3a3f49;
            margin: 5px 8px;
        }
        #controlBar {
            background: #1d2027;
            border-top: 1px solid #30343d;
        }
        #panelTitle {
            font-size: 15px;
            font-weight: 600;
        }
        #mutedText, #timeText {
            color: #a7adb8;
        }
        QListWidget {
            background: #1a1d23;
            border: 1px solid #30343d;
            border-radius: 4px;
            padding: 4px;
            outline: none;
        }
        QListWidget::item {
            min-height: 28px;
            padding: 5px 7px;
            border-radius: 3px;
        }
        QListWidget::item:selected {
            background: #4a3324;
            color: #ffffff;
        }
        QToolButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 5px;
            min-width: 28px;
            min-height: 28px;
        }
        QToolButton:hover {
            background: #30343d;
            border-color: #424854;
        }
        #primaryButton {
            background: #e87325;
            border-color: #f08a43;
            min-width: 38px;
            min-height: 34px;
        }
        #primaryButton:hover {
            background: #f18438;
        }
        QPushButton {
            background: #272b33;
            border: 1px solid #3a3f49;
            border-radius: 4px;
            padding: 5px 10px;
        }
        QPushButton:hover {
            background: #313641;
        }
        QPushButton:checked {
            background: #4a3324;
            border-color: #e87325;
            color: #ffb985;
        }
        QComboBox {
            background: #272b33;
            border: 1px solid #3a3f49;
            border-radius: 4px;
            padding: 5px 8px;
        }
        QComboBox QAbstractItemView {
            background: #22252c;
            selection-background-color: #4a3324;
        }
        QSlider::groove:horizontal {
            height: 4px;
            background: #383d47;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #e87325;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: #f2f3f5;
            width: 12px;
            margin: -5px 0;
            border-radius: 6px;
        }
        QStatusBar {
            background: #1a1d23;
            border-top: 1px solid #30343d;
        }
        QLineEdit {
            background: #1a1d23;
            border: 1px solid #30343d;
            border-radius: 4px;
            padding: 4px 6px;
            selection-background-color: #4a3324;
        }
        QPlainTextEdit {
            background: #1a1d23;
            border: 1px solid #30343d;
            border-radius: 4px;
            color: #eef0f3;
        }
        QSplitter::handle {
            background: #30343d;
            width: 1px;
        }
    )"));
        return;
    }

    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget {
            background: #f4f5f7;
            color: #1c1e23;
            font-family: "Segoe UI";
            font-size: 13px;
        }
        QMenuBar {
            background: #e9ebef;
            border-bottom: 1px solid #c9cdd4;
            padding: 3px 6px;
        }
        QMenuBar::item:selected, QMenu::item:selected {
            background: #d8dce3;
        }
        QMenu {
            background: #ffffff;
            border: 1px solid #c9cdd4;
            padding: 5px;
        }
        QMenu::separator {
            height: 1px;
            background: #d5d9e0;
            margin: 5px 8px;
        }
        #controlBar {
            background: #e9ebef;
            border-top: 1px solid #c9cdd4;
        }
        #panelTitle {
            font-size: 15px;
            font-weight: 600;
        }
        #mutedText, #timeText {
            color: #5c636e;
        }
        QListWidget {
            background: #ffffff;
            border: 1px solid #c9cdd4;
            border-radius: 4px;
            padding: 4px;
            outline: none;
        }
        QListWidget::item {
            min-height: 28px;
            padding: 5px 7px;
            border-radius: 3px;
        }
        QListWidget::item:selected {
            background: #f7dcc7;
            color: #1c1e23;
        }
        QToolButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 5px;
            min-width: 28px;
            min-height: 28px;
        }
        QToolButton:hover {
            background: #d8dce3;
            border-color: #b9bfc9;
        }
        #primaryButton {
            background: #e87325;
            border-color: #f08a43;
            min-width: 38px;
            min-height: 34px;
        }
        #primaryButton:hover {
            background: #f18438;
        }
        QPushButton {
            background: #ffffff;
            border: 1px solid #c9cdd4;
            border-radius: 4px;
            padding: 5px 10px;
        }
        QPushButton:hover {
            background: #e9ebef;
        }
        QPushButton:checked {
            background: #f7dcc7;
            border-color: #e87325;
            color: #8a4310;
        }
        QComboBox {
            background: #ffffff;
            border: 1px solid #c9cdd4;
            border-radius: 4px;
            padding: 5px 8px;
        }
        QComboBox QAbstractItemView {
            background: #ffffff;
            selection-background-color: #f7dcc7;
            selection-color: #1c1e23;
        }
        QLineEdit {
            background: #ffffff;
            border: 1px solid #c9cdd4;
            border-radius: 4px;
            padding: 4px 6px;
        }
        QSlider::groove:horizontal {
            height: 4px;
            background: #c9cdd4;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #e87325;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: #4b4f57;
            width: 12px;
            margin: -5px 0;
            border-radius: 6px;
        }
        QStatusBar {
            background: #e9ebef;
            border-top: 1px solid #c9cdd4;
        }
        QSplitter::handle {
            background: #c9cdd4;
            width: 1px;
        }
        QPlainTextEdit {
            background: #ffffff;
            border: 1px solid #c9cdd4;
            border-radius: 4px;
        }
    )"));
}

void MainWindow::applyTheme()
{
    configureStyle();
    updatePlaylistButtons();
}

void MainWindow::setDarkTheme(bool dark)
{
    if (darkTheme_ == dark) {
        return;
    }
    darkTheme_ = dark;
    applyTheme();
    if (lightThemeAction_) {
        lightThemeAction_->setChecked(!dark);
    }
    statusLabel_->setText(dark ? tr("Dark theme") : tr("Light theme"));
}

void MainWindow::connectPlayer()
{
    connect(player_, &FFmpegPlayer::stateChanged, this, &MainWindow::updateState);
    connect(player_, &FFmpegPlayer::mediaOpened, this,
            [this](qint64 durationMs, bool hasAudio, bool hasVideo) {
                updateDuration(durationMs);
                if (pendingResumeMs_ >= 0) {
                    const qint64 resumeMs = pendingResumeMs_;
                    pendingResumeMs_ = -1;
                    if (durationMs > 0 && resumeMs >= durationMs - 1000) {
                        player_->setPosition(0);
                    } else if (resumeMs > 5000) {
                        showCanvasOsd(tr("Resumed at %1").arg(formatTime(resumeMs)));
                    }
                }
                metadataRotation_ = player_->videoRotation();
                manualRotation_ = 0;
                videoCanvas_->setRotation(metadataRotation_);
                lastAiVisionPositionMs_ = -1;
                rebuildTrackMenus();
                rebuildHlsMenu();
                rebuildChapterMenu();
                if (aiService_->isRunning()) {
                    const QString target = player_->currentTarget();
                    if (!aiMediaTarget_.isEmpty() && aiMediaTarget_ != target) {
                        aiTranscript_.clear();
                        aiCoreContent_.clear();
                        aiService_->resetSession();
                        if (aiContentDialog_) {
                            aiContentDialog_->setTranscript({});
                            aiContentDialog_->setCoreContent({});
                        }
                    }
                    aiMediaTarget_ = target;
                    player_->setRecognitionAudioEnabled(
                        aiService_->config().audioEnabled && hasAudio);
                    if (aiVisionTimer_) {
                        aiVisionTimer_->stop();
                    }
                    if (aiService_->config().visionEnabled && hasVideo && aiVisionTimer_) {
                        aiVisionTimer_->setInterval(
                            aiService_->config().visionIntervalSeconds * 1000);
                        aiVisionTimer_->start();
                        QTimer::singleShot(900, this, &MainWindow::captureAiVideoFrame);
                    }
                }
                if (currentIndex_ >= 0 && currentIndex_ < playlist_.size()
                    && !playlist_.at(currentIndex_).isUrl) {
                    addRecentTarget(playlist_.at(currentIndex_).target);
                }
                QStringList streamKinds;
                if (hasVideo) {
                    streamKinds.append(tr("video"));
                }
                if (hasAudio) {
                    streamKinds.append(tr("audio"));
                }
                setCanvasPlaceholder(hasVideo ? QStringLiteral("Decoding video...")
                                              : QStringLiteral("Audio playback"));
                statusLabel_->setText(tr("Opened %1 stream(s)").arg(streamKinds.join(tr(" + "))));
            });
    connect(player_, &FFmpegPlayer::mediaInfoChanged, this, [this](const QString& info) {
        lastInfo_ = info;
        updateStatus();
    });
    connect(player_, &FFmpegPlayer::hlsVariantsChanged,
            this, &MainWindow::rebuildHlsMenu);
    connect(player_, &FFmpegPlayer::hlsVariantChanged,
            this, [this](int index) {
                rebuildHlsMenu();
                if (index >= 0) {
                    const QVector<HlsVariant> variants = player_->hlsVariants();
                    if (index < variants.size()) {
                        statusLabel_->setText(
                            tr("HLS quality: %1").arg(variants.at(index).label()));
                    }
                } else if (player_->hlsAutoMode()) {
                    statusLabel_->setText(tr("HLS quality: Auto"));
                }
            });
    connect(player_, &FFmpegPlayer::errorOccurred, this, [this](const QString& message) {
        showError(tr("Playback error"), message);
    }, Qt::QueuedConnection);
    connect(player_, &FFmpegPlayer::playbackFinished, this, [this] {
        const int next = nextPlaylistIndex(true);
        if (next >= 0 && (loopPlaylist_ || shufflePlaylist_
                          || currentIndex_ + 1 < playlist_.size())) {
            playIndex(next);
        } else {
            updateStatus();
        }
    }, Qt::QueuedConnection);
}

void MainWindow::connectAiRecognition()
{
    connect(player_, &FFmpegPlayer::audioSamplesReady,
            aiService_, &AiRecognitionService::pushAudioSamples);
    connect(player_, &FFmpegPlayer::seekRequested, this, [this](qint64) {
        aiService_->flushAudio();
        lastAiVisionPositionMs_ = -1;
        if (aiService_->isRunning() && aiService_->config().visionEnabled
            && player_->hasVideo()) {
            QTimer::singleShot(700, this, &MainWindow::captureAiVideoFrame);
        }
    });

    connect(aiService_, &AiRecognitionService::runningChanged, this,
            [this](bool running) {
                lastAiVisionPositionMs_ = -1;
                player_->setRecognitionAudioEnabled(running
                                                    && aiService_->config().audioEnabled
                                                    && player_->hasAudio());
                if (aiVisionTimer_) {
                    aiVisionTimer_->stop();
                }
                if (running && aiService_->config().visionEnabled && player_->hasVideo()) {
                    aiVisionTimer_->setInterval(
                        aiService_->config().visionIntervalSeconds * 1000);
                    aiVisionTimer_->start();
                    QTimer::singleShot(900, this, &MainWindow::captureAiVideoFrame);
                }
                if (aiContentDialog_) {
                    aiContentDialog_->setRunning(running);
                }
                if (aiRecognitionAction_) {
                    aiRecognitionAction_->setText(
                        running ? tr("AI Recognition (Running)...")
                                : tr("AI Speech and Video Recognition..."));
                }
            });
    connect(aiService_, &AiRecognitionService::statusChanged, this,
            [this](const QString& status) {
                statusLabel_->setText(status);
                if (aiContentDialog_) {
                    aiContentDialog_->setStatus(status);
                }
            });
    connect(aiService_, &AiRecognitionService::errorOccurred, this,
            [this](const QString& message) {
                statusLabel_->setText(message);
                if (aiContentDialog_) {
                    aiContentDialog_->showServiceError(message);
                }
            });
    connect(aiService_, &AiRecognitionService::transcriptSegmentReady, this,
            [this](const QString& text, qint64 startMs, qint64 endMs) {
                const QString line = tr("[%1-%2] %3")
                                         .arg(formatTime(startMs), formatTime(endMs), text);
                if (!aiTranscript_.isEmpty()) {
                    aiTranscript_.append(QChar('\n'));
                }
                aiTranscript_.append(line);
                if (aiTranscript_.size() > 30000) {
                    aiTranscript_ = aiTranscript_.right(26000);
                }
                if (aiContentDialog_) {
                    aiContentDialog_->setTranscript(aiTranscript_);
                }
            });
    connect(aiService_, &AiRecognitionService::visionNoteReady, this,
            [this](const QString& text, qint64 positionMs) {
                const QString line = tr("[Video %1] %2")
                                         .arg(formatTime(positionMs), text);
                if (!aiTranscript_.isEmpty()) {
                    aiTranscript_.append(QChar('\n'));
                }
                aiTranscript_.append(line);
                if (aiTranscript_.size() > 30000) {
                    aiTranscript_ = aiTranscript_.right(26000);
                }
                if (aiContentDialog_) {
                    aiContentDialog_->setTranscript(aiTranscript_);
                }
            });
    connect(aiService_, &AiRecognitionService::coreContentUpdated, this,
            [this](const QString& summary) {
                aiCoreContent_ = summary;
                if (aiContentDialog_) {
                    aiContentDialog_->setCoreContent(summary);
                }
            });
}

void MainWindow::loadSettings()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    QSettings settings(QDir(directory).filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);

    savedVolume_ = std::clamp(settings.value(QStringLiteral("playback/volume"), 100).toInt(), 0, 125);
    lastVolume_ = savedVolume_ > 0 ? savedVolume_ : 100;
    muted_ = settings.value(QStringLiteral("playback/muted"), false).toBool();
    loopPlaylist_ = settings.value(QStringLiteral("playback/loop"), false).toBool();
    shufflePlaylist_ = settings.value(QStringLiteral("playback/shuffle"), false).toBool();
    topmost_ = settings.value(QStringLiteral("window/topmost"), false).toBool();
    embeddedSubtitlesVisible_ = settings.value(QStringLiteral("subtitle/visible"), true).toBool();
    subtitleDelayMs_ = settings.value(QStringLiteral("subtitle/delay"), 0).toInt();
    subtitleScale_ = std::clamp(settings.value(QStringLiteral("subtitle/scale"), 1.0).toDouble(),
                                0.5, 2.5);
    subtitleMargin_ = std::clamp(settings.value(QStringLiteral("subtitle/margin"), 0.055).toDouble(),
                                 0.02, 0.45);
    subtitleStyle_.fontFamily = settings.value(QStringLiteral("subtitle/font")).toString();
    const QColor savedTextColor(settings.value(QStringLiteral("subtitle/textColor"),
                                              QStringLiteral("#ffffff")).toString());
    if (savedTextColor.isValid()) {
        subtitleStyle_.textColor = savedTextColor;
    }
    const QColor savedOutlineColor(settings.value(QStringLiteral("subtitle/outlineColor"),
                                                 QStringLiteral("#000000")).toString());
    if (savedOutlineColor.isValid()) {
        subtitleStyle_.outlineColor = savedOutlineColor;
    }
    subtitleStyle_.outlineWidth = std::clamp(
        settings.value(QStringLiteral("subtitle/outlineWidth"), 0).toInt(), 0, 8);
    const QColor savedBackgroundColor(settings.value(QStringLiteral("subtitle/backgroundColor"),
                                                     QStringLiteral("#000000")).toString());
    if (savedBackgroundColor.isValid()) {
        subtitleStyle_.backgroundColor = savedBackgroundColor;
    }
    subtitleStyle_.backgroundOpacity = std::clamp(
        settings.value(QStringLiteral("subtitle/backgroundOpacity"), 180).toInt(), 0, 255);
    snapshotWithSubtitles_ = settings.value(QStringLiteral("snapshot/subtitles"), false).toBool();
    recentFiles_ = settings.value(QStringLiteral("recent/files")).toStringList();
    darkTheme_ = settings.value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString()
        != QStringLiteral("light");

    const AiRecognitionService::Config aiDefaults;
    const QString environmentBaseUrl = qEnvironmentVariable("OPENAI_BASE_URL");
    const QString environmentApiKey = qEnvironmentVariable("OPENAI_API_KEY");
    aiConfig_.baseUrl = settings.value(
        QStringLiteral("ai/baseUrl"),
        environmentBaseUrl.isEmpty() ? aiDefaults.baseUrl : environmentBaseUrl).toString();
    aiConfig_.apiKey = settings.value(
        QStringLiteral("ai/apiKey"),
        environmentApiKey.isEmpty() ? aiDefaults.apiKey : environmentApiKey).toString();
    aiConfig_.transcriptionModel = settings.value(
        QStringLiteral("ai/transcriptionModel"), aiDefaults.transcriptionModel).toString();
    aiConfig_.textModel = settings.value(
        QStringLiteral("ai/textModel"), aiDefaults.textModel).toString();
    aiConfig_.visionModel = settings.value(
        QStringLiteral("ai/visionModel"), aiDefaults.visionModel).toString();
    aiConfig_.transcriptionLanguage = settings.value(
        QStringLiteral("ai/transcriptionLanguage"), aiDefaults.transcriptionLanguage).toString();
    aiConfig_.summaryLanguage = settings.value(
        QStringLiteral("ai/summaryLanguage"), aiDefaults.summaryLanguage).toString();
    aiConfig_.audioEnabled = settings.value(
        QStringLiteral("ai/audioEnabled"), aiDefaults.audioEnabled).toBool();
    aiConfig_.visionEnabled = settings.value(
        QStringLiteral("ai/visionEnabled"), aiDefaults.visionEnabled).toBool();
    aiConfig_.audioChunkSeconds = std::clamp(
        settings.value(QStringLiteral("ai/audioChunkSeconds"), aiDefaults.audioChunkSeconds).toInt(),
        5, 30);
    aiConfig_.visionIntervalSeconds = std::clamp(
        settings.value(QStringLiteral("ai/visionIntervalSeconds"),
                       aiDefaults.visionIntervalSeconds).toInt(),
        5, 120);
    aiService_->setConfig(aiConfig_);

    const double rate = std::clamp(settings.value(QStringLiteral("playback/rate"), 1.0).toDouble(),
                                   0.25, 4.0);
    const int rateIndex = speedCombo_->findData(rate);
    if (rateIndex >= 0) {
        speedCombo_->setCurrentIndex(rateIndex);
    }

    loopButton_->setChecked(loopPlaylist_);
    shuffleButton_->setChecked(shufflePlaylist_);
    loopAction_->setChecked(loopPlaylist_);
    shuffleAction_->setChecked(shufflePlaylist_);
    playlistAction_->setChecked(settings.value(QStringLiteral("view/playlist"), true).toBool());
    playlistPanel_->setVisible(playlistAction_->isChecked());
    topmostAction_->setChecked(topmost_);
    if (topmost_) {
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
        show();
    }

    showSubtitlesAction_->setChecked(embeddedSubtitlesVisible_);
    snapshotSubtitlesAction_->setChecked(snapshotWithSubtitles_);
    videoCanvas_->setSubtitleScale(subtitleScale_);
    videoCanvas_->setSubtitleBottomMargin(subtitleMargin_);

    const int fitMode = std::clamp(settings.value(QStringLiteral("video/fitMode"), 0).toInt(), 0, 5);
    const auto mode = static_cast<VideoCanvas::FitMode>(fitMode);
    videoCanvas_->setFitMode(mode);
    if (fitModeGroup_) {
        for (QAction* action : fitModeGroup_->actions()) {
            action->setChecked(action->data().toInt() == fitMode);
        }
    }

    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }

    const int itemCount = settings.beginReadArray(QStringLiteral("session/items"));
    for (int index = 0; index < itemCount; ++index) {
        settings.setArrayIndex(index);
        PlaylistEntry entry;
        entry.target = settings.value(QStringLiteral("target")).toString();
        entry.title = settings.value(QStringLiteral("title")).toString();
        if (entry.target.isEmpty()) {
            continue;
        }
        entry.isUrl = settings.value(QStringLiteral("isUrl"), isUrl(entry.target)).toBool();
        if (entry.title.isEmpty()) {
            entry.title = makeTitle(entry.target);
        }
        appendPlaylistEntry(entry, false);
    }
    settings.endArray();

    sessionIndex_ = settings.value(QStringLiteral("session/current"), -1).toInt();
    sessionPosition_ = qMax<qint64>(0, settings.value(QStringLiteral("session/position"), 0).toLongLong());
    if (sessionIndex_ < 0 || sessionIndex_ >= playlist_.size()) {
        sessionIndex_ = -1;
        sessionPosition_ = 0;
    }

    const int resumeCount = settings.beginReadArray(QStringLiteral("resume/items"));
    for (int index = 0; index < resumeCount; ++index) {
        settings.setArrayIndex(index);
        const QString target = settings.value(QStringLiteral("target")).toString();
        const qint64 position = settings.value(QStringLiteral("position"), 0).toLongLong();
        if (!target.isEmpty() && position > 0) {
            resumePositions_.insert(target, position);
        }
    }
    settings.endArray();

    const QString languageCode = settings.value(QStringLiteral("ui/language")).toString();
    uiLanguage_ = languageCode.isEmpty()
        ? UiLanguageSupport::systemDefault()
        : UiLanguageSupport::fromCode(languageCode);
    if (englishLanguageAction_) {
        englishLanguageAction_->setChecked(uiLanguage_ == UiLanguage::English);
    }
    if (chineseLanguageAction_) {
        chineseLanguageAction_->setChecked(uiLanguage_ == UiLanguage::Chinese);
    }

    if (lightThemeAction_) {
        const QSignalBlocker blocker(lightThemeAction_);
        lightThemeAction_->setChecked(!darkTheme_);
    }
    applyTheme();
    applySubtitleStyle();
    updatePlaylistButtons();
}

void MainWindow::saveSettings()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    QSettings settings(QDir(directory).filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("playback/volume"), savedVolume_);
    settings.setValue(QStringLiteral("playback/muted"), muted_);
    settings.setValue(QStringLiteral("playback/rate"), player_->playbackRate());
    settings.setValue(QStringLiteral("playback/loop"), loopPlaylist_);
    settings.setValue(QStringLiteral("playback/shuffle"), shufflePlaylist_);
    settings.setValue(QStringLiteral("view/playlist"), playlistAction_->isChecked());
    settings.setValue(QStringLiteral("window/topmost"), miniMode_ ? miniRestoreTopmost_ : topmost_);
    settings.setValue(QStringLiteral("window/geometry"),
                      miniMode_ && !miniRestoreGeometry_.isEmpty() ? miniRestoreGeometry_
                                                                   : saveGeometry());
    settings.setValue(QStringLiteral("video/fitMode"), static_cast<int>(videoCanvas_->fitMode()));
    settings.setValue(QStringLiteral("subtitle/visible"), embeddedSubtitlesVisible_);
    settings.setValue(QStringLiteral("subtitle/delay"), subtitleDelayMs_);
    settings.setValue(QStringLiteral("subtitle/scale"), subtitleScale_);
    settings.setValue(QStringLiteral("subtitle/margin"), subtitleMargin_);
    settings.setValue(QStringLiteral("subtitle/font"), subtitleStyle_.fontFamily);
    settings.setValue(QStringLiteral("subtitle/textColor"), subtitleStyle_.textColor.name());
    settings.setValue(QStringLiteral("subtitle/outlineColor"), subtitleStyle_.outlineColor.name());
    settings.setValue(QStringLiteral("subtitle/outlineWidth"), subtitleStyle_.outlineWidth);
    settings.setValue(QStringLiteral("subtitle/backgroundColor"),
                      subtitleStyle_.backgroundColor.name());
    settings.setValue(QStringLiteral("subtitle/backgroundOpacity"),
                      subtitleStyle_.backgroundOpacity);
    settings.setValue(QStringLiteral("snapshot/subtitles"), snapshotWithSubtitles_);
    settings.setValue(QStringLiteral("recent/files"), recentFiles_);
    settings.setValue(QStringLiteral("ui/language"), UiLanguageSupport::code(uiLanguage_));
    settings.setValue(QStringLiteral("ui/theme"),
                      darkTheme_ ? QStringLiteral("dark") : QStringLiteral("light"));
    settings.setValue(QStringLiteral("ai/baseUrl"), aiConfig_.baseUrl);
    settings.setValue(QStringLiteral("ai/apiKey"), aiConfig_.apiKey);
    settings.setValue(QStringLiteral("ai/transcriptionModel"), aiConfig_.transcriptionModel);
    settings.setValue(QStringLiteral("ai/textModel"), aiConfig_.textModel);
    settings.setValue(QStringLiteral("ai/visionModel"), aiConfig_.visionModel);
    settings.setValue(QStringLiteral("ai/transcriptionLanguage"),
                      aiConfig_.transcriptionLanguage);
    settings.setValue(QStringLiteral("ai/summaryLanguage"), aiConfig_.summaryLanguage);
    settings.setValue(QStringLiteral("ai/audioEnabled"), aiConfig_.audioEnabled);
    settings.setValue(QStringLiteral("ai/visionEnabled"), aiConfig_.visionEnabled);
    settings.setValue(QStringLiteral("ai/audioChunkSeconds"), aiConfig_.audioChunkSeconds);
    settings.setValue(QStringLiteral("ai/visionIntervalSeconds"),
                      aiConfig_.visionIntervalSeconds);

    rememberCurrentPosition();
    settings.beginWriteArray(QStringLiteral("resume/items"), resumePositions_.size());
    int resumeIndex = 0;
    for (auto it = resumePositions_.constBegin(); it != resumePositions_.constEnd(); ++it) {
        settings.setArrayIndex(resumeIndex++);
        settings.setValue(QStringLiteral("target"), it.key());
        settings.setValue(QStringLiteral("position"), it.value());
    }
    settings.endArray();

    settings.beginWriteArray(QStringLiteral("session/items"), playlist_.size());
    for (int index = 0; index < playlist_.size(); ++index) {
        settings.setArrayIndex(index);
        settings.setValue(QStringLiteral("target"), playlist_.at(index).target);
        settings.setValue(QStringLiteral("title"), playlist_.at(index).title);
        settings.setValue(QStringLiteral("isUrl"), playlist_.at(index).isUrl);
    }
    settings.endArray();
    settings.setValue(QStringLiteral("session/current"), currentIndex_);

    qint64 position = player_->position();
    const FFmpegPlayer::State state = player_->state();
    if (state == FFmpegPlayer::State::Idle || state == FFmpegPlayer::State::Stopped
        || state == FFmpegPlayer::State::Ended || state == FFmpegPlayer::State::Error) {
        position = 0;
    }
    settings.setValue(QStringLiteral("session/position"), position);
    settings.sync();
}

void MainWindow::rebuildTrackMenus()
{
    if (!audioTrackMenu_ || !subtitleTrackMenu_) {
        return;
    }

    delete audioTrackGroup_;
    audioTrackGroup_ = new QActionGroup(this);
    audioTrackGroup_->setExclusive(true);
    audioTrackMenu_->clear();

    auto addAudioAction = [this](const QString& label, int streamIndex) {
        QAction* action = audioTrackMenu_->addAction(label);
        action->setCheckable(true);
        action->setData(streamIndex);
        audioTrackGroup_->addAction(action);
        const int requested = player_->requestedAudioStream();
        action->setChecked(streamIndex >= 0 ? requested == streamIndex : requested < 0);
        connect(action, &QAction::triggered, this, [this, streamIndex] {
            player_->selectAudioStream(streamIndex);
            updateStatus();
            showCanvasOsd(tr("Audio track changed"));
        });
    };

    const QVector<FFmpegPlayer::TrackInfo> audioTracks = player_->audioTracks();
    addAudioAction(tr("Auto"), -1);
    if (audioTracks.isEmpty()) {
        audioTrackMenu_->addSeparator();
        QAction* empty = audioTrackMenu_->addAction(tr("No audio tracks"));
        empty->setEnabled(false);
    } else {
        for (const FFmpegPlayer::TrackInfo& track : audioTracks) {
            addAudioAction(tr("Track #%1  %2").arg(track.index).arg(track.label), track.index);
        }
    }
    audioTrackMenu_->setEnabled(true);

    delete subtitleTrackGroup_;
    subtitleTrackGroup_ = new QActionGroup(this);
    subtitleTrackGroup_->setExclusive(true);
    subtitleTrackMenu_->clear();

    auto addSubtitleAction = [this](const QString& label, int streamIndex) {
        QAction* action = subtitleTrackMenu_->addAction(label);
        action->setCheckable(true);
        action->setData(streamIndex);
        subtitleTrackGroup_->addAction(action);
        action->setChecked(player_->requestedSubtitleStream() == streamIndex);
        connect(action, &QAction::triggered, this, [this, streamIndex] {
            subtitleTrack_->clear();
            subtitleText_.clear();
            videoCanvas_->setSubtitleText({});
            player_->selectSubtitleStream(streamIndex);
            embeddedSubtitlesVisible_ = true;
            showSubtitlesAction_->setChecked(true);
            updateStatus();
            showCanvasOsd(tr("Embedded subtitle track changed"));
        });
    };

    addSubtitleAction(tr("None"), -2);
    addSubtitleAction(tr("Automatic"), -1);
    const QVector<FFmpegPlayer::TrackInfo> subtitleTracks = player_->subtitleTracks();
    if (subtitleTracks.isEmpty()) {
        subtitleTrackMenu_->addSeparator();
        QAction* empty = subtitleTrackMenu_->addAction(tr("No embedded subtitle tracks"));
        empty->setEnabled(false);
    } else {
        for (const FFmpegPlayer::TrackInfo& track : subtitleTracks) {
            addSubtitleAction(tr("Track #%1  %2").arg(track.index).arg(track.label), track.index);
        }
    }
}

void MainWindow::rebuildHlsMenu()
{
    if (!hlsQualityMenu_) {
        return;
    }

    delete hlsQualityGroup_;
    hlsQualityGroup_ = new QActionGroup(this);
    hlsQualityGroup_->setExclusive(true);
    hlsQualityMenu_->clear();

    QAction* autoAction = hlsQualityMenu_->addAction(tr("Auto"));
    autoAction->setCheckable(true);
    autoAction->setData(-1);
    autoAction->setChecked(player_->hlsAutoMode());
    hlsQualityGroup_->addAction(autoAction);
    connect(autoAction, &QAction::triggered, this, [this] {
        player_->setHlsAutoMode(true);
        rebuildHlsMenu();
    });
    hlsQualityMenu_->addSeparator();

    const QVector<HlsVariant> variants = player_->hlsVariants();
    const int activeIndex = player_->activeHlsVariant();
    if (variants.isEmpty()) {
        QAction* empty = hlsQualityMenu_->addAction(tr("No HLS variants"));
        empty->setEnabled(false);
        return;
    }

    for (int index = 0; index < variants.size(); ++index) {
        QAction* action = hlsQualityMenu_->addAction(
            QStringLiteral("%1  %2").arg(index + 1).arg(variants.at(index).label()));
        action->setCheckable(true);
        action->setData(index);
        action->setChecked(!player_->hlsAutoMode() && index == activeIndex);
        hlsQualityGroup_->addAction(action);
        connect(action, &QAction::triggered, this, [this, index] {
            player_->selectHlsVariant(index);
            rebuildHlsMenu();
        });
    }
}

void MainWindow::rebuildChapterMenu()
{
    if (!chapterMenu_) {
        return;
    }
    for (QAction* action : chapterEntryActions_) {
        chapterMenu_->removeAction(action);
        delete action;
    }
    chapterEntryActions_.clear();

    const QVector<FFmpegPlayer::ChapterInfo> chapters = player_->chapters();
    if (chapters.isEmpty()) {
        QAction* empty = chapterMenu_->addAction(tr("No chapters"));
        empty->setEnabled(false);
        chapterEntryActions_.append(empty);
        return;
    }

    for (int index = 0; index < chapters.size(); ++index) {
        const FFmpegPlayer::ChapterInfo& chapter = chapters.at(index);
        const QString label = chapter.title.isEmpty()
            ? tr("Chapter %1").arg(index + 1)
            : tr("Chapter %1  %2").arg(index + 1).arg(chapter.title);
        QAction* action = chapterMenu_->addAction(label);
        action->setData(chapter.startMs);
        connect(action, &QAction::triggered, this, [this, chapter, label] {
            player_->setPosition(chapter.startMs);
            statusLabel_->setText(label);
            showCanvasOsd(label);
        });
        chapterEntryActions_.append(action);
    }
}

void MainWindow::rebuildRecentMenu()
{
    if (!recentMenu_) {
        return;
    }
    recentMenu_->clear();
    if (recentFiles_.isEmpty()) {
        QAction* empty = recentMenu_->addAction(tr("No recent files"));
        empty->setEnabled(false);
        return;
    }

    int count = 0;
    for (const QString& path : recentFiles_) {
        if (count >= 12) {
            break;
        }
        ++count;
        QAction* action = recentMenu_->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] {
            addPaths({path}, OpenMode::Replace);
        });
    }
    recentMenu_->addSeparator();
    QAction* clearAction = recentMenu_->addAction(tr("Clear Recent Files"));
    connect(clearAction, &QAction::triggered, this, [this] {
        recentFiles_.clear();
        statusLabel_->setText(tr("Recent files cleared"));
    });
}

void MainWindow::addRecentTarget(const QString& target)
{
    if (target.isEmpty() || isUrl(target)) {
        return;
    }
    const QString path = QFileInfo(target).absoluteFilePath();
    recentFiles_.removeAll(path);
    recentFiles_.prepend(path);
    while (recentFiles_.size() > 12) {
        recentFiles_.removeLast();
    }
}

void MainWindow::setFitMode(VideoCanvas::FitMode mode)
{
    if (!videoCanvas_) {
        return;
    }
    videoCanvas_->setFitMode(mode);
    if (fitModeGroup_) {
        for (QAction* action : fitModeGroup_->actions()) {
            action->setChecked(action->data().toInt() == static_cast<int>(mode));
        }
    }

    QString label;
    switch (mode) {
    case VideoCanvas::FitMode::Fit:
        label = tr("Fit to window");
        break;
    case VideoCanvas::FitMode::Fill:
        label = tr("Fill and crop");
        break;
    case VideoCanvas::FitMode::Stretch:
        label = tr("Stretch");
        break;
    case VideoCanvas::FitMode::Ratio16x9:
        label = QStringLiteral("16:9");
        break;
    case VideoCanvas::FitMode::Ratio4x3:
        label = QStringLiteral("4:3");
        break;
    case VideoCanvas::FitMode::Original:
        label = tr("Original size");
        break;
    }
    statusLabel_->setText(tr("Aspect ratio: %1").arg(label));
    showCanvasOsd(label);
}

void MainWindow::adjustSubtitleDelay(int deltaMs)
{
    subtitleDelayMs_ = std::clamp(subtitleDelayMs_ + deltaMs, -60000, 60000);
    statusLabel_->setText(tr("Subtitle delay: %1 ms").arg(subtitleDelayMs_));
    showCanvasOsd(tr("Subtitle delay %1 ms").arg(subtitleDelayMs_));
    updateSubtitle(player_->position());
}

void MainWindow::adjustSubtitleScale(double delta)
{
    subtitleScale_ = std::clamp(subtitleScale_ + delta, 0.5, 2.5);
    videoCanvas_->setSubtitleScale(subtitleScale_);
    const int percent = static_cast<int>(std::lround(subtitleScale_ * 100.0));
    statusLabel_->setText(tr("Subtitle size: %1%").arg(percent));
    showCanvasOsd(tr("Subtitle size %1%").arg(percent));
}

void MainWindow::adjustSubtitleMargin(double delta)
{
    subtitleMargin_ = std::clamp(subtitleMargin_ + delta, 0.02, 0.45);
    videoCanvas_->setSubtitleBottomMargin(subtitleMargin_);
    showCanvasOsd(tr("Subtitle position %1%").arg(static_cast<int>(std::lround(subtitleMargin_ * 100.0))));
}

void MainWindow::setEmbeddedSubtitlesVisible(bool visible)
{
    embeddedSubtitlesVisible_ = visible;
    if (!visible) {
        subtitleText_.clear();
        videoCanvas_->setSubtitleText({});
        statusLabel_->setText(tr("Subtitles hidden"));
    } else {
        updateSubtitle(player_->position());
        statusLabel_->setText(tr("Subtitles shown"));
    }
}

void MainWindow::syncPlaylistOrder()
{
    if (syncingPlaylistOrder_) {
        return;
    }
    syncingPlaylistOrder_ = true;

    const QString currentTarget = (currentIndex_ >= 0 && currentIndex_ < playlist_.size())
        ? playlist_.at(currentIndex_).target
        : QString();
    const bool hadCurrent = !currentTarget.isEmpty();

    QVector<PlaylistEntry> reordered;
    reordered.reserve(playlistView_->count());
    for (int row = 0; row < playlistView_->count(); ++row) {
        QListWidgetItem* item = playlistView_->item(row);
        if (!item) {
            continue;
        }
        PlaylistEntry entry;
        entry.target = item->data(Qt::UserRole).toString();
        entry.title = item->text();
        entry.isUrl = item->data(Qt::UserRole + 1).toBool();
        reordered.append(entry);
    }
    playlist_ = reordered;

    if (hadCurrent) {
        currentIndex_ = -1;
        for (int row = 0; row < playlist_.size(); ++row) {
            if (playlist_.at(row).target == currentTarget) {
                currentIndex_ = row;
                break;
            }
        }
    }

    syncingPlaylistOrder_ = false;
    updatePlaylistButtons();
}

void MainWindow::showCanvasOsd(const QString& text)
{
    if (uiReady_ && videoCanvas_) {
        videoCanvas_->showOsd(text);
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updateCompactChrome();
    if (controlBarOverlay_) {
        updateFullscreenChromeGeometry();
    }
}

void MainWindow::updateCompactChrome()
{
    if (!mediaLabel_ || !speedCombo_ || !loopButton_ || !shuffleButton_
        || !volumeSlider_ || !volumeLabel_) {
        return;
    }

    const int logicalWidth = width();
    speedCombo_->setVisible(logicalWidth >= 820);
    loopButton_->setVisible(logicalWidth >= 780);
    shuffleButton_->setVisible(logicalWidth >= 780);
    volumeSlider_->setVisible(logicalWidth >= 740);
    volumeLabel_->setVisible(logicalWidth >= 740);

    const bool showMedia = logicalWidth >= 700;
    mediaLabel_->setVisible(showMedia);
    const QString full = mediaTitle_.isEmpty() ? tr("No media loaded") : mediaTitle_;
    if (showMedia) {
        int available = mediaLabel_->width() - 8;
        if (available < 60) {
            available = qBound(80, controlBar_->width() / 3, 320);
        }
        mediaLabel_->setText(mediaLabel_->fontMetrics().elidedText(full, Qt::ElideMiddle, available));
    } else {
        mediaLabel_->setText(full);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (fullscreen_ && controlBarOverlay_ && event->type() == QEvent::MouseMove) {
        showFullscreenChrome();
    }

    if (watched == seekSlider_) {
        const QEvent::Type type = event->type();
        if (type == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                sliderDragging_ = true;
                seekSlider_->setSliderDown(true);
                // The press is consumed, so QSlider never grabs the mouse
                // itself; take the grab to keep receiving moves outside.
                seekSlider_->grabMouse();
                seekToSliderPosition(static_cast<int>(mouse->position().x()));
                return true;
            }
        } else if (type == QEvent::MouseMove && sliderDragging_) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            seekToSliderPosition(static_cast<int>(mouse->position().x()));
            return true;
        } else if (type == QEvent::MouseButtonRelease && sliderDragging_) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                seekToSliderPosition(static_cast<int>(mouse->position().x()));
                sliderDragging_ = false;
                seekSlider_->releaseMouse();
                // Emits sliderReleased, whose handler performs the seek.
                seekSlider_->setSliderDown(false);
                return true;
            }
        } else if (type == QEvent::UngrabMouse && sliderDragging_) {
            // Alt+Tab or a popup can steal the grab mid-drag; commit the
            // previewed position instead of leaving the slider stuck.
            sliderDragging_ = false;
            seekSlider_->setSliderDown(false);
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::seekToSliderPosition(int x)
{
    if (!seekSlider_ || seekSlider_->maximum() <= seekSlider_->minimum()) {
        return;
    }
    const int value = QStyle::sliderValueFromPosition(
        seekSlider_->minimum(),
        seekSlider_->maximum(),
        x,
        qMax(1, seekSlider_->width()),
        seekSlider_->invertedAppearance());
    seekSlider_->setValue(value);
    timeLabel_->setText(
        QStringLiteral("%1 / %2").arg(formatTime(value), formatTime(durationMs_)));
}

void MainWindow::openFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("Open media files"),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
        mediaNameFilters().join(QStringLiteral(";;")));
    if (!files.isEmpty()) {
        addPaths(files, OpenMode::Replace);
    }
}

void MainWindow::addFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("Add media files"),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
        mediaNameFilters().join(QStringLiteral(";;")));
    if (!files.isEmpty()) {
        addPaths(files, OpenMode::Append);
    }
}

void MainWindow::openFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(
        this,
        tr("Open media folder"),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    if (!folder.isEmpty()) {
        addPaths({folder}, playlist_.isEmpty() ? OpenMode::Replace : OpenMode::Append);
    }
}

void MainWindow::openUrl()
{
    bool accepted = false;
    const QString url = QInputDialog::getText(
        this,
        tr("Open network stream"),
        tr("Network URL"),
        QLineEdit::Normal,
        QStringLiteral("https://"),
        &accepted);
    if (!accepted) {
        return;
    }
    const QString target = url.trimmed();
    if (!isUrl(target)) {
        QMessageBox::warning(this, tr("Open network stream"),
                             tr("Enter a complete URL with a network scheme."));
        return;
    }
    addPaths({target}, OpenMode::Replace);
}

void MainWindow::savePlaylist()
{
    if (playlist_.isEmpty()) {
        statusLabel_->setText(tr("The playlist is empty."));
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        this,
        tr("Save playlist"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::MusicLocation))
            .filePath(QStringLiteral("playlist.m3u")),
        tr("M3U playlist (*.m3u);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".m3u");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        showError(tr("Save playlist"), file.errorString());
        return;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "#EXTM3U\n";
    for (const PlaylistEntry& entry : playlist_) {
        stream << "#EXTINF:-1," << entry.title << '\n';
        stream << entry.target << '\n';
    }
    statusLabel_->setText(tr("Playlist saved: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::openPlaylistFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open playlist"),
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
        tr("Playlists (*.m3u *.m3u8 *.pls);;All files (*.*)"));
    if (!path.isEmpty()) {
        addPaths({path}, OpenMode::Replace);
    }
}

void MainWindow::openSubtitle()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Load subtitle"),
        currentIndex_ >= 0 && !playlist_.at(currentIndex_).isUrl
            ? QFileInfo(playlist_.at(currentIndex_).target).absolutePath()
            : QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
        tr("Subtitles (*.srt *.ass *.ssa *.vtt);;All files (*.*)"));
    if (!path.isEmpty()) {
        setSubtitle(path);
    }
}

void MainWindow::takeSnapshot()
{
    const QImage image = videoCanvas_->snapshot(snapshotWithSubtitles_);
    if (image.isNull()) {
        statusLabel_->setText(tr("There is no video frame to save."));
        return;
    }

    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty()) {
        directory = QDir::currentPath();
    }
    directory = QDir(directory).filePath(QStringLiteral("FFmpeg Qt Player"));
    QDir().mkpath(directory);
    const QString path = QDir(directory).filePath(
        QStringLiteral("snapshot-%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
    if (image.save(path, "PNG")) {
        statusLabel_->setText(tr("Snapshot saved: %1").arg(path));
    } else {
        showError(tr("Snapshot"), tr("Cannot save the image to %1").arg(path));
    }
}

void MainWindow::toggleFullscreen()
{
    if (!fullscreen_) {
        if (miniMode_) {
            setMiniMode(false);
            if (miniModeAction_) {
                const QSignalBlocker blocker(miniModeAction_);
                miniModeAction_->setChecked(false);
            }
        }
        wasMaximized_ = isMaximized();
        fullscreen_ = true;
        fullscreenAction_->setChecked(true);
        enterFullscreenChrome();
    } else {
        fullscreen_ = false;
        fullscreenAction_->setChecked(false);
        // Going through showNormal() first would resize the window twice and
        // flash the desktop between states; one state change is enough.
        if (wasMaximized_) {
            showMaximized();
        } else {
            showNormal();
        }
        exitFullscreenChrome();
    }
}

void MainWindow::enterFullscreenChrome()
{
    menuBar()->hide();
    statusBar()->hide();
    playlistPanel_->hide();

    const int barHeight = qMax(controlBar_->height(), controlBar_->sizeHint().height());
    controlBar_->hide();
    centralLayout_->removeWidget(controlBar_);
    controlBar_->setParent(centralWidget());
    controlBar_->setFixedHeight(qMax(48, barHeight));
    controlBarOverlay_ = true;

    showFullScreen();
    // showFullScreen() applies the new geometry asynchronously, so laying the
    // overlay out now would place it at the old window size and make it jump.
    QTimer::singleShot(0, this, [this] {
        if (!fullscreen_ || !controlBarOverlay_) {
            return;
        }
        updateFullscreenChromeGeometry();
        showFullscreenChrome();
        if (videoCanvas_) {
            videoCanvas_->update();
        }
    });
}

void MainWindow::exitFullscreenChrome()
{
    if (fullscreenChromeTimer_) {
        fullscreenChromeTimer_->stop();
    }
    controlBarOverlay_ = false;
    controlBar_->hide();
    controlBar_->setMinimumHeight(0);
    controlBar_->setMaximumHeight(QWIDGETSIZE_MAX);
    centralLayout_->addWidget(controlBar_);
    controlBar_->show();
    unsetCursor();

    menuBar()->show();
    statusBar()->show();
    playlistPanel_->setVisible(playlistAction_->isChecked());
    updateCompactChrome();
    if (videoCanvas_) {
        videoCanvas_->update();
    }
}

void MainWindow::updateFullscreenChromeGeometry()
{
    if (!controlBarOverlay_ || !controlBar_ || !centralWidget()) {
        return;
    }
    const int barHeight = qMax(48, controlBar_->height() > 0
                                       ? controlBar_->height()
                                       : controlBar_->sizeHint().height());
    controlBar_->setGeometry(0,
                             centralWidget()->height() - barHeight,
                             centralWidget()->width(),
                             barHeight);
    controlBar_->raise();
}

void MainWindow::showFullscreenChrome()
{
    if (!fullscreen_ || !controlBarOverlay_ || !controlBar_) {
        return;
    }
    updateFullscreenChromeGeometry();
    controlBar_->show();
    controlBar_->raise();
    unsetCursor();
    restartFullscreenChromeTimer();
}

void MainWindow::hideFullscreenChrome()
{
    if (!fullscreen_ || !controlBarOverlay_ || !controlBar_) {
        return;
    }
    if (controlBar_->underMouse() || sliderDragging_) {
        restartFullscreenChromeTimer();
        return;
    }
    controlBar_->hide();
    setCursor(Qt::BlankCursor);
}

void MainWindow::restartFullscreenChromeTimer()
{
    if (fullscreenChromeTimer_) {
        fullscreenChromeTimer_->start(2500);
    }
}

void MainWindow::toggleTopmost()
{
    topmost_ = !topmost_;
    topmostAction_->setChecked(topmost_);
    const bool visible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, topmost_);
    if (visible) {
        show();
    }
    updateStatus();
}

void MainWindow::rotateVideo(int deltaDegrees)
{
    if (!player_->hasVideo()) {
        statusLabel_->setText(tr("The current media has no video stream."));
        return;
    }
    manualRotation_ = (manualRotation_ + deltaDegrees) % 360;
    if (manualRotation_ < 0) {
        manualRotation_ += 360;
    }
    const int displayed = (metadataRotation_ + manualRotation_) % 360;
    videoCanvas_->setRotation(displayed);
    statusLabel_->setText(tr("Rotation: %1°").arg(displayed));
    showCanvasOsd(tr("Rotation %1°").arg(displayed));
}

void MainWindow::resetVideoRotation()
{
    manualRotation_ = 0;
    videoCanvas_->setRotation(metadataRotation_);
    statusLabel_->setText(tr("Rotation reset"));
    showCanvasOsd(tr("Rotation reset"));
}

void MainWindow::setMiniMode(bool enabled)
{
    if (miniMode_ == enabled) {
        return;
    }
    miniMode_ = enabled;
    if (enabled) {
        if (fullscreen_) {
            toggleFullscreen();
        }
        miniRestoreGeometry_ = saveGeometry();
        miniRestoreTopmost_ = topmost_;
        const bool visible = isVisible();
        menuBar()->hide();
        statusBar()->hide();
        playlistPanel_->hide();
        if (!topmost_) {
            topmost_ = true;
            const QSignalBlocker blocker(topmostAction_);
            topmostAction_->setChecked(true);
            setWindowFlag(Qt::WindowStaysOnTopHint, true);
            if (visible) {
                show();
            }
        }
        setMinimumSize(400, 260);
        resize(560, 340);
        statusLabel_->setText(tr("Mini player enabled"));
    } else {
        const bool visible = isVisible();
        menuBar()->show();
        statusBar()->show();
        playlistPanel_->setVisible(playlistAction_->isChecked());
        if (!miniRestoreTopmost_) {
            topmost_ = false;
            const QSignalBlocker blocker(topmostAction_);
            topmostAction_->setChecked(false);
            setWindowFlag(Qt::WindowStaysOnTopHint, false);
            if (visible) {
                show();
            }
        }
        setMinimumSize(640, 400);
        if (!miniRestoreGeometry_.isEmpty()) {
            restoreGeometry(miniRestoreGeometry_);
        }
        statusLabel_->setText(tr("Mini player disabled"));
    }
    updateStatus();
}

void MainWindow::showSleepTimerDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Sleep Timer"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* minutes = new QSpinBox(&dialog);
    minutes->setRange(1, 24 * 60);
    minutes->setValue(30);
    minutes->setSuffix(tr(" min"));
    form->addRow(tr("Minutes:"), minutes);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted) {
        setSleepTimer(minutes->value());
    }
}

void MainWindow::setSleepTimer(int minutes)
{
    if (!sleepTimer_ || minutes <= 0) {
        return;
    }
    sleepRemainingMs_ = static_cast<qint64>(minutes) * 60 * 1000;
    sleepTimer_->start();
    if (cancelSleepTimerAction_) {
        cancelSleepTimerAction_->setEnabled(true);
    }
    showCanvasOsd(tr("Sleep timer: %1").arg(formatTime(sleepRemainingMs_)));
    updateStatus();
}

void MainWindow::cancelSleepTimer()
{
    if (sleepTimer_) {
        sleepTimer_->stop();
    }
    sleepRemainingMs_ = 0;
    if (cancelSleepTimerAction_) {
        cancelSleepTimerAction_->setEnabled(false);
    }
    statusLabel_->setText(tr("Sleep timer cancelled"));
    showCanvasOsd(tr("Sleep timer cancelled"));
}

void MainWindow::sleepTimerTick()
{
    sleepRemainingMs_ = qMax<qint64>(0, sleepRemainingMs_ - 1000);
    if (sleepRemainingMs_ > 0) {
        updateStatus();
        return;
    }

    sleepTimer_->stop();
    if (cancelSleepTimerAction_) {
        cancelSleepTimerAction_->setEnabled(false);
    }
    player_->pause();
    statusLabel_->setText(tr("Sleep timer finished"));
    showCanvasOsd(tr("Sleep timer finished"));
}

void MainWindow::showAiContentDialog()
{
    if (!aiContentDialog_) {
        aiContentDialog_ = new AiContentDialog(this);
        connect(aiContentDialog_, &AiContentDialog::startRequested,
                this, &MainWindow::startAiRecognition);
        connect(aiContentDialog_, &AiContentDialog::stopRequested,
                this, &MainWindow::stopAiRecognition);
    }
    aiContentDialog_->setConfig(aiService_->isRunning()
                                    ? aiService_->config()
                                    : aiConfig_);
    aiContentDialog_->setTranscript(aiTranscript_);
    aiContentDialog_->setCoreContent(aiCoreContent_);
    aiContentDialog_->setRunning(aiService_->isRunning());
    aiContentDialog_->setStatus(aiService_->isRunning()
                                    ? tr("AI recognition is running")
                                    : tr("Not started"));
    aiContentDialog_->show();
    aiContentDialog_->raise();
    aiContentDialog_->activateWindow();
}

void MainWindow::startAiRecognition()
{
    if (!aiContentDialog_) {
        return;
    }
    if (currentIndex_ < 0) {
        aiContentDialog_->showServiceError(tr("Open media before starting AI recognition."));
        return;
    }

    aiConfig_ = aiContentDialog_->config();
    if (!aiConfig_.audioEnabled && !aiConfig_.visionEnabled) {
        aiContentDialog_->showServiceError(
            tr("Enable audio recognition or video recognition first."));
        return;
    }
    if (aiConfig_.audioEnabled && !player_->hasAudio()
        && !(aiConfig_.visionEnabled && player_->hasVideo())) {
        aiContentDialog_->showServiceError(
            tr("The current media has no audio or video stream to recognize."));
        return;
    }

    aiService_->setConfig(aiConfig_);
    aiTranscript_.clear();
    aiCoreContent_.clear();
    aiMediaTarget_ = player_->currentTarget();
    lastAiVisionPositionMs_ = -1;
    aiContentDialog_->setTranscript({});
    aiContentDialog_->setCoreContent({});
    aiService_->start();
    saveSettings();
}

void MainWindow::stopAiRecognition()
{
    aiService_->stop();
    lastAiVisionPositionMs_ = -1;
    if (aiContentDialog_) {
        aiContentDialog_->setRunning(false);
        aiContentDialog_->setStatus(tr("AI recognition stopped"));
    }
}

void MainWindow::captureAiVideoFrame()
{
    if (!aiService_->isRunning() || !aiService_->config().visionEnabled
        || !player_->hasVideo()) {
        return;
    }
    const FFmpegPlayer::State state = player_->state();
    if (state != FFmpegPlayer::State::Playing
        && state != FFmpegPlayer::State::Paused) {
        return;
    }
    const qint64 positionMs = player_->position();
    if (lastAiVisionPositionMs_ >= 0
        && qAbs(positionMs - lastAiVisionPositionMs_) < 1000) {
        return;
    }
    const QImage frame = videoCanvas_->snapshot(false);
    if (!frame.isNull() && aiService_->pushVideoFrame(frame, positionMs)) {
        lastAiVisionPositionMs_ = positionMs;
    }
}

void MainWindow::showMediaInfo()
{
    QString detail = player_->mediaInfo();
    if (detail.isEmpty()) {
        detail = tr("No media has been opened.");
    } else {
        detail += QStringLiteral("\n\n");
        detail += tr("Source: %1").arg(player_->currentTarget());
        if (!isUrl(player_->currentTarget())) {
            const qint64 bytes = QFileInfo(player_->currentTarget()).size();
            if (bytes > 0) {
                detail += QChar('\n');
                detail += tr("File size: %1").arg(
                    QLocale().formattedDataSize(bytes, 2, QLocale::DataSizeTraditionalFormat));
            }
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Media information"));
    dialog.resize(560, 440);
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QPlainTextEdit(detail, &dialog);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(text, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* copyButton = buttons->addButton(tr("Copy to Clipboard"),
                                          QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, &dialog, [detail] {
        QApplication::clipboard()->setText(detail);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showVideoContextMenu(const QPoint& globalPosition)
{
    QMenu menu(this);
    QAction* playPause = menu.addAction(
        style()->standardIcon(player_->state() == FFmpegPlayer::State::Playing
                                  ? QStyle::SP_MediaPause
                                  : QStyle::SP_MediaPlay),
        tr("Play / Pause"));
    QAction* nextFrame = menu.addAction(style()->standardIcon(QStyle::SP_MediaSeekForward),
                                        tr("Next Video Frame"));
    QAction* stop = menu.addAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop"));
    nextFrame->setEnabled(player_->hasVideo() && player_->state() == FFmpegPlayer::State::Paused);
    menu.addSeparator();
    QAction* back = menu.addAction(tr("Back 5 Seconds"));
    QAction* forward = menu.addAction(tr("Forward 5 Seconds"));
    menu.addSeparator();
    QAction* snapshot = menu.addAction(tr("Take Snapshot"));
    QAction* subtitle = menu.addAction(tr("Load Subtitle..."));
    menu.addSeparator();
    QAction* fullscreen = menu.addAction(fullscreen_
                                             ? tr("Exit Fullscreen")
                                             : tr("Fullscreen"));

    connect(playPause, &QAction::triggered, this, &MainWindow::togglePlayPause);
    connect(nextFrame, &QAction::triggered, this, &MainWindow::stepOneFrame);
    connect(stop, &QAction::triggered, this, &MainWindow::stopPlayback);
    connect(back, &QAction::triggered, this, [this] { player_->seekRelative(-5000); });
    connect(forward, &QAction::triggered, this, [this] { player_->seekRelative(5000); });
    connect(snapshot, &QAction::triggered, this, &MainWindow::takeSnapshot);
    connect(subtitle, &QAction::triggered, this, &MainWindow::openSubtitle);
    connect(fullscreen, &QAction::triggered, this, &MainWindow::toggleFullscreen);
    menu.exec(globalPosition);
}

void MainWindow::exportDiagnostics()
{
    const QString documents = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString suggested = QDir(documents).filePath(
        QStringLiteral("FFmpegQtPlayer-diagnostics-%1.txt")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    QString path = QFileDialog::getSaveFileName(
        this,
        tr("Export Diagnostics"),
        suggested,
        tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".txt");
    }

    QString error;
    if (!player_->exportDiagnostics(path, &error)) {
        showError(tr("Export Diagnostics"), error);
        return;
    }
    statusLabel_->setText(
        tr("Diagnostics exported: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::showAbout()
{
    const QString detail = tr(
        "FFmpeg Qt Player\n\n"
        "A Qt 6 desktop player whose demuxing, decoding, resampling, scaling and clock "
        "control are implemented with FFmpeg.\n\n"
        "FFmpeg libavformat %1.%2\n"
        "FFmpeg libavcodec %3.%4\n"
        "FFmpeg libavutil %5.%6\n"
        "Build: %7")
                               .arg(LIBAVFORMAT_VERSION_MAJOR)
                               .arg(LIBAVFORMAT_VERSION_MINOR)
                               .arg(LIBAVCODEC_VERSION_MAJOR)
                               .arg(LIBAVCODEC_VERSION_MINOR)
                               .arg(LIBAVUTIL_VERSION_MAJOR)
                               .arg(LIBAVUTIL_VERSION_MINOR)
                               .arg(QString::fromLatin1(__DATE__));
    QMessageBox::about(this, tr("About FFmpeg Qt Player"), detail);
}

void MainWindow::showShortcuts()
{
    struct Section
    {
        const char* heading;
        QVector<QPair<QString, QString>> entries;
    };

    const QVector<Section> sections = {
        {QT_TR_NOOP("Playback"),
         {{tr("Play / Pause"), QStringLiteral("Space")},
          {tr("Stop"), QStringLiteral("S")},
          {tr("Next"), QStringLiteral("N")},
          {tr("Previous"), QStringLiteral("P")},
          {tr("Back 5 seconds"), QStringLiteral("Left")},
          {tr("Forward 5 seconds"), QStringLiteral("Right")},
          {tr("Next Video Frame"), QStringLiteral("E")},
          {tr("Previous Video Frame"), QStringLiteral("Ctrl+Left")},
          {tr("Go to Time..."), QStringLiteral("Ctrl+G")},
          {tr("Previous Chapter"), QStringLiteral("Ctrl+PgUp")},
          {tr("Next Chapter"), QStringLiteral("Ctrl+PgDn")},
          {tr("Slower"), QStringLiteral("[")},
          {tr("Faster"), QStringLiteral("]")},
          {tr("Mute"), QStringLiteral("M")},
          {tr("Volume Up"), QStringLiteral("Up")},
          {tr("Volume Down"), QStringLiteral("Down")}}},
        {QT_TR_NOOP("A-B Repeat"),
         {{tr("Set A Point"), QStringLiteral("Ctrl+Shift+A")},
          {tr("Set B Point"), QStringLiteral("Ctrl+Shift+B")},
          {tr("Clear A-B Repeat"), QStringLiteral("Ctrl+Shift+X")}}},
        {QT_TR_NOOP("Subtitle"),
         {{tr("Load Subtitle File..."), QStringLiteral("Ctrl+Shift+T")},
          {tr("Subtitle Style..."), QStringLiteral("Ctrl+Shift+S")},
          {tr("Copy Current Subtitle"), QStringLiteral("Ctrl+Shift+C")},
          {tr("Show Subtitles"), QStringLiteral("Ctrl+H")},
          {tr("Delay -100 ms"), QStringLiteral("Ctrl+Alt+Left")},
          {tr("Delay +100 ms"), QStringLiteral("Ctrl+Alt+Right")}}},
        {QT_TR_NOOP("Playlist"),
         {{tr("Search Playlist"), QStringLiteral("Ctrl+F")},
          {tr("Remove Selected"), QStringLiteral("Delete")}}},
        {QT_TR_NOOP("Video"),
         {{tr("Fullscreen"), QStringLiteral("F / F11")},
          {tr("Take Snapshot"), QStringLiteral("Ctrl+P")},
          {tr("Rotate Clockwise"), QStringLiteral("Ctrl+R")},
          {tr("Rotate Counterclockwise"), QStringLiteral("Ctrl+Shift+R")},
          {tr("Mini Player"), QStringLiteral("Ctrl+Shift+M")},
          {tr("Playlist"), QStringLiteral("Ctrl+J")}}},
        {QT_TR_NOOP("Tools"),
         {{tr("AI Speech and Video Recognition..."), QStringLiteral("Ctrl+Shift+I")}}},
        {QT_TR_NOOP("File"),
         {{tr("Open Files..."), QStringLiteral("Ctrl+O")},
          {tr("Open Network Stream..."), QStringLiteral("Ctrl+L")},
          {tr("Save Playlist..."), QStringLiteral("Ctrl+S")},
          {tr("Exit"), QStringLiteral("Ctrl+Q")}}}};

    QString text;
    for (const Section& section : sections) {
        if (!text.isEmpty()) {
            text += QChar('\n');
        }
        text += QStringLiteral("[%1]\n").arg(UiLanguageSupport::dictionaryText(
            QString::fromLatin1(section.heading)));
        for (const auto& entry : section.entries) {
            text += QStringLiteral("%1\t%2\n").arg(entry.first, entry.second);
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Keyboard Shortcuts"));
    dialog.resize(460, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* view = new QPlainTextEdit(text, &dialog);
    view->setReadOnly(true);
    view->setFont(QFont(QStringLiteral("Consolas"), 10));
    layout->addWidget(view, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::revealCurrentFile()
{
    if (currentIndex_ < 0 || playlist_.at(currentIndex_).isUrl) {
        return;
    }
    const QString path = QDir::toNativeSeparators(playlist_.at(currentIndex_).target);
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,") + path});
}

void MainWindow::addPaths(const QStringList& paths, OpenMode mode)
{
    QVector<PlaylistEntry> entries;
    for (const QString& path : paths) {
        if (!isUrl(path) && isPlaylistFile(path)) {
            if (!loadPlaylist(path, &entries, nullptr)) {
                showError(tr("Open playlist"), tr("No usable entries were found in %1").arg(path));
                return;
            }
            break;
        }
    }

    if (entries.isEmpty()) {
        for (const QString& path : paths) {
            expandPath(path, &entries);
        }
    }
    if (entries.isEmpty()) {
        statusLabel_->setText(tr("No supported media was found."));
        return;
    }

    if (mode == OpenMode::Replace) {
        clearPlaylist();
    }

    const int firstAddedRow = playlist_.size();
    for (const PlaylistEntry& entry : entries) {
        appendPlaylistEntry(entry, false);
    }
    if (currentIndex_ < 0 || mode == OpenMode::Replace) {
        playIndex(firstAddedRow);
    } else {
        statusLabel_->setText(tr("Added %1 item(s) to the playlist.").arg(entries.size()));
    }
    filterPlaylist(playlistFilter_->text());
    updatePlaylistButtons();
}

void MainWindow::expandPath(const QString& path, QVector<PlaylistEntry>* entries) const
{
    if (isUrl(path)) {
        entries->append({path, makeTitle(path), true});
        return;
    }

    const QFileInfo info(path);
    if (info.isDir()) {
        QStringList files;
        QDirIterator iterator(info.absoluteFilePath(), QDir::Files | QDir::Readable,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString file = iterator.next();
            if (isSupportedMedia(file) && !isPlaylistFile(file)) {
                files.append(QFileInfo(file).absoluteFilePath());
            }
        }
        std::sort(files.begin(), files.end(), [](const QString& left, const QString& right) {
            return QString::localeAwareCompare(left, right) < 0;
        });
        for (const QString& file : files) {
            entries->append({file, makeTitle(file), false});
        }
        return;
    }

    if (info.isFile()) {
        entries->append({info.absoluteFilePath(), makeTitle(info.absoluteFilePath()), false});
    }
}

bool MainWindow::loadPlaylist(const QString& path, QVector<PlaylistEntry>* entries,
                              QString* error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }

    QString content;
    decodePlaylistText(file.readAll(), &content);
    const QString baseDirectory = QFileInfo(path).absolutePath();
    const QString suffix = QFileInfo(path).suffix().toLower();

    if (suffix == QStringLiteral("pls")) {
        QHash<int, QString> titles;
        QHash<int, QString> targets;
        static const QRegularExpression titlePattern(
            QStringLiteral("^Title([0-9]+)=(.*)$"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression filePattern(
            QStringLiteral("^File([0-9]+)=(.*)$"),
            QRegularExpression::CaseInsensitiveOption);

        for (const QString& line : content.split(QRegularExpression(QStringLiteral("[\\r\\n]+")))) {
            QRegularExpressionMatch match = titlePattern.match(line.trimmed());
            if (match.hasMatch()) {
                titles.insert(match.captured(1).toInt(), match.captured(2).trimmed());
                continue;
            }
            match = filePattern.match(line.trimmed());
            if (match.hasMatch()) {
                targets.insert(match.captured(1).toInt(), match.captured(2).trimmed());
            }
        }
        QList<int> indexes = targets.keys();
        std::sort(indexes.begin(), indexes.end());
        for (int index : indexes) {
            const QString target = resolvePlaylistTarget(baseDirectory, targets.value(index));
            const bool remote = isUrl(target);
            const QString title = titles.value(index);
            entries->append({target, title.isEmpty() ? makeTitle(target) : title, remote});
        }
    } else {
        QString pendingTitle;
        for (const QString& rawLine : content.split(QRegularExpression(QStringLiteral("[\\r\\n]+")))) {
            const QString line = rawLine.trimmed();
            if (line.isEmpty()) {
                continue;
            }
            if (line.startsWith(QStringLiteral("#EXTINF:"), Qt::CaseInsensitive)) {
                const qsizetype comma = line.indexOf(QChar(','));
                if (comma >= 0) {
                    pendingTitle = line.mid(comma + 1).trimmed();
                }
                continue;
            }
            if (line.startsWith(QChar('#'))) {
                continue;
            }
            const QString target = resolvePlaylistTarget(baseDirectory, line);
            const bool remote = isUrl(target);
            entries->append({target, pendingTitle.isEmpty() ? makeTitle(target) : pendingTitle, remote});
            pendingTitle.clear();
        }
    }

    if (entries->isEmpty()) {
        if (error) {
            *error = tr("The playlist contains no usable entries.");
        }
        return false;
    }
    return true;
}

void MainWindow::appendPlaylistEntry(const PlaylistEntry& entry, bool playWhenAdded)
{
    playlist_.append(entry);
    auto* item = new QListWidgetItem(entry.title, playlistView_);
    item->setToolTip(entry.target);
    item->setData(Qt::UserRole, entry.target);
    item->setData(Qt::UserRole + 1, entry.isUrl);
    item->setFlags(item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    if (playWhenAdded) {
        playIndex(playlist_.size() - 1);
    }
}

void MainWindow::playIndex(int index, qint64 startMs, bool startPaused)
{
    if (index < 0 || index >= playlist_.size()) {
        return;
    }

    rememberCurrentPosition();
    const PlaylistEntry& entry = playlist_.at(index);
    if (startMs <= 0 && !startPaused) {
        const auto resume = resumePositions_.constFind(entry.target);
        if (resume != resumePositions_.constEnd() && resume.value() > 5000) {
            startMs = resume.value();
            pendingResumeMs_ = startMs;
        }
    }

    repeatStartMs_ = -1;
    repeatEndMs_ = -1;
    repeatJumping_ = false;
    currentIndex_ = index;
    playlistView_->setCurrentRow(index);
    subtitleTrack_->clear();
    subtitleText_.clear();
    videoCanvas_->clearFrame();
    setCanvasPlaceholder(QStringLiteral("Opening media..."));
    videoCanvas_->setSubtitleText({});
    progressBar_->setValue(0);
    player_->open(entry.target, startMs, startPaused);
    loadSidecarSubtitle(entry.target);
    updateWindowTitle();
    updatePlaylistButtons();
    updateStatus();
}

void MainWindow::rememberCurrentPosition()
{
    if (currentIndex_ < 0 || currentIndex_ >= playlist_.size()) {
        return;
    }
    const PlaylistEntry& entry = playlist_.at(currentIndex_);
    if (entry.isUrl) {
        return;
    }
    const FFmpegPlayer::State state = player_->state();
    const qint64 position = player_->position();
    const qint64 duration = player_->duration();
    const bool usable = (state == FFmpegPlayer::State::Playing
                         || state == FFmpegPlayer::State::Paused)
        && position > 5000 && (duration <= 0 || position < duration - 3000);
    if (usable) {
        resumePositions_.insert(entry.target, position);
        while (resumePositions_.size() > 100) {
            auto victim = resumePositions_.begin();
            if (victim.key() == entry.target && resumePositions_.size() > 1) {
                ++victim;
            }
            resumePositions_.erase(victim);
        }
    } else {
        resumePositions_.remove(entry.target);
    }
}

void MainWindow::playNext()
{
    const int next = nextPlaylistIndex(true);
    if (next >= 0) {
        playIndex(next);
    } else {
        statusLabel_->setText(tr("End of playlist."));
    }
}

void MainWindow::playPrevious()
{
    const int previous = nextPlaylistIndex(false);
    if (previous >= 0) {
        playIndex(previous);
    } else {
        statusLabel_->setText(tr("Start of playlist."));
    }
}

void MainWindow::removeSelected()
{
    const int index = playlistView_->currentRow();
    if (index < 0 || index >= playlist_.size()) {
        return;
    }
    if (index == currentIndex_) {
        player_->stop();
        currentIndex_ = -1;
        videoCanvas_->clearFrame();
        setCanvasPlaceholder(QStringLiteral("Stopped"));
    } else if (index < currentIndex_) {
        --currentIndex_;
    }
    playlist_.removeAt(index);
    delete playlistView_->takeItem(index);
    if (playlist_.isEmpty()) {
        currentIndex_ = -1;
        videoCanvas_->clearFrame();
        setCanvasPlaceholder(QStringLiteral("Open media to begin"));
        updateWindowTitle();
    } else {
        playlistView_->setCurrentRow(qMin(index, playlist_.size() - 1));
    }
    updatePlaylistButtons();
    updateStatus();
}

void MainWindow::clearPlaylist()
{
    player_->stop();
    playlist_.clear();
    playlistView_->clear();
    currentIndex_ = -1;
    durationMs_ = 0;
    updateDuration(0);
    progressBar_->setValue(0);
    subtitleTrack_->clear();
    subtitleText_.clear();
    videoCanvas_->clearFrame();
    setCanvasPlaceholder(QStringLiteral("Open media to begin"));
    videoCanvas_->setSubtitleText({});
    updateWindowTitle();
    updatePlaylistButtons();
    updateStatus();
}

void MainWindow::updatePlaylistButtons()
{
    const bool filtering = playlistFilter_ && !playlistFilter_->text().trimmed().isEmpty();
    int visibleCount = 0;
    for (int row = 0; row < playlistView_->count(); ++row) {
        QListWidgetItem* item = playlistView_->item(row);
        if (!item->isHidden()) {
            ++visibleCount;
        }
        QFont font = item->font();
        font.setBold(row == currentIndex_);
        item->setFont(font);
        item->setForeground(row == currentIndex_
                                ? QColor(255, 185, 133)
                                : (darkTheme_ ? QColor(220, 223, 229) : QColor(28, 30, 35)));
    }
    playlistCountLabel_->setText(filtering
                                     ? tr("%1 / %2 item(s)").arg(visibleCount).arg(playlist_.size())
                                     : tr("%1 item(s)").arg(playlist_.size()));
}

void MainWindow::filterPlaylist(const QString& text)
{
    if (!playlistView_) {
        return;
    }
    const QString needle = text.trimmed();
    for (int row = 0; row < playlistView_->count(); ++row) {
        QListWidgetItem* item = playlistView_->item(row);
        const bool matches = needle.isEmpty()
            || item->text().contains(needle, Qt::CaseInsensitive)
            || item->data(Qt::UserRole).toString().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!matches);
    }
    updatePlaylistButtons();
}

void MainWindow::focusPlaylistSearch()
{
    if (miniMode_) {
        setMiniMode(false);
        if (miniModeAction_) {
            const QSignalBlocker blocker(miniModeAction_);
            miniModeAction_->setChecked(false);
        }
    }
    if (!playlistPanel_->isVisible()) {
        playlistAction_->setChecked(true);
        playlistPanel_->show();
    }
    playlistFilter_->setFocus(Qt::ShortcutFocusReason);
    playlistFilter_->selectAll();
}

void MainWindow::rebuildPlaylistView()
{
    const QString filterText = playlistFilter_ ? playlistFilter_->text() : QString();
    syncingPlaylistOrder_ = true;
    playlistView_->clear();
    for (const PlaylistEntry& entry : playlist_) {
        auto* item = new QListWidgetItem(entry.title, playlistView_);
        item->setToolTip(entry.target);
        item->setData(Qt::UserRole, entry.target);
        item->setData(Qt::UserRole + 1, entry.isUrl);
        item->setFlags(item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    }
    syncingPlaylistOrder_ = false;
    if (currentIndex_ >= 0 && currentIndex_ < playlist_.size()) {
        playlistView_->setCurrentRow(currentIndex_);
    }
    filterPlaylist(filterText);
    updatePlaylistButtons();
}

void MainWindow::sortPlaylist(PlaylistSort mode)
{
    if (playlist_.size() < 2) {
        statusLabel_->setText(tr("The playlist is empty."));
        return;
    }
    const QString currentTarget = (currentIndex_ >= 0 && currentIndex_ < playlist_.size())
        ? playlist_.at(currentIndex_).target
        : QString();
    std::stable_sort(playlist_.begin(), playlist_.end(),
                     [mode](const PlaylistEntry& left, const PlaylistEntry& right) {
                         if (mode == PlaylistSort::Path) {
                             const int byPath = QString::localeAwareCompare(left.target,
                                                                            right.target);
                             if (byPath != 0) {
                                 return byPath < 0;
                             }
                             return QString::localeAwareCompare(left.title, right.title) < 0;
                         }
                         const int byTitle = QString::localeAwareCompare(left.title,
                                                                         right.title);
                         if (byTitle != 0) {
                             return byTitle < 0;
                         }
                         return QString::localeAwareCompare(left.target, right.target) < 0;
                     });
    currentIndex_ = -1;
    if (!currentTarget.isEmpty()) {
        for (int row = 0; row < playlist_.size(); ++row) {
            if (playlist_.at(row).target == currentTarget) {
                currentIndex_ = row;
                break;
            }
        }
    }
    rebuildPlaylistView();
    statusLabel_->setText(mode == PlaylistSort::Title ? tr("Playlist sorted by title")
                                                      : tr("Playlist sorted by path"));
}

void MainWindow::removeDuplicateEntries()
{
    if (playlist_.size() < 2) {
        statusLabel_->setText(tr("The playlist is empty."));
        return;
    }
    const QString currentTarget = (currentIndex_ >= 0 && currentIndex_ < playlist_.size())
        ? playlist_.at(currentIndex_).target
        : QString();
    QSet<QString> seen;
    QVector<PlaylistEntry> unique;
    unique.reserve(playlist_.size());
    for (const PlaylistEntry& entry : playlist_) {
        const QString key = entry.isUrl ? entry.target : entry.target.toLower();
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        unique.append(entry);
    }
    const int removed = playlist_.size() - unique.size();
    if (removed <= 0) {
        statusLabel_->setText(tr("No duplicate items found"));
        return;
    }
    playlist_ = unique;
    currentIndex_ = -1;
    if (!currentTarget.isEmpty()) {
        for (int row = 0; row < playlist_.size(); ++row) {
            if (playlist_.at(row).target == currentTarget) {
                currentIndex_ = row;
                break;
            }
        }
    }
    rebuildPlaylistView();
    statusLabel_->setText(tr("Removed %1 duplicate(s)").arg(removed));
}

int MainWindow::nextPlaylistIndex(bool forward) const
{
    const int count = playlist_.size();
    if (count == 0) {
        return -1;
    }
    if (currentIndex_ < 0 || currentIndex_ >= count) {
        return 0;
    }
    if (shufflePlaylist_ && count > 1) {
        int index = QRandomGenerator::global()->bounded(count - 1);
        if (index >= currentIndex_) {
            ++index;
        }
        return index;
    }
    const int next = currentIndex_ + (forward ? 1 : -1);
    if (next >= 0 && next < count) {
        return next;
    }
    return loopPlaylist_ ? (forward ? 0 : count - 1) : -1;
}

void MainWindow::togglePlayPause()
{
    if (currentIndex_ < 0) {
        if (playlist_.isEmpty()) {
            openFiles();
        } else {
            playIndex(0);
        }
        return;
    }
    player_->togglePause();
}

void MainWindow::stepOneFrame()
{
    if (!player_->hasVideo()) {
        statusLabel_->setText(tr("The current media has no video stream."));
        return;
    }
    if (player_->state() == FFmpegPlayer::State::Playing) {
        player_->pause();
    }
    if (player_->state() != FFmpegPlayer::State::Paused) {
        statusLabel_->setText(tr("Pause playback before stepping frames."));
        return;
    }
    player_->stepFrame();
    statusLabel_->setText(tr("Stepping one video frame"));
}

void MainWindow::stepPreviousFrame()
{
    if (!player_->hasVideo()) {
        statusLabel_->setText(tr("The current media has no video stream."));
        return;
    }
    if (player_->state() == FFmpegPlayer::State::Playing) {
        player_->pause();
    }
    if (player_->state() != FFmpegPlayer::State::Paused) {
        statusLabel_->setText(tr("Pause playback before stepping frames."));
        return;
    }
    const double frameRate = player_->videoFrameRate();
    qint64 interval = frameRate > 0.1
        ? static_cast<qint64>(std::lround(1000.0 / frameRate))
        : qint64(40);
    interval = std::clamp<qint64>(interval, 10, 100);
    player_->setPosition(qMax<qint64>(0, player_->position() - interval));
    statusLabel_->setText(tr("Stepping back one video frame"));
}

bool MainWindow::repeatActive() const
{
    return repeatStartMs_ >= 0 && repeatEndMs_ > repeatStartMs_;
}

void MainWindow::setRepeatPointA()
{
    if (currentIndex_ < 0) {
        return;
    }
    repeatStartMs_ = player_->position();
    if (repeatEndMs_ <= repeatStartMs_) {
        repeatEndMs_ = -1;
    }
    repeatJumping_ = false;
    statusLabel_->setText(tr("A point set at %1").arg(formatTime(repeatStartMs_)));
    showCanvasOsd(tr("A: %1").arg(formatTime(repeatStartMs_)));
}

void MainWindow::setRepeatPointB()
{
    if (currentIndex_ < 0) {
        return;
    }
    if (repeatStartMs_ < 0) {
        statusLabel_->setText(tr("Set the A point first."));
        showCanvasOsd(tr("Set the A point first."));
        return;
    }
    const qint64 position = player_->position();
    if (position <= repeatStartMs_ + 200) {
        statusLabel_->setText(tr("The B point must be after the A point."));
        showCanvasOsd(tr("The B point must be after the A point."));
        return;
    }
    repeatEndMs_ = position;
    repeatJumping_ = false;
    statusLabel_->setText(tr("B point set at %1").arg(formatTime(repeatEndMs_)));
    showCanvasOsd(tr("B: %1").arg(formatTime(repeatEndMs_)));
    updateStatus();
}

void MainWindow::clearRepeat()
{
    repeatStartMs_ = -1;
    repeatEndMs_ = -1;
    repeatJumping_ = false;
    statusLabel_->setText(tr("A-B repeat cleared"));
    showCanvasOsd(tr("A-B repeat cleared"));
    updateStatus();
}

int MainWindow::currentChapterIndex() const
{
    const QVector<FFmpegPlayer::ChapterInfo> chapters = player_->chapters();
    const qint64 position = player_->position();
    int active = -1;
    for (int index = 0; index < chapters.size(); ++index) {
        if (chapters.at(index).startMs > position + 250) {
            break;
        }
        active = index;
    }
    return active;
}

void MainWindow::playNextChapter()
{
    const QVector<FFmpegPlayer::ChapterInfo> chapters = player_->chapters();
    if (chapters.isEmpty()) {
        statusLabel_->setText(tr("No chapters"));
        showCanvasOsd(tr("No chapters"));
        return;
    }

    const int current = currentChapterIndex();
    int target = current + 1;
    if (current < 0) {
        target = 0;
    }
    if (target >= chapters.size()) {
        statusLabel_->setText(tr("Last chapter"));
        showCanvasOsd(tr("Last chapter"));
        return;
    }

    const FFmpegPlayer::ChapterInfo& chapter = chapters.at(target);
    const QString label = chapter.title.isEmpty()
        ? tr("Chapter %1").arg(target + 1)
        : tr("Chapter %1  %2").arg(target + 1).arg(chapter.title);
    player_->setPosition(chapter.startMs);
    statusLabel_->setText(label);
    showCanvasOsd(label);
}

void MainWindow::playPreviousChapter()
{
    const QVector<FFmpegPlayer::ChapterInfo> chapters = player_->chapters();
    if (chapters.isEmpty()) {
        statusLabel_->setText(tr("No chapters"));
        showCanvasOsd(tr("No chapters"));
        return;
    }

    int target = currentChapterIndex();
    if (target < 0) {
        target = 0;
    } else if (player_->position() - chapters.at(target).startMs <= 2000) {
        target = qMax(0, target - 1);
    }

    const FFmpegPlayer::ChapterInfo& chapter = chapters.at(target);
    const QString label = chapter.title.isEmpty()
        ? tr("Chapter %1").arg(target + 1)
        : tr("Chapter %1  %2").arg(target + 1).arg(chapter.title);
    player_->setPosition(chapter.startMs);
    statusLabel_->setText(label);
    showCanvasOsd(label);
}

void MainWindow::goToTime()
{
    if (currentIndex_ < 0) {
        return;
    }
    bool accepted = false;
    const QString input = QInputDialog::getText(
        this,
        tr("Go to Time"),
        tr("Enter a time (MM:SS or seconds):"),
        QLineEdit::Normal,
        formatTime(player_->position()),
        &accepted);
    if (!accepted || input.trimmed().isEmpty()) {
        return;
    }
    qint64 milliseconds = 0;
    if (!parseTimeText(input, &milliseconds)) {
        statusLabel_->setText(tr("Invalid time."));
        showCanvasOsd(tr("Invalid time."));
        return;
    }
    player_->setPosition(milliseconds);
    statusLabel_->setText(tr("Jumped to %1").arg(formatTime(milliseconds)));
}

void MainWindow::copyCurrentSubtitle()
{
    const QString text = subtitleText_.trimmed();
    if (text.isEmpty()) {
        statusLabel_->setText(tr("No subtitle is showing right now."));
        showCanvasOsd(tr("No subtitle is showing right now."));
        return;
    }
    QApplication::clipboard()->setText(text);
    statusLabel_->setText(tr("Subtitle copied to the clipboard"));
    showCanvasOsd(tr("Subtitle copied"));
}

void MainWindow::applySubtitleStyle()
{
    if (videoCanvas_) {
        videoCanvas_->setSubtitleStyle(subtitleStyle_);
    }
}

void MainWindow::showSubtitleStyleDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Subtitle Style"));
    auto* layout = new QFormLayout(&dialog);

    auto* fontCombo = new QFontComboBox(&dialog);
    QFont font = fontCombo->currentFont();
    if (!subtitleStyle_.fontFamily.isEmpty()) {
        font.setFamily(subtitleStyle_.fontFamily);
    }
    fontCombo->setCurrentFont(font);
    layout->addRow(tr("Font"), fontCombo);

    QColor textColor = subtitleStyle_.textColor;
    auto* textColorButton = new QPushButton(&dialog);
    styleColorButton(textColorButton, textColor);
    layout->addRow(tr("Text Color"), textColorButton);

    QColor outlineColor = subtitleStyle_.outlineColor;
    auto* outlineColorButton = new QPushButton(&dialog);
    styleColorButton(outlineColorButton, outlineColor);
    layout->addRow(tr("Outline Color"), outlineColorButton);

    auto* outlineSpin = new QSpinBox(&dialog);
    outlineSpin->setRange(0, 8);
    outlineSpin->setValue(std::clamp(subtitleStyle_.outlineWidth, 0, 8));
    layout->addRow(tr("Outline Width"), outlineSpin);

    QColor backgroundColor = subtitleStyle_.backgroundColor;
    auto* backgroundButton = new QPushButton(&dialog);
    styleColorButton(backgroundButton, backgroundColor);
    layout->addRow(tr("Background Color"), backgroundButton);

    auto* opacitySlider = new QSlider(Qt::Horizontal, &dialog);
    opacitySlider->setRange(0, 100);
    opacitySlider->setValue(std::clamp(subtitleStyle_.backgroundOpacity * 100 / 255, 0, 100));
    layout->addRow(tr("Background Opacity"), opacitySlider);

    const auto collect = [&]() {
        VideoCanvas::SubtitleStyle style;
        style.fontFamily = fontCombo->currentFont().family();
        style.textColor = textColor;
        style.outlineColor = outlineColor;
        style.outlineWidth = outlineSpin->value();
        style.backgroundColor = backgroundColor;
        style.backgroundOpacity = opacitySlider->value() * 255 / 100;
        return style;
    };
    const auto updatePreview = [this, &collect] {
        videoCanvas_->setSubtitleStyle(collect());
    };

    connect(textColorButton, &QPushButton::clicked, &dialog, [&] {
        const QColor chosen = QColorDialog::getColor(textColor, &dialog, tr("Text Color"));
        if (chosen.isValid()) {
            textColor = chosen;
            styleColorButton(textColorButton, textColor);
            updatePreview();
        }
    });
    connect(outlineColorButton, &QPushButton::clicked, &dialog, [&] {
        const QColor chosen = QColorDialog::getColor(outlineColor, &dialog, tr("Outline Color"));
        if (chosen.isValid()) {
            outlineColor = chosen;
            styleColorButton(outlineColorButton, outlineColor);
            updatePreview();
        }
    });
    connect(backgroundButton, &QPushButton::clicked, &dialog, [&] {
        const QColor chosen = QColorDialog::getColor(backgroundColor, &dialog,
                                                     tr("Background Color"));
        if (chosen.isValid()) {
            backgroundColor = chosen;
            styleColorButton(backgroundButton, backgroundColor);
            updatePreview();
        }
    });
    connect(fontCombo, &QFontComboBox::currentFontChanged, &dialog,
            [&updatePreview](const QFont&) { updatePreview(); });
    connect(outlineSpin, &QSpinBox::valueChanged, &dialog,
            [&updatePreview](int) { updatePreview(); });
    connect(opacitySlider, &QSlider::valueChanged, &dialog,
            [&updatePreview](int) { updatePreview(); });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                             | QDialogButtonBox::RestoreDefaults,
                                         &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog,
            [&] {
                const VideoCanvas::SubtitleStyle defaults;
                fontCombo->setCurrentFont(QFont(QStringLiteral("Segoe UI")));
                textColor = defaults.textColor;
                outlineColor = defaults.outlineColor;
                backgroundColor = defaults.backgroundColor;
                outlineSpin->setValue(defaults.outlineWidth);
                opacitySlider->setValue(std::clamp(defaults.backgroundOpacity * 100 / 255, 0, 100));
                styleColorButton(textColorButton, textColor);
                styleColorButton(outlineColorButton, outlineColor);
                styleColorButton(backgroundButton, backgroundColor);
                updatePreview();
            });

    if (dialog.exec() == QDialog::Accepted) {
        subtitleStyle_ = collect();
        applySubtitleStyle();
        saveSettings();
        statusLabel_->setText(tr("Subtitle style updated"));
        showCanvasOsd(tr("Subtitle style updated"));
    } else {
        applySubtitleStyle();
    }
}

void MainWindow::stopPlayback()
{
    player_->stop();
    videoCanvas_->clearFrame();
    setCanvasPlaceholder(QStringLiteral("Stopped"));
    progressBar_->setValue(0);
    updateState(FFmpegPlayer::State::Stopped);
}

void MainWindow::cycleRate()
{
    const int next = (speedCombo_->currentIndex() + 1) % speedCombo_->count();
    speedCombo_->setCurrentIndex(next);
}

void MainWindow::adjustVolume(int delta)
{
    volumeSlider_->setValue(std::clamp(volumeSlider_->value() + delta, 0, 125));
}

void MainWindow::toggleMute()
{
    if (volumeSlider_->value() == 0 && !muted_) {
        volumeSlider_->setValue(qMax(1, lastVolume_));
        return;
    }
    muted_ = !muted_;
    player_->setMuted(muted_);
    muteButton_->setIcon(style()->standardIcon(
        muted_ ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    if (!muted_ && volumeSlider_->value() == 0) {
        volumeSlider_->setValue(qMax(1, lastVolume_));
    }
    showCanvasOsd(muted_ ? tr("Muted") : tr("Volume %1%").arg(volumeSlider_->value()));
    updateStatus();
}

void MainWindow::updateUi()
{
    if (closing_) {
        return;
    }
    const qint64 duration = player_->duration();
    if (duration != durationMs_) {
        updateDuration(duration);
    }
    const qint64 position = player_->position();
    if (!sliderDragging_ && !seeking_) {
        seekSlider_->setValue(static_cast<int>(qMin<qint64>(position, seekSlider_->maximum())));
        timeLabel_->setText(
            QStringLiteral("%1 / %2").arg(formatTime(position), formatTime(durationMs_)));
    }
    if (durationMs_ > 0) {
        const qint64 scaled = qBound<qint64>(qint64(0), position * 1000 / durationMs_, qint64(1000));
        progressBar_->setValue(static_cast<int>(scaled));
    } else {
        progressBar_->setValue(0);
    }

    if (repeatActive()) {
        if (position < repeatEndMs_ - 400) {
            repeatJumping_ = false;
        }
        if (!repeatJumping_ && position >= repeatEndMs_
            && player_->state() == FFmpegPlayer::State::Playing) {
            repeatJumping_ = true;
            player_->setPosition(repeatStartMs_);
        }
    } else {
        repeatJumping_ = false;
    }
    if (repeatPointAAction_) {
        const FFmpegPlayer::State state = player_->state();
        const bool mediaActive = currentIndex_ >= 0 && state != FFmpegPlayer::State::Idle
            && state != FFmpegPlayer::State::Stopped;
        repeatPointAAction_->setEnabled(mediaActive);
        repeatPointBAction_->setEnabled(mediaActive);
        repeatClearAction_->setEnabled(repeatActive());
    }

    updateSubtitle(position);
}

void MainWindow::updateDuration(qint64 durationMs)
{
    durationMs_ = qMax<qint64>(0, durationMs);
    const qint64 bounded = qMin<qint64>(durationMs_, std::numeric_limits<int>::max());
    seekSlider_->setRange(0, static_cast<int>(bounded));
    timeLabel_->setText(QStringLiteral("%1 / %2")
                            .arg(formatTime(player_->position()), formatTime(durationMs_)));
}

void MainWindow::updateState(FFmpegPlayer::State state)
{
    switch (state) {
    case FFmpegPlayer::State::Opening:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        aiService_->flushAudio();
        statusLabel_->setText(tr("Opening media..."));
        rebuildChapterMenu();
        break;
    case FFmpegPlayer::State::Buffering:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        statusLabel_->setText(tr("Buffering..."));
        break;
    case FFmpegPlayer::State::Stalled:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        statusLabel_->setText(tr("Network stalled, retrying..."));
        break;
    case FFmpegPlayer::State::Reconnecting:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        statusLabel_->setText(tr("Reconnecting..."));
        break;
    case FFmpegPlayer::State::Playing:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
        updateStatus();
        break;
    case FFmpegPlayer::State::Paused:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        aiService_->flushAudio();
        statusLabel_->setText(tr("Paused"));
        break;
    case FFmpegPlayer::State::Stopped:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        aiService_->flushAudio();
        statusLabel_->setText(tr("Stopped"));
        break;
    case FFmpegPlayer::State::Ended:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        aiService_->flushAudio();
        statusLabel_->setText(tr("Playback finished"));
        break;
    case FFmpegPlayer::State::Error:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        statusLabel_->setText(player_->lastError().isEmpty()
                                  ? tr("Playback error")
                                  : player_->lastError());
        break;
    case FFmpegPlayer::State::Idle:
    default:
        playButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        break;
    }
    updatePlaylistButtons();
}

void MainWindow::updateSubtitle(qint64 positionMs)
{
    if (!embeddedSubtitlesVisible_) {
        if (!subtitleText_.isEmpty()) {
            subtitleText_.clear();
            videoCanvas_->setSubtitleText({});
        }
        return;
    }

    const qint64 shifted = positionMs - subtitleDelayMs_;
    QString text;
    if (subtitleTrack_ && !subtitleTrack_->isEmpty()) {
        text = subtitleTrack_->textAt(shifted);
    } else {
        text = player_->embeddedSubtitleTextAt(shifted);
    }
    if (text != subtitleText_) {
        subtitleText_ = text;
        videoCanvas_->setSubtitleText(text);
    }
}

void MainWindow::updateStatus()
{
    if (!statusLabel_) {
        return;
    }
    if (player_->state() == FFmpegPlayer::State::Error) {
        return;
    }
    QStringList flags;
    flags.append(loopPlaylist_ ? tr("loop on") : tr("loop off"));
    flags.append(shufflePlaylist_ ? tr("shuffle on") : tr("shuffle off"));
    flags.append(topmost_ ? tr("topmost") : tr("normal window"));
    if (miniMode_) {
        flags.append(tr("mini player"));
    }
    flags.append(QStringLiteral("%1x").arg(player_->playbackRate(), 0, 'g', 3));
    if (repeatActive()) {
        flags.append(tr("A-B %1-%2").arg(formatTime(repeatStartMs_), formatTime(repeatEndMs_)));
    }
    const int chapter = currentChapterIndex();
    if (chapter >= 0) {
        flags.append(tr("chapter %1").arg(chapter + 1));
    }
    if (sleepRemainingMs_ > 0) {
        flags.append(tr("sleep in %1").arg(formatTime(sleepRemainingMs_)));
    }
    const QString title = currentMediaTitle();
    if (!title.isEmpty()) {
        statusLabel_->setText(QStringLiteral("%1  |  %2").arg(title, flags.join(QStringLiteral(" | "))));
    }
}

void MainWindow::updateWindowTitle()
{
    const QString title = currentMediaTitle();
    mediaTitle_ = title;
    setWindowTitle(title.isEmpty()
                       ? tr("FFmpeg Qt Player")
                       : tr("%1 - FFmpeg Qt Player").arg(title));
    if (mediaLabel_) {
        mediaLabel_->setToolTip(title);
    }
    updateCompactChrome();
    // Re-elide once the layout has given the label its final width.
    QTimer::singleShot(0, this, &MainWindow::updateCompactChrome);
}

void MainWindow::showError(const QString& title, const QString& detail)
{
    statusLabel_->setText(detail);
    QMessageBox::warning(this, title, detail);
}

void MainWindow::loadSidecarSubtitle(const QString& mediaTarget)
{
    if (isUrl(mediaTarget)) {
        return;
    }
    const QFileInfo mediaInfo(mediaTarget);
    for (const QString& extension : {QStringLiteral("srt"), QStringLiteral("ass"),
                                     QStringLiteral("ssa"), QStringLiteral("vtt")}) {
        const QString candidate = mediaInfo.dir().filePath(
            mediaInfo.completeBaseName() + QChar('.') + extension);
        if (QFileInfo::exists(candidate)) {
            setSubtitle(candidate);
            return;
        }
    }
}

void MainWindow::setSubtitle(const QString& path)
{
    QString error;
    if (!subtitleTrack_->load(path, &error)) {
        showError(tr("Subtitle"), error);
        return;
    }
    subtitleText_.clear();
    videoCanvas_->setSubtitleText({});
    embeddedSubtitlesVisible_ = true;
    if (showSubtitlesAction_) {
        showSubtitlesAction_->setChecked(true);
    }
    updateSubtitle(player_->position());
    statusLabel_->setText(tr("Subtitle loaded: %1").arg(QFileInfo(path).fileName()));
}

QString MainWindow::currentMediaTitle() const
{
    if (currentIndex_ < 0 || currentIndex_ >= playlist_.size()) {
        return {};
    }
    return playlist_.at(currentIndex_).title;
}

QString MainWindow::makeTitle(const QString& target)
{
    if (isUrl(target)) {
        const QUrl url(target);
        const QString fileName = QFileInfo(url.path()).fileName();
        if (!fileName.isEmpty()) {
            return fileName;
        }
        if (!url.host().isEmpty()) {
            return url.host();
        }
        return target;
    }
    return QFileInfo(target).fileName();
}

QString MainWindow::formatTime(qint64 milliseconds)
{
    if (milliseconds < 0) {
        milliseconds = 0;
    }
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

bool MainWindow::isUrl(const QString& target)
{
    static const QRegularExpression windowsPath(
        QStringLiteral("^[A-Za-z]:[\\\\/]"));
    if (windowsPath.match(target).hasMatch()) {
        return false;
    }
    const QUrl url(target);
    return url.isValid() && !url.scheme().isEmpty() && url.scheme().size() > 1;
}

bool MainWindow::isPlaylistFile(const QString& path)
{
    if (isUrl(path)) {
        return false;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("m3u") || suffix == QStringLiteral("m3u8")
        || suffix == QStringLiteral("pls");
}

bool MainWindow::isSupportedMedia(const QString& path)
{
    static const QSet<QString> extensions = {
        QStringLiteral("mp4"),  QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("mov"),  QStringLiteral("flv"), QStringLiteral("ts"),
        QStringLiteral("m2ts"), QStringLiteral("m4v"), QStringLiteral("webm"),
        QStringLiteral("wmv"),  QStringLiteral("mpg"), QStringLiteral("mpeg"),
        QStringLiteral("mp3"),  QStringLiteral("aac"), QStringLiteral("wav"),
        QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("opus"),
        QStringLiteral("m4a"),  QStringLiteral("wma"), QStringLiteral("ape")
    };
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

QStringList MainWindow::mediaNameFilters()
{
    return {
        tr("Media files (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.m2ts *.m4v *.webm *.wmv "
           "*.mpg *.mpeg *.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a *.wma *.ape)"),
        tr("Video files (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.m2ts *.m4v *.webm *.wmv "
           "*.mpg *.mpeg)"),
        tr("Audio files (*.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a *.wma *.ape)"),
        tr("All files (*.*)")
    };
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        } else if (url.isValid()) {
            paths.append(url.toString());
        }
    }
    if (!paths.isEmpty()) {
        addPaths(paths, playlist_.isEmpty() ? OpenMode::Replace : OpenMode::Append);
        event->acceptProposedAction();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    closing_ = true;
    updateTimer_->stop();
    saveSettings();
    player_->stop();
    event->accept();
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::recordUiTexts()
{
    uiTexts_.clear();
    const QSet<QWidget*> dynamicWidgets = {
        timeLabel_, volumeLabel_, mediaLabel_, playlistCountLabel_, statusLabel_
    };

    const auto add = [this](QObject* object, int field, const QString& english) {
        if (!object || english.isEmpty() || object->property("noTranslate").toBool()) {
            return;
        }
        uiTexts_.append({object, field, english});
    };

    for (QAction* action : findChildren<QAction*>()) {
        add(action, 0, action->text());
        add(action, 1, action->toolTip());
        add(action, 2, action->statusTip());
    }

    for (QWidget* widget : findChildren<QWidget*>()) {
        if (dynamicWidgets.contains(widget)) {
            continue;
        }
        if (auto* label = qobject_cast<QLabel*>(widget)) {
            add(widget, 0, label->text());
        } else if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
            add(widget, 0, button->text());
        } else if (auto* group = qobject_cast<QGroupBox*>(widget)) {
            add(widget, 0, group->title());
        }
        add(widget, 1, widget->toolTip());
    }
}

void MainWindow::retranslateUi()
{
    for (const UiText& entry : uiTexts_) {
        QObject* object = entry.object.data();
        if (!object) {
            continue;
        }
        const QString translated = UiLanguageSupport::dictionaryText(entry.english);
        if (entry.field == 0) {
            if (auto* action = qobject_cast<QAction*>(object)) {
                action->setText(translated);
            } else if (auto* label = qobject_cast<QLabel*>(object)) {
                label->setText(translated);
            } else if (auto* button = qobject_cast<QAbstractButton*>(object)) {
                button->setText(translated);
            } else if (auto* group = qobject_cast<QGroupBox*>(object)) {
                group->setTitle(translated);
            }
        } else if (entry.field == 1) {
            if (auto* action = qobject_cast<QAction*>(object)) {
                action->setToolTip(translated);
            } else if (auto* widget = qobject_cast<QWidget*>(object)) {
                widget->setToolTip(translated);
            }
        } else if (auto* action = qobject_cast<QAction*>(object)) {
            action->setStatusTip(translated);
        }
    }

    if (!placeholderEnglish_.isEmpty()) {
        videoCanvas_->setPlaceholderText(
            UiLanguageSupport::dictionaryText(placeholderEnglish_));
    }
    if (playlistFilter_) {
        playlistFilter_->setPlaceholderText(
            UiLanguageSupport::dictionaryText(QStringLiteral("Search playlist...")));
    }
    updateWindowTitle();
    updateDuration(durationMs_);
    updatePlaylistButtons();
    volumeLabel_->setText(QStringLiteral("%1%").arg(volumeSlider_->value()));
    rebuildTrackMenus();
    rebuildChapterMenu();
    rebuildRecentMenu();
    if (aiRecognitionAction_) {
        aiRecognitionAction_->setText(
            aiService_->isRunning() ? tr("AI Recognition (Running)...")
                                    : tr("AI Speech and Video Recognition..."));
    }
    if (aiContentDialog_) {
        aiContentDialog_->close();
        aiContentDialog_->deleteLater();
        aiContentDialog_ = nullptr;
    }

    const FFmpegPlayer::State state = player_->state();
    if (state == FFmpegPlayer::State::Idle) {
        statusLabel_->setText(tr("Ready"));
    } else if (state == FFmpegPlayer::State::Playing) {
        updateStatus();
    } else {
        updateState(state);
    }
}

void MainWindow::switchLanguage(UiLanguage language)
{
    if (language == uiLanguage_) {
        return;
    }
    uiLanguage_ = language;
    englishLanguageAction_->setChecked(language == UiLanguage::English);
    chineseLanguageAction_->setChecked(language == UiLanguage::Chinese);
    UiLanguageSupport::setCurrent(language);
    retranslateUi();
    saveSettings();
}

void MainWindow::setCanvasPlaceholder(const QString& english)
{
    placeholderEnglish_ = english;
    if (videoCanvas_) {
        videoCanvas_->setPlaceholderText(UiLanguageSupport::dictionaryText(english));
    }
}

