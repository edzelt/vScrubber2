#include "mainwindow.h"
#include "videowidget.h"
#include "playbackcontroller.h"
#include "inputcontroller.h"
#include "transportpanel.h"
#include <QMenuBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QApplication>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("vScrubber2");
    setMinimumSize(640, 360);
    resize(1280, 720);

    // ── Видео-виджет ─────────────────────────────────────────────────────────
    m_videoWidget = new VideoWidget(this);
    setCentralWidget(m_videoWidget);

    // ── Контроллер воспроизведения ────────────────────────────────────────────
    m_playback = new PlaybackController(this);

    // ── Контроллер ввода ──────────────────────────────────────────────────────
    m_input = new InputController(m_playback, this);
    m_input->setVideoWidget(m_videoWidget);

    // Передаём InputController виджету для обработки событий мыши
    m_videoWidget->setInputController(m_input);

    setupMenu();

    // ── Связи: VideoWidget → MainWindow ──────────────────────────────────────
    connect(m_videoWidget, &VideoWidget::fileLoaded,
            this, &MainWindow::onFileLoaded);
    connect(m_videoWidget, &VideoWidget::positionChanged,
            this, &MainWindow::onPositionChanged);
    connect(m_videoWidget, &VideoWidget::endOfFileReached,
            this, &MainWindow::onEndOfFile);
    connect(m_videoWidget, &VideoWidget::seekCompleted,
            m_playback, &PlaybackController::resetClock);

    // ── Связи: PlaybackController → VideoWidget ──────────────────────────────
    // Шаг кадров: контроллер говорит сколько кадров показать
    connect(m_playback, &PlaybackController::stepRequested,
            m_videoWidget, &VideoWidget::stepFrame);

    // Непрерывное воспроизведение: decodeNext без seek+flush
    connect(m_playback, &PlaybackController::continuousPlayChanged,
            m_videoWidget, &VideoWidget::setContinuousPlay);

    // ── Связи: PlaybackController → MainWindow (обновление UI) ───────────────
    connect(m_playback, &PlaybackController::speedChanged,
            this, &MainWindow::onSpeedChanged);
    connect(m_playback, &PlaybackController::stateChanged,
            this, [this](PlaybackController::State s) {
                onPlaybackStateChanged(static_cast<int>(s));
            });

    // ── Связи: InputController → VideoWidget (навигация по keyframe) ──────────
    connect(m_input, &InputController::seekKeyframe,
            this, &MainWindow::onSeekKeyframe);

    // ── Связи: InputController → VideoWidget (zoom/pan) ──────────────────────
    connect(m_input, &InputController::zoomRequested,
            m_videoWidget, &VideoWidget::applyZoom);
    connect(m_input, &InputController::panRequested,
            m_videoWidget, &VideoWidget::applyPan);

    // ── Связи: PlaybackController → OSD (скорость) ───────────────────────────
    connect(m_playback, &PlaybackController::speedChanged,
            m_videoWidget, &VideoWidget::setOsdSpeed);

    // ── Связи: InputController → OSD (режим колеса) ──────────────────────────
    connect(m_input, &InputController::wheelModeChanged,
            this, [this](InputController::WheelMode mode) {
                m_videoWidget->setOsdWheelMode(static_cast<int>(mode));
            });

    // ── Связи: TransportPanel → MainWindow ───────────────────────────────────
    auto* tp = m_videoWidget->transportPanel();
    if (tp) {
        connect(tp, &TransportPanel::seekRequested,
                this, [this](double pts) {
                    m_videoWidget->seekTo(pts);
                });
        connect(tp, &TransportPanel::fileSelected,
                this, [this](const QString& path) {
                    openFileKeepState(path);
                });
    }
}

MainWindow::~MainWindow() = default;

// ── Меню ─────────────────────────────────────────────────────────────────────

void MainWindow::setupMenu()
{
    auto* fileMenu = menuBar()->addMenu("Файл");

    auto* openAct = fileMenu->addAction("Открыть...");
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::onOpenFile);

    fileMenu->addSeparator();

    auto* exitAct = fileMenu->addAction("Выход");
    exitAct->setShortcut(QKeySequence::Quit);
    connect(exitAct, &QAction::triggered, qApp, &QApplication::quit);
}

// ── Открытие файла ──────────────────────────────────────────────────────────

void MainWindow::onOpenFile()
{
    QString path = QFileDialog::getOpenFileName(
        this,
        "Открыть видеофайл",
        {},
        "Видео (*.mp4 *.mkv *.mov *.avi *.hevc *.h265 *.h264 *.ts *.mts);;"
        "Все файлы (*.*)"
        );
    if (!path.isEmpty())
        openFile(path);
}

void MainWindow::openFile(const QString& path)
{
    // Полный сброс при ручном открытии
    m_playback->stop();
    m_playback->setSpeed(1.0);
    m_input->reset();
    m_videoWidget->resetZoom();
    m_videoWidget->setOsdSpeed(1.0);
    m_videoWidget->setOsdWheelMode(0);

    openFileKeepState(path);
}

void MainWindow::openFileKeepState(const QString& path)
{
    // Открытие файла с сохранением текущего режима воспроизведения
    double savedSpeed = m_playback->speed();
    bool wasPlaying = m_playback->isPlaying();

    // Останавливаем воспроизведение на время загрузки
    if (wasPlaying)
        m_playback->pause();
    m_videoWidget->setContinuousPlay(false);

    if (m_videoWidget->transportPanel())
        m_videoWidget->transportPanel()->setCurrentFile(path);

    m_videoWidget->openFile(path);

    // Сохраняем скорость и состояние — восстановим в onFileLoaded
    m_pendingSpeed = savedSpeed;
    m_pendingPlay  = wasPlaying;

    QFileInfo fi(path);
    setWindowTitle(QString("vScrubber2 — %1").arg(fi.fileName()));
}

// ── Клавиатура ───────────────────────────────────────────────────────────────
// Делегируем в InputController

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    m_input->handleKeyPress(event->key());

    // Не вызываем base — InputController обрабатывает Space/Esc
    // Стрелки обрабатываются в VideoWidget::keyPressEvent
}

// ── Callbacks ────────────────────────────────────────────────────────────────

void MainWindow::onFileLoaded(bool success)
{
    if (!success) {
        setWindowTitle("vScrubber2 — ошибка открытия файла");
        m_pendingPlay = false;
        return;
    }

    m_playback->setFps(m_videoWidget->fps());
    updateTitle();

    // Восстанавливаем режим воспроизведения (при переходе между файлами)
    if (m_pendingPlay) {
        m_playback->setSpeed(m_pendingSpeed);
        m_videoWidget->setOsdSpeed(m_pendingSpeed);
        m_playback->play();
        m_pendingPlay = false;
    }
}

void MainWindow::onPositionChanged(double pts)
{
    Q_UNUSED(pts)
    updateTitle();
}

void MainWindow::onSpeedChanged(double speed)
{
    Q_UNUSED(speed)
    updateTitle();
}

void MainWindow::onPlaybackStateChanged(int state)
{
    Q_UNUSED(state)
    updateTitle();
}

// ── Перемотка по ключевым кадрам ─────────────────────────────────────────────
void MainWindow::onSeekKeyframe(int direction)
{
    if (!m_videoWidget->isFileLoaded()) return;

    double currentPts = m_videoWidget->currentPts();

    // Находим текущий GOP
    int curGop = m_videoWidget->findGopByPts(currentPts);
    if (curGop < 0) return;

    int targetGop = curGop + direction;
    int gopCount = m_videoWidget->gopCount();

    if (targetGop < 0) targetGop = 0;
    if (targetGop >= gopCount) targetGop = gopCount - 1;

    // Seek на начало целевого GOP
    double gopPts = m_videoWidget->gopStartPts(targetGop);
    m_videoWidget->seekTo(gopPts);
}

// ── Обновление заголовка окна ────────────────────────────────────────────────
void MainWindow::updateTitle()
{
    if (!m_videoWidget->isFileLoaded()) return;

    QString fileName;
    auto* tp = m_videoWidget->transportPanel();
    if (tp) {
        QFileInfo fi(tp->currentFilePath());
        fileName = fi.fileName();
    }

    setWindowTitle(QString("vScrubber2 — %1").arg(fileName));
}

// ── Обработка конца/начала файла при воспроизведении ──────────────────────────
void MainWindow::onEndOfFile()
{
    auto* tp = m_videoWidget->transportPanel();
    double spd = m_playback->speed();
    bool loopFile = tp && tp->isLoopFile();
    bool loopDir  = tp && tp->isLoopDir();

    if (loopFile && !loopDir) {
        // Только Loop: зацикливаем текущий файл
        double targetPts = (spd > 0.0) ? 0.0 : m_videoWidget->maxPts();
        m_videoWidget->seekTo(targetPts);
        m_videoWidget->forceSyncDecoder(targetPts);
    } else if (loopDir || loopFile) {
        // Dir (без Loop): проиграть до конца каталога, остановиться
        // Dir + Loop: зациклить весь каталог
        bool hasNext = true;
        if (spd > 0.0) {
            hasNext = tp->tryNextFile();
        } else {
            hasNext = tp->tryPrevFile();
        }
        // Если файлов больше нет и Loop+Dir — зацикливаем каталог
        if (!hasNext && loopFile && loopDir) {
            if (spd > 0.0)
                tp->goToFirstFile();
            else
                tp->goToLastFile();
        } else if (!hasNext) {
            m_playback->notifyEndOfFile();
        }
    } else {
        // Ничего не нажато — стоп
        m_playback->notifyEndOfFile();
    }
}