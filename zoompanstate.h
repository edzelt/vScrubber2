#pragma once

#include <QPointF>
#include <QSizeF>

// ── Состояние Zoom/Pan для видео ─────────────────────────────────────────────
//
// Шейдер: uv = (vUV - 0.5 - uOffset) / uScale + 0.5
// uScale = letterbox * zoomLevel. Больше uScale → видео крупнее.
// uOffset = panOffset. Смещение в нормализованных координатах.
//
// Zoom: 1.0 = fit to screen, 16.0 = максимум. Колесо к себе = увеличение.
// Pan: только при zoom > 1.0. Границы кадра не выходят за экран.
//
class ZoomPanState
{
public:
    ZoomPanState() = default;

    // ── Установить aspect ratio видео и экрана (вызывать при открытии/resize) ──
    void setAspects(float videoAspect, float screenAspect);

    // ── Zoom в точку курсора ─────────────────────────────────────────────────
    // delta > 0 — приближение, delta < 0 — отдаление.
    void zoom(double delta, const QPointF& cursorPos, const QSizeF& widgetSize);
    void resetZoom();
    double zoomLevel() const { return m_zoomLevel; }

    // ── Pan ──────────────────────────────────────────────────────────────────
    void pan(const QPointF& deltaPx, const QSizeF& widgetSize);
    QPointF panOffset() const { return m_panOffset; }

    // ── Для шейдера ──────────────────────────────────────────────────────────
    QPointF calcShaderScale() const;
    QPointF calcShaderOffset() const { return m_panOffset; }

    // ── Ограничения ──────────────────────────────────────────────────────────
    static constexpr double MIN_ZOOM  =  1.0;
    static constexpr double MAX_ZOOM  = 16.0;
    static constexpr double ZOOM_STEP =  1.15;

private:
    void clampPan();

    double  m_zoomLevel    = 1.0;
    QPointF m_panOffset    {0.0, 0.0};
    float   m_videoAspect  = 16.0f / 9.0f;
    float   m_screenAspect = 16.0f / 9.0f;
};