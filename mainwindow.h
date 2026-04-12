#pragma once

#include <QMainWindow>

class VideoWidget;
class PlaybackController;
class InputController;

// ── Главное окно vScrubber2 ──────────────────────────────────────────────────
//
// UI:
//   - Меню: Файл → Открыть, Выход
//   - Управление: полностью через InputController (mouse-only workflow)
//   - Воспроизведение: PlaybackController (wall-clock планирование)
//
// InputController обрабатывает все события мыши и клавиатуры,
// делегируя команды в PlaybackController и VideoWidget.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Открыть файл (можно вызвать извне, например из main)
    void openFile(const QString& path);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onOpenFile();
    void onFileLoaded(bool success);
    void onPositionChanged(double pts);
    void onSpeedChanged(double speed);
    void onPlaybackStateChanged(int state);
    void onSeekKeyframe(int direction);
    void onEndOfFile();

private:
    void setupMenu();
    void updateTitle();
    void openFileKeepState(const QString& path);  // открытие с сохранением режима

    VideoWidget*        m_videoWidget = nullptr;
    PlaybackController* m_playback    = nullptr;
    InputController*    m_input       = nullptr;

    // Отложенное восстановление play после загрузки файла
    double m_pendingSpeed = 1.0;
    bool   m_pendingPlay  = false;
};