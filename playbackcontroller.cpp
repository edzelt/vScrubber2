#include "playbackcontroller.h"
#include <cmath>

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent)
{
    m_ticker.setTimerType(Qt::PreciseTimer);
    m_ticker.setInterval(TICK_INTERVAL_MS);
    connect(&m_ticker, &QTimer::timeout, this, &PlaybackController::onTick);
}

// ── Установить FPS файла ─────────────────────────────────────────────────────
void PlaybackController::setFps(double fps)
{
    if (fps > 0.0)
        m_fps = fps;
}

// ── Play / Pause / Stop ──────────────────────────────────────────────────────

void PlaybackController::togglePlayPause()
{
    if (m_state == Playing)
        pause();
    else
        play();
}

void PlaybackController::play()
{
    if (m_state == Playing) return;

    m_state = Playing;
    m_frameDebt = 0.0;
    startTicker();

    // Уведомляем VideoWidget: включить непрерывное декодирование
    // (только при положительной скорости — реверс через seekTo)
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

// ── Управление скоростью ─────────────────────────────────────────────────────

void PlaybackController::setSpeed(double speed)
{
    // Округляем до 0.2 (шаг скорости)
    double newSpeed = std::round(speed * 5.0) / 5.0;

    // Ограничиваем диапазон: -MAX_SPEED .. +MAX_SPEED, 0 допускается
    newSpeed = std::clamp(newSpeed, -MAX_SPEED, MAX_SPEED);

    // Малые значения (< MIN_SPEED по модулю) округляем до 0
    if (std::abs(newSpeed) > 0.0 && std::abs(newSpeed) < MIN_SPEED)
        newSpeed = 0.0;

    if (std::abs(newSpeed - m_speed) < 0.001) return;

    m_speed = newSpeed;
    m_frameDebt = 0.0;  // сброс накопления при смене скорости

    // Обновляем режим непрерывного воспроизведения
    if (m_state == Playing) {
        bool shouldBeContinuous = (m_speed > 0.0);
        emit continuousPlayChanged(shouldBeContinuous);
    }

    emit speedChanged(m_speed);
}

void PlaybackController::adjustSpeed(double delta)
{
    setSpeed(m_speed + delta);
}

void PlaybackController::resetSpeed()
{
    setSpeed(1.0);
}

// ── Уведомление о конце файла ────────────────────────────────────────────────
void PlaybackController::notifyEndOfFile()
{
    if (m_state == Playing) {
        pause();
    }
}

// ── Wall-clock tick ──────────────────────────────────────────────────────────
// Вызывается каждые ~2мс. Считает сколько кадров пора показать
// исходя из прошедшего реального времени и текущей скорости.
void PlaybackController::onTick()
{
    if (m_state != Playing) return;
    if (std::abs(m_speed) < 0.001) return;  // скорость 0 — стоим

    // Сколько секунд прошло с последнего тика
    double elapsedSec = m_clock.nsecsElapsed() / 1.0e9;
    m_clock.restart();

    // Защита от аномально больших скачков (>100мс — вкладка была в фоне и т.п.)
    if (elapsedSec > 0.1)
        elapsedSec = 0.1;

    // Сколько кадров нужно показать за это время
    // При скорости 1.0x и 30fps: 0.002с × 30 × 1.0 = 0.06 кадра за тик
    // Накапливаем дробную часть в m_frameDebt
    double framesNeeded = elapsedSec * m_fps * std::abs(m_speed);
    m_frameDebt += framesNeeded;

    // Показываем по одному кадру за итерацию — сохраняем reference chain
    // При высокой скорости просто чаще попадаем сюда с framesToStep > 1,
    // но шагаем строго по 1 за вызов stepRequested
    int framesToStep = static_cast<int>(m_frameDebt);
    if (framesToStep > 0) {
        m_frameDebt -= framesToStep;

        // Направление: отрицательная скорость = реверс
        int direction = (m_speed < 0.0) ? -1 : 1;

        // Шагаем по одному кадру в цикле — HEVC декодер требует
        // последовательного декодирования для корректных reference frames
        for (int i = 0; i < framesToStep; ++i) {
            emit stepRequested(direction);
        }
    }
}

// ── Управление тикером ───────────────────────────────────────────────────────

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