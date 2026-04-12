#include "inputcontroller.h"
#include "playbackcontroller.h"
#include "videowidget.h"
#include <QMainWindow>
#include <QMenuBar>
#include <QApplication>
#include <QDebug>
#include <cmath>

InputController::InputController(PlaybackController* playback, QObject* parent)
    : QObject(parent)
    , m_playback(playback)
{
    // Таймер для разделения single/double click
    m_clickTimer.setSingleShot(true);
    m_clickTimer.setInterval(QApplication::doubleClickInterval());
    connect(&m_clickTimer, &QTimer::timeout, this, &InputController::onClickTimer);
}

// ── Нажатие кнопки мыши ──────────────────────────────────────────────────────

void InputController::handleMousePress(Qt::MouseButton button, const QPointF& pos)
{
    if (button == Qt::LeftButton) {
        m_leftPressed    = true;
        m_panning        = false;
        m_leftUsedZoom   = false;
        m_suppressClick  = false;  // новый press — сбрасываем подавление
        m_pressPos       = pos;
        m_lastMousePos   = pos;
    }
    else if (button == Qt::RightButton) {
        m_rightPressed   = true;
        m_rightUsedWheel = false;
    }
    else if (button == Qt::MiddleButton) {
        toggleWheelMode();
    }
}

// ── Отпускание кнопки мыши ───────────────────────────────────────────────────

void InputController::handleMouseRelease(Qt::MouseButton button, const QPointF& pos)
{
    Q_UNUSED(pos)

    if (button == Qt::LeftButton) {
        // Если был pan, zoom или подавлен после doubleClick — не клик
        if (!m_panning && !m_leftUsedZoom && !m_suppressClick) {
            m_waitingDoubleClick = true;
            m_clickTimer.start();
        }
        m_leftPressed  = false;
        m_panning      = false;
        m_leftUsedZoom = false;
    }
    else if (button == Qt::RightButton) {
        // Если ПКМ не использовалась с колесом — контекстное меню (Qt обработает)
        m_rightPressed   = false;
        m_rightUsedWheel = false;
    }
}

// ── Движение мыши ────────────────────────────────────────────────────────────

void InputController::handleMouseMove(const QPointF& pos)
{
    if (m_leftPressed) {
        QPointF delta = pos - m_lastMousePos;

        // Проверяем порог pan
        if (!m_panning) {
            QPointF totalDelta = pos - m_pressPos;
            double dist = std::sqrt(totalDelta.x() * totalDelta.x() +
                                    totalDelta.y() * totalDelta.y());
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

// ── Двойной клик ─────────────────────────────────────────────────────────────

void InputController::handleMouseDoubleClick(Qt::MouseButton button)
{
    if (button == Qt::LeftButton) {
        // Отменяем отложенный single click — это double click
        m_clickTimer.stop();
        m_waitingDoubleClick = false;
        m_suppressClick = true;  // подавить play/pause от второго release

        if (m_videoWidget) {
            QWidget* window = m_videoWidget->window();
            if (window->isFullScreen()) {
                window->showNormal();
                if (auto* mw = qobject_cast<QMainWindow*>(window))
                    mw->menuBar()->show();
            } else {
                if (auto* mw = qobject_cast<QMainWindow*>(window))
                    mw->menuBar()->hide();
                window->showFullScreen();
            }
        }
    }
}

// ── Таймер single click (сработал = это был одиночный клик) ──────────────────

void InputController::onClickTimer()
{
    if (m_waitingDoubleClick) {
        m_waitingDoubleClick = false;
        doPlayPause();
    }
}

// ── Колесо мыши ──────────────────────────────────────────────────────────────

void InputController::handleWheel(int angleDelta, const QPointF& pos)
{
    int steps = angleDelta / 120;
    if (steps == 0) return;

    // ── ПКМ + колесо: перемотка по keyframe ──────────────────────────────────
    if (m_rightPressed) {
        m_rightUsedWheel = true;

        if (m_wheelMode == Shuttle) {
            m_wheelMode = Jog;
            emit wheelModeChanged(m_wheelMode);
        }
        m_playback->pause();
        m_playback->setSpeed(1.0);

        int direction = (steps > 0) ? 1 : -1;
        emit seekKeyframe(direction);
        return;
    }

    // ── ЛКМ + колесо: zoom ──────────────────────────────────────────────────
    if (m_leftPressed) {
        m_leftUsedZoom = true;
        m_panning = true;  // предотвращаем play/pause при отпускании
        double zoomDelta = (steps > 0) ? 1.0 : -1.0;
        emit zoomRequested(zoomDelta, pos);
        return;
    }

    // ── Обычное колесо: Jog или Shuttle ──────────────────────────────────────
    switch (m_wheelMode) {
    case Jog:
        if (m_playback->isPlaying()) {
            m_playback->pause();
            m_playback->setSpeed(1.0);
        }
        if (m_videoWidget)
            m_videoWidget->stepFrame(steps);
        break;

    case Shuttle:
        m_playback->adjustSpeed(steps * 0.2);

        if (std::abs(m_playback->speed()) < 0.05) {
            if (m_playback->isPlaying())
                m_playback->pause();
        } else {
            if (!m_playback->isPlaying())
                m_playback->play();
        }
        break;
    }
}

// ── Клавиатура ───────────────────────────────────────────────────────────────

void InputController::handleKeyPress(int key)
{
    switch (key) {
    case Qt::Key_Space:
        doPlayPause();
        break;

    case Qt::Key_Escape:
        if (m_videoWidget) {
            QWidget* window = m_videoWidget->window();
            if (window->isFullScreen()) {
                window->showNormal();
                if (auto* mw = qobject_cast<QMainWindow*>(window))
                    mw->menuBar()->show();
            }
        }
        break;

    default:
        break;
    }
}

// ── Play/Pause с полным сбросом shuttle ──────────────────────────────────────

void InputController::doPlayPause()
{
    if (m_playback->isPlaying()) {
        m_playback->pause();
    } else {
        if (m_wheelMode == Shuttle) {
            m_wheelMode = Jog;
            emit wheelModeChanged(m_wheelMode);
        }
        m_playback->setSpeed(1.0);
        m_playback->play();
    }
}

// ── Сброс ────────────────────────────────────────────────────────────────────

void InputController::reset()
{
    m_wheelMode = Jog;
    m_leftPressed = false;
    m_rightPressed = false;
    m_rightUsedWheel = false;
    m_panning = false;
    m_leftUsedZoom = false;
    m_clickTimer.stop();
    m_waitingDoubleClick = false;
    m_suppressClick = false;
    emit wheelModeChanged(m_wheelMode);
}

// ── Переключение режима колеса ───────────────────────────────────────────────

void InputController::toggleWheelMode()
{
    if (m_wheelMode == Jog) {
        m_wheelMode = Shuttle;
        m_playback->pause();
        m_playback->setSpeed(0.0);
    } else {
        m_wheelMode = Jog;
        m_playback->pause();
        m_playback->setSpeed(1.0);
    }
    emit wheelModeChanged(m_wheelMode);
}