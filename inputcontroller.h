#pragma once

#include <QObject>
#include <QPointF>
#include <QTimer>

class PlaybackController;
class VideoWidget;

// ── Контроллер ввода: Mouse-Only Workflow ────────────────────────────────────
//
// Воспроизведение:
//   ЛКМ клик         → Play/Pause (сброс shuttle, скорость 1x)
//   ЛКМ двойной клик → Fullscreen toggle (play/pause НЕ срабатывает)
//   Space             → Play/Pause
//   Esc               → Выход из fullscreen
//
// Навигация (колесо мыши):
//   Режим Jog:      колесо ±1 = ±1 кадр
//   Режим Shuttle:  колесо ±1 = скорость ±0.2x
//   ПКМ + колесо:   перемотка по keyframe
//
// Обзор:
//   ЛКМ + колесо    → zoom
//   ЛКМ + движение  → pan
//
class InputController : public QObject
{
    Q_OBJECT

public:
    enum WheelMode { Jog, Shuttle };
    Q_ENUM(WheelMode)

    explicit InputController(PlaybackController* playback,
                             QObject* parent = nullptr);

    void setVideoWidget(VideoWidget* vw) { m_videoWidget = vw; }

    // ── Обработчики событий (вызываются из VideoWidget) ──────────────────────
    void handleMousePress(Qt::MouseButton button, const QPointF& pos);
    void handleMouseRelease(Qt::MouseButton button, const QPointF& pos);
    void handleMouseMove(const QPointF& pos);
    void handleMouseDoubleClick(Qt::MouseButton button);
    void handleWheel(int angleDelta, const QPointF& pos);
    void handleKeyPress(int key);

    WheelMode wheelMode() const { return m_wheelMode; }
    void reset();

signals:
    void seekKeyframe(int direction);
    void zoomRequested(double delta, const QPointF& pos);
    void panRequested(const QPointF& delta);
    void wheelModeChanged(InputController::WheelMode mode);

private slots:
    void onClickTimer();           // отложенный single click

private:
    void doPlayPause();
    void toggleWheelMode();

    PlaybackController* m_playback    = nullptr;
    VideoWidget*        m_videoWidget = nullptr;

    // ── Состояние кнопок ─────────────────────────────────────────────────────
    bool m_leftPressed    = false;
    bool m_rightPressed   = false;
    bool m_rightUsedWheel = false;

    // ── Режим колеса ─────────────────────────────────────────────────────────
    WheelMode m_wheelMode = Jog;

    // ── Pan / click разделение ───────────────────────────────────────────────
    QPointF m_pressPos;            // позиция нажатия ЛКМ
    QPointF m_lastMousePos;        // последняя позиция для delta pan
    bool    m_panning     = false; // ЛКМ зажата + мышь двигалась
    bool    m_leftUsedZoom = false; // ЛКМ использовалась для zoom

    // ── Single / Double click разделение ─────────────────────────────────────
    // При release ЛКМ: не вызываем play/pause сразу, а ставим таймер.
    // Если за 250мс придёт doubleClick — отменяем таймер → fullscreen.
    // Если таймер сработал — single click → play/pause.
    QTimer  m_clickTimer;
    bool    m_waitingDoubleClick = false;
    bool    m_suppressClick      = false;  // подавляет click после doubleClick

    static constexpr double PAN_THRESHOLD = 5.0;  // пиксели
};