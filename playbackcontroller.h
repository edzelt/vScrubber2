#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

// ── Контроллер воспроизведения с wall-clock планированием ─────────────────────
//
// Заменяет временный QTimer в MainWindow.
// Работает на точном wall-clock таймере:
//   - Высокочастотный tick (~2мс) считает, сколько кадров пора показать
//   - Поддержка переменной скорости 0.1x–3.0x (шаг 0.1x)
//   - Реверс (отрицательная скорость)
//   - Автоматический шаг по N кадров при высокой скорости (>1.0x)
//
// Сигнал stepRequested(int delta) → VideoWidget::stepFrame
// Сигнал speedChanged(double) → OSD
// Сигнал stateChanged(PlaybackState) → UI
//
class PlaybackController : public QObject
{
    Q_OBJECT

public:
    enum State {
        Stopped,   // файл не загружен или конец
        Paused,    // пауза (скорость помнится)
        Playing    // воспроизведение
    };
    Q_ENUM(State)

    explicit PlaybackController(QObject* parent = nullptr);

    // ── Управление ───────────────────────────────────────────────────────────
    void setFps(double fps);             // установить FPS файла
    void togglePlayPause();              // переключить Play/Pause
    void play();                         // запустить воспроизведение
    void pause();                        // поставить на паузу
    void stop();                         // остановить (сброс состояния)

    // ── Скорость ─────────────────────────────────────────────────────────────
    void setSpeed(double speed);         // установить скорость (-3.0 .. +3.0)
    void adjustSpeed(double delta);      // изменить скорость на delta (обычно ±0.1)
    void resetSpeed();                   // сбросить скорость на 1.0x

    // ── Свойства ─────────────────────────────────────────────────────────────
    State  state()    const { return m_state; }
    double speed()    const { return m_speed; }
    double fps()      const { return m_fps; }
    bool   isPlaying() const { return m_state == Playing; }
    bool   isPaused()  const { return m_state == Paused; }

    // ── Уведомление о конце файла (вызывается из VideoWidget) ────────────────
    void notifyEndOfFile();

signals:
    void stepRequested(int delta);              // запрос шага на delta кадров
    void speedChanged(double speed);            // скорость изменилась
    void stateChanged(PlaybackController::State state);  // состояние изменилось
    void continuousPlayChanged(bool enabled);   // для VideoWidget: вкл/выкл непрерывный режим

private slots:
    void onTick();

private:
    void startTicker();
    void stopTicker();

    // ── Состояние ────────────────────────────────────────────────────────────
    State  m_state = Stopped;
    double m_speed = 1.0;        // текущая скорость (отрицательная = реверс)
    double m_fps   = 30.0;       // FPS видеофайла

    // ── Wall-clock планирование ──────────────────────────────────────────────
    QTimer         m_ticker;           // высокочастотный таймер (~2мс)
    QElapsedTimer  m_clock;            // wall-clock для подсчёта кадров
    double         m_frameDebt = 0.0;  // накопленные «недопоказанные» кадры

    // ── Ограничения ──────────────────────────────────────────────────────────
    static constexpr double MIN_SPEED =  0.1;
    static constexpr double MAX_SPEED =  3.0;
    static constexpr int    TICK_INTERVAL_MS = 2;  // интервал тика
};