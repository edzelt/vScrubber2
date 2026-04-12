#include "inputcontroller.h"
#include "playbackcontroller.h"
#include "videowidget.h"
#include <QDebug>
#include <cmath>

InputController::InputController(PlaybackController* playback, QObject* parent)
    : QObject(parent)
    , m_playback(playback)
{
}

// ── Обработка нажатий мыши ───────────────────────────────────────────────────

void InputController::handleMousePress(Qt::MouseButton button, const QPointF& pos)
{
    if (button == Qt::LeftButton) {
        m_leftPressed = true;
        m_panning = false;
        m_lastMousePos = pos;
    }
    else if (button == Qt::RightButton) {
        m_rightPressed = true;
        m_rightUsedWheel = false;  // пока колесо не крутили — меню возможно
    }
    else if (button == Qt::MiddleButton) {
        toggleWheelMode();
    }
}

void InputController::handleMouseRelease(Qt::MouseButton button, const QPointF& pos)
{
    Q_UNUSED(pos)

    if (button == Qt::LeftButton) {
        // Если не было pan — это клик → Play/Pause
        if (!m_panning) {
            doPlayPause();
        }
        m_leftPressed = false;
        m_panning = false;
    }
    else if (button == Qt::RightButton) {
        // Если ПКМ не использовалась с колесом — показываем контекстное меню
        if (!m_rightUsedWheel && m_videoWidget) {
            // Контекстное меню будет обработано стандартным Qt механизмом
            // (QWidget::customContextMenuRequested или contextMenuEvent)
        }
        m_rightPressed = false;
        m_rightUsedWheel = false;
    }
}

void InputController::handleMouseMove(const QPointF& pos)
{
    if (m_leftPressed) {
        QPointF delta = pos - m_lastMousePos;

        // Проверяем порог pan — защита от случайного срабатывания при клике
        if (!m_panning) {
            double dist = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
            if (dist >= PAN_THRESHOLD) {
                m_panning = true;
            }
        }

        if (m_panning) {
            emit panRequested(delta);
        }

        m_lastMousePos = pos;
    }
}

void InputController::handleMouseDoubleClick(Qt::MouseButton button)
{
    if (button == Qt::LeftButton && m_videoWidget) {
        // Fullscreen toggle
        QWidget* window = m_videoWidget->window();
        if (window->isFullScreen()) {
            window->showNormal();
        } else {
            window->showFullScreen();
        }
    }
}

// ── Обработка колеса мыши ────────────────────────────────────────────────────
// angleDelta: положительный = вперёд (от себя), отрицательный = назад (к себе)

void InputController::handleWheel(int angleDelta, const QPointF& pos)
{
    // Нормализуем: 1 щелчок = ±120 единиц → ±1 шаг
    int steps = angleDelta / 120;
    if (steps == 0) return;

    // ── ПКМ зажата + колесо: перемотка по ключевым кадрам ────────────────────
    if (m_rightPressed) {
        m_rightUsedWheel = true;

        // Сбрасываем shuttle и скорость
        if (m_wheelMode == Shuttle) {
            m_wheelMode = Jog;
            emit wheelModeChanged(m_wheelMode);
        }
        m_playback->pause();
        m_playback->setSpeed(1.0);  // сбрасываем скорость на 1x

        // Перемотка по keyframe: direction = знак steps
        int direction = (steps > 0) ? 1 : -1;
        emit seekKeyframe(direction);
        return;
    }

    // ── ЛКМ зажата + колесо: zoom ───────────────────────────────────────────
    if (m_leftPressed) {
        m_panning = true;  // предотвращаем play/pause при отпускании
        double zoomDelta = (steps > 0) ? 0.1 : -0.1;
        emit zoomRequested(zoomDelta, pos);
        return;
    }

    // ── Обычное колесо: Jog или Shuttle ──────────────────────────────────────
    switch (m_wheelMode) {
    case Jog:
        // 1 щелчок = ±1 кадр, пауза воспроизведения
        if (m_playback->isPlaying()) {
            m_playback->pause();
            m_playback->setSpeed(1.0);
        }
        if (m_videoWidget) {
            m_videoWidget->stepFrame(steps);
        }
        break;

    case Shuttle:
        // Колесо меняет скорость на ±0.1x (свободно проходит через 0 в реверс)
        m_playback->adjustSpeed(steps * 0.1);

        // Автоматический play/pause в зависимости от скорости
        if (std::abs(m_playback->speed()) < 0.05) {
            // Скорость 0 — пауза
            if (m_playback->isPlaying())
                m_playback->pause();
        } else {
            // Скорость ≠ 0 — play
            if (!m_playback->isPlaying())
                m_playback->play();
        }
        break;
    }
}

// ── Обработка клавиатуры ─────────────────────────────────────────────────────

void InputController::handleKeyPress(int key)
{
    switch (key) {
    case Qt::Key_Space:
        doPlayPause();
        break;

    case Qt::Key_Escape:
        // Выход из fullscreen
        if (m_videoWidget) {
            QWidget* window = m_videoWidget->window();
            if (window->isFullScreen()) {
                window->showNormal();
            }
        }
        break;

    default:
        break;
    }
}

// ── Play/Pause с полным сбросом shuttle ──────────────────────────────────────
// Любая команда Play (ЛКМ / Space) сбрасывает Jog/Shuttle
// и возвращает нормальную скорость 1x.
void InputController::doPlayPause()
{
    if (m_playback->isPlaying()) {
        m_playback->pause();
    } else {
        // Сброс shuttle → jog
        if (m_wheelMode == Shuttle) {
            m_wheelMode = Jog;
            emit wheelModeChanged(m_wheelMode);
        }
        // Сброс скорости на 1x
        m_playback->setSpeed(1.0);
        m_playback->play();
    }
}

// ── Переключение режима колеса ───────────────────────────────────────────────
void InputController::toggleWheelMode()
{
    if (m_wheelMode == Jog) {
        m_wheelMode = Shuttle;
        // При входе в shuttle — пауза, скорость 0
        // Пользователь колесом будет задавать скорость и направление
        m_playback->pause();
        m_playback->setSpeed(0.0);
        qDebug() << "InputController: режим Shuttle";
    } else {
        m_wheelMode = Jog;
        // При выходе из shuttle — пауза, сброс скорости
        m_playback->pause();
        m_playback->setSpeed(1.0);
        qDebug() << "InputController: режим Jog";
    }
    emit wheelModeChanged(m_wheelMode);
}