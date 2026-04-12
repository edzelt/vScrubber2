#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QPointF>

class PlaybackController;
class VideoWidget;

// ── Контроллер ввода: Mouse-Only Workflow ────────────────────────────────────
//
// Машина состояний для управления видео мышью:
//
// Воспроизведение:
//   ЛКМ клик         → Play/Pause (сброс shuttle, скорость 1x)
//   ЛКМ двойной клик → Fullscreen toggle
//   Space             → Play/Pause (сброс shuttle, скорость 1x)
//   Esc               → Выход из fullscreen
//   ПКМ               → Контекстное меню (если не было колеса при зажатой ПКМ)
//
// Навигация (колесо мыши):
//   Режим Jog (по умолчанию):
//     Колесо ±1       → ±1 кадр
//   Режим Shuttle (переключается средней кнопкой):
//     Колесо ±1       → скорость ±0.1x, автоматический play
//   ПКМ зажата + колесо:
//     → перемотка по ключевым кадрам, сброс jog/shuttle, скорость 0
//
// Обзор (zoom/pan) — заготовка, ZoomPanState добавится позже:
//   ЛКМ зажата + колесо  → zoom в точку курсора
//   ЛКМ зажата + движение → pan
//
class InputController : public QObject
{
    Q_OBJECT

public:
    // ── Режимы колеса ────────────────────────────────────────────────────────
    enum WheelMode {
        Jog,       // 1 щелчок = ±1 кадр (по умолчанию)
        Shuttle    // колесо управляет скоростью воспроизведения
    };
    Q_ENUM(WheelMode)

    explicit InputController(PlaybackController* playback,
                             QObject* parent = nullptr);

    // ── Привязка к виджету (для fullscreen, seekToKeyframe) ──────────────────
    void setVideoWidget(VideoWidget* vw) { m_videoWidget = vw; }

    // ── Обработчики событий мыши (вызываются из VideoWidget) ─────────────────
    void handleMousePress(Qt::MouseButton button, const QPointF& pos);
    void handleMouseRelease(Qt::MouseButton button, const QPointF& pos);
    void handleMouseMove(const QPointF& pos);
    void handleMouseDoubleClick(Qt::MouseButton button);
    void handleWheel(int angleDelta, const QPointF& pos);

    // ── Обработчики клавиатуры ───────────────────────────────────────────────
    void handleKeyPress(int key);

    // ── Свойства ─────────────────────────────────────────────────────────────
    WheelMode wheelMode() const { return m_wheelMode; }

signals:
    // Навигация по ключевым кадрам (±1 = следующий/предыдущий keyframe)
    void seekKeyframe(int direction);

    // Запрос zoom (delta > 0 = приближение) в точке pos
    void zoomRequested(double delta, const QPointF& pos);

    // Запрос pan на смещение delta
    void panRequested(const QPointF& delta);

    // Режим колеса изменился
    void wheelModeChanged(InputController::WheelMode mode);

private:
    void doPlayPause();         // play/pause с полным сбросом shuttle
    void toggleWheelMode();     // переключить Jog ↔ Shuttle

    PlaybackController* m_playback    = nullptr;
    VideoWidget*        m_videoWidget = nullptr;

    // ── Состояние кнопок ─────────────────────────────────────────────────────
    bool m_leftPressed   = false;   // ЛКМ зажата
    bool m_rightPressed  = false;   // ПКМ зажата
    bool m_rightUsedWheel = false;  // ПКМ была использована с колесом (не показывать меню)

    // ── Режим колеса ─────────────────────────────────────────────────────────
    WheelMode m_wheelMode = Jog;

    // ── Pan tracking ─────────────────────────────────────────────────────────
    QPointF m_lastMousePos;
    bool    m_panning = false;      // true если ЛКМ зажата и мышь двигалась

    // ── Защита от ложного клика при pan ──────────────────────────────────────
    // Если мышь сдвинулась больше порога при зажатой ЛКМ — это pan, не клик
    static constexpr double PAN_THRESHOLD = 3.0; // пиксели
};