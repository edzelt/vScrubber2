#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

// ── Контроллер воспроизведения с wall-clock планированием ─────────────────────
//
// «Сердце» плеера. Тикает каждые 2мс, считает кадры по wall-clock.
//
//   play()        → запустить тикер, emit continuousPlayChanged
//   pause()       → остановить тикер
//   resetClock()  → сбросить часы и долг (вызывать после seek/gopDecoded)
//   stepRequested → VideoWidget::stepFrame
//
class PlaybackController : public QObject
{
    Q_OBJECT

public:
    enum State { Stopped, Paused, Playing };
    Q_ENUM(State)

    explicit PlaybackController(QObject* parent = nullptr);

    void setFps(double fps);
    void togglePlayPause();
    void play();
    void pause();
    void stop();

    void setSpeed(double speed);
    void adjustSpeed(double delta);
    void resetSpeed();

    // Сброс wall-clock и накопленного долга.
    // Вызывать после seek, gopDecoded — чтобы тикер не пытался
    // «догнать» время, потраченное на декодирование.
    void resetClock();

    State  state()    const { return m_state; }
    double speed()    const { return m_speed; }
    double fps()      const { return m_fps; }
    bool   isPlaying() const { return m_state == Playing; }
    bool   isPaused()  const { return m_state == Paused; }

    void notifyEndOfFile();

signals:
    void stepRequested(int delta);
    void speedChanged(double speed);
    void stateChanged(PlaybackController::State state);
    void continuousPlayChanged(bool enabled);

private slots:
    void onTick();

private:
    void startTicker();
    void stopTicker();

    State  m_state = Stopped;
    double m_speed = 1.0;
    double m_fps   = 30.0;

    QTimer         m_ticker;
    QElapsedTimer  m_clock;
    double         m_frameDebt = 0.0;

    static constexpr double MIN_SPEED =  0.2;
    static constexpr double MAX_SPEED =  3.0;
    static constexpr int    TICK_INTERVAL_MS = 2;
};