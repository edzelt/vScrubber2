#include "playbackcontroller.h"
#include <cmath>

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent)
{
    m_ticker.setTimerType(Qt::PreciseTimer);
    m_ticker.setInterval(TICK_INTERVAL_MS);
    connect(&m_ticker, &QTimer::timeout, this, &PlaybackController::onTick);
}

void PlaybackController::setFps(double fps)
{
    if (fps > 0.0) m_fps = fps;
}

// ── Play / Pause / Stop ──────────────────────────────────────────────────────

void PlaybackController::play()
{
    if (m_state == Playing) return;
    m_state = Playing;
    startTicker();
    emit continuousPlayChanged(m_speed > 0.0);
    emit stateChanged(m_state);
}

void PlaybackController::pause()
{
    if (m_state != Playing) return;
    m_state = Paused;
    stopTicker();
    emit continuousPlayChanged(false);
    emit stateChanged(m_state);
}

void PlaybackController::stop()
{
    stopTicker();
    m_state = Stopped;
    m_frameDebt = 0.0;
    emit continuousPlayChanged(false);
    emit stateChanged(m_state);
}

void PlaybackController::togglePlayPause()
{
    if (m_state == Playing) pause(); else play();
}

// ── Скорость ─────────────────────────────────────────────────────────────────

void PlaybackController::setSpeed(double speed)
{
    double newSpeed = std::round(speed * 5.0) / 5.0;  // округление до 0.2
    newSpeed = std::clamp(newSpeed, -MAX_SPEED, MAX_SPEED);
    if (std::abs(newSpeed) > 0.0 && std::abs(newSpeed) < MIN_SPEED)
        newSpeed = 0.0;
    if (std::abs(newSpeed - m_speed) < 0.001) return;

    bool wasContinuous = (m_speed > 0.0);
    m_speed = newSpeed;
    m_frameDebt = 0.0;

    // Обновляем continuous play только если направление изменилось
    if (m_state == Playing) {
        bool shouldBeContinuous = (m_speed > 0.0);
        if (shouldBeContinuous != wasContinuous)
            emit continuousPlayChanged(shouldBeContinuous);
    }

    emit speedChanged(m_speed);
}

void PlaybackController::adjustSpeed(double delta) { setSpeed(m_speed + delta); }
void PlaybackController::resetSpeed() { setSpeed(1.0); }

// ── resetClock — сброс после seek ────────────────────────────────────────────
void PlaybackController::resetClock()
{
    m_frameDebt = 0.0;
    m_clock.restart();
}

void PlaybackController::notifyEndOfFile()
{
    if (m_state == Playing) pause();
}

// ── Wall-clock tick ──────────────────────────────────────────────────────────
void PlaybackController::onTick()
{
    if (m_state != Playing) return;
    if (std::abs(m_speed) < 0.001) return;

    double elapsedSec = m_clock.nsecsElapsed() / 1.0e9;
    m_clock.restart();

    if (elapsedSec > 0.1) elapsedSec = 0.1;

    double framesNeeded = elapsedSec * m_fps * std::abs(m_speed);
    m_frameDebt += framesNeeded;

    int framesToStep = static_cast<int>(m_frameDebt);
    if (framesToStep > 0) {
        m_frameDebt -= framesToStep;
        int direction = (m_speed < 0.0) ? -1 : 1;

        for (int i = 0; i < framesToStep; ++i)
            emit stepRequested(direction);
    }
}

void PlaybackController::startTicker()
{
    m_frameDebt = 0.0;
    m_clock.restart();
    m_ticker.start();
}

void PlaybackController::stopTicker()
{
    m_ticker.stop();
}