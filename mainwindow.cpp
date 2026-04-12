#include "mainwindow.h"
#include "videowidget.h"
#include "playbackcontroller.h"
#include "inputcontroller.h"
#include <QMenuBar>
#include <QFileDialog>
#include <QKeyEvent>
#include <QApplication>
#include <QDebug>
#include <cmath>

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
    // Останавливаем воспроизведение
    m_playback->stop();

    m_videoWidget->openFile(path);
    setWindowTitle(QString("vScrubber2 — %1").arg(path));
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
        return;
    }

    // Передаём FPS в контроллер воспроизведения
    m_playback->setFps(m_videoWidget->fps());
    updateTitle();
}

void MainWindow::onPositionChanged(double pts)
{
    Q_UNUSED(pts)
    updateTitle();

    // Проверяем конец файла
    if (m_playback->isPlaying()) {
        double dur = m_videoWidget->duration();
        double cur = m_videoWidget->currentPts();
        double spd = m_playback->speed();

        // Конец файла при движении вперёд
        if (spd > 0.0 && cur >= dur - 0.01) {
            m_playback->notifyEndOfFile();
        }
        // Начало файла при реверсе
        if (spd < 0.0 && cur <= 0.01) {
            m_playback->notifyEndOfFile();
        }
    }
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
    double fps = m_videoWidget->fps();

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

    QString state;
    switch (m_playback->state()) {
    case PlaybackController::Playing: state = "▶"; break;
    case PlaybackController::Paused:  state = "⏸"; break;
    case PlaybackController::Stopped: state = "⏹"; break;
    }

    double pts = m_videoWidget->currentPts();
    double dur = m_videoWidget->duration();
    double speed = m_playback->speed();

    int curMin   = static_cast<int>(pts) / 60;
    int curSec   = static_cast<int>(pts) % 60;
    int curFrame = static_cast<int>(m_videoWidget->currentIdx() %
                                    static_cast<int64_t>(m_videoWidget->fps()));
    int durMin   = static_cast<int>(dur) / 60;
    int durSec   = static_cast<int>(dur) % 60;

    QString speedStr;
    if (std::abs(speed - 1.0) > 0.05) {
        speedStr = QString(" [%1x]").arg(speed, 0, 'f', 1);
    }

    setWindowTitle(QString("vScrubber2 %1 %2:%3.%4 / %5:%6%7")
                       .arg(state)
                       .arg(curMin, 2, 10, QChar('0'))
                       .arg(curSec, 2, 10, QChar('0'))
                       .arg(curFrame, 2, 10, QChar('0'))
                       .arg(durMin, 2, 10, QChar('0'))
                       .arg(durSec, 2, 10, QChar('0'))
                       .arg(speedStr));
}