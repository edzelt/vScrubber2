#pragma once

#include <QPointF>
#include <QSizeF>

// ── Состояние Zoom/Pan для видео ─────────────────────────────────────────────
//
// Хранит текущий масштаб и смещение, вычисляет uScale/uOffset для шейдера.
//
// Шейдер использует формулу: uv = (vUV - 0.5 - uOffset) / uScale + 0.5
// где uScale = letterboxScale / zoomLevel, uOffset = panOffset.
//
// Zoom:
//   - zoomLevel = 1.0 — исходный размер (fit to screen)
//   - zoomLevel = 2.0 — двукратное увеличение
//   - Минимум 1.0 (нельзя уменьшить меньше экрана)
//   - Максимум 16.0
//   - Zoom в точку курсора (pivot point)
//
// Pan:
//   - Только при zoom > 1.0
//   - Ограничен границами кадра (нельзя выйти за пределы видео)
//
class ZoomPanState
{
public:
    ZoomPanState() = default;

    // ── Zoom ─────────────────────────────────────────────────────────────────

    // Zoom с привязкой к точке курсора.
    // delta > 0 — приближение, delta < 0 — отдаление.
    // cursorPos — позиция курсора в координатах виджета (пиксели).
    // widgetSize — размер виджета (пиксели).
    void zoom(double delta, const QPointF& cursorPos, const QSizeF& widgetSize);

    // Сбросить zoom на 1.0
    void resetZoom();

    double zoomLevel() const { return m_zoomLevel; }

    // ── Pan ──────────────────────────────────────────────────────────────────

    // Сместить вид на delta пикселей.
    // widgetSize нужен для конвертации пикселей в UV координаты.
    void pan(const QPointF& deltaPx, const QSizeF& widgetSize);

    QPointF panOffset() const { return m_panOffset; }

    // ── Для шейдера ──────────────────────────────────────────────────────────

    // Вычислить uScale с учётом letterbox и zoom.
    // videoAspect = videoW / videoH, screenAspect = screenW / screenH.
    QPointF calcShaderScale(float videoAspect, float screenAspect) const;

    // Вычислить uOffset (= panOffset, уже в UV координатах).
    QPointF calcShaderOffset() const { return m_panOffset; }

    // ── Ограничения ──────────────────────────────────────────────────────────
    static constexpr double MIN_ZOOM =  1.0;
    static constexpr double MAX_ZOOM = 16.0;
    static constexpr double ZOOM_STEP = 1.15;  // множитель за один щелчок колеса

private:
    void clampPan(float videoAspect, float screenAspect);

    double  m_zoomLevel = 1.0;
    QPointF m_panOffset{0.0, 0.0};  // смещение в UV координатах
};