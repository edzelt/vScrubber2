#include "zoompanstate.h"
#include <algorithm>
#include <cmath>

// ── Zoom в точку курсора ─────────────────────────────────────────────────────
// Принцип: точка под курсором должна остаться на месте после zoom.
// Для этого пересчитываем panOffset при изменении zoomLevel.
void ZoomPanState::zoom(double delta, const QPointF& cursorPos, const QSizeF& widgetSize)
{
    if (widgetSize.width() <= 0 || widgetSize.height() <= 0) return;

    double oldZoom = m_zoomLevel;

    // Множитель: delta > 0 — приближение, < 0 — отдаление
    if (delta > 0)
        m_zoomLevel *= ZOOM_STEP;
    else
        m_zoomLevel /= ZOOM_STEP;

    m_zoomLevel = std::clamp(m_zoomLevel, MIN_ZOOM, MAX_ZOOM);

    // Если zoom не изменился (уже на границе) — ничего не делаем
    if (std::abs(m_zoomLevel - oldZoom) < 0.001) return;

    // Позиция курсора в нормализованных координатах виджета [0..1]
    double cx = cursorPos.x() / widgetSize.width();
    double cy = cursorPos.y() / widgetSize.height();

    // Пересчёт panOffset чтобы точка под курсором осталась на месте:
    // Точка в UV до zoom:  uvOld = (cursor - 0.5 - panOld) / scaleOld + 0.5
    // Точка в UV после zoom: uvNew = (cursor - 0.5 - panNew) / scaleNew + 0.5
    // Условие: uvOld == uvNew
    // Решение: panNew = panOld + (cursor - 0.5) * (1/scaleNew - 1/scaleOld) * scaleOld
    // Упрощённо: panNew = (cursor - 0.5) * (1 - oldZoom/newZoom) + panOld * oldZoom/newZoom
    // ... но проще записать через соотношение масштабов:
    double ratio = oldZoom / m_zoomLevel;
    double pivotX = cx - 0.5;
    double pivotY = cy - 0.5;

    m_panOffset.setX(m_panOffset.x() * ratio + pivotX * (1.0 - ratio));
    m_panOffset.setY(m_panOffset.y() * ratio + pivotY * (1.0 - ratio));
}

// ── Сброс zoom ───────────────────────────────────────────────────────────────
void ZoomPanState::resetZoom()
{
    m_zoomLevel = 1.0;
    m_panOffset = {0.0, 0.0};
}

// ── Pan ──────────────────────────────────────────────────────────────────────
void ZoomPanState::pan(const QPointF& deltaPx, const QSizeF& widgetSize)
{
    if (m_zoomLevel <= 1.0) return;  // pan только при увеличении
    if (widgetSize.width() <= 0 || widgetSize.height() <= 0) return;

    // Конвертируем пиксели в нормализованные координаты
    double dx = deltaPx.x() / widgetSize.width();
    double dy = deltaPx.y() / widgetSize.height();

    m_panOffset.setX(m_panOffset.x() + dx);
    m_panOffset.setY(m_panOffset.y() - dy);  // инвертируем Y (экранные vs UV)
}

// ── Вычислить uScale для шейдера ─────────────────────────────────────────────
// Комбинация letterbox и zoom.
// Шейдер: uv = (vUV - 0.5 - uOffset) / uScale + 0.5
// При uScale < 1 — видео увеличивается (zoom in).
QPointF ZoomPanState::calcShaderScale(float videoAspect, float screenAspect) const
{
    // Letterbox scale (fit to screen)
    float scaleX = 1.0f, scaleY = 1.0f;
    if (screenAspect > videoAspect)
        scaleX = videoAspect / screenAspect;
    else
        scaleY = screenAspect / videoAspect;

    // Применяем zoom (делим — больший zoom = меньший uScale = видео крупнее)
    float z = static_cast<float>(m_zoomLevel);
    return QPointF(scaleX / z, scaleY / z);
}

// ── Ограничение pan границами кадра ──────────────────────────────────────────
void ZoomPanState::clampPan(float videoAspect, float screenAspect)
{
    if (m_zoomLevel <= 1.0) {
        m_panOffset = {0.0, 0.0};
        return;
    }

    // Максимальное смещение зависит от того, сколько видео «выходит» за экран
    // При zoom = 2x видимая область = 50% кадра, смещение ±25% = ±0.25
    QPointF scale = calcShaderScale(videoAspect, screenAspect);
    double maxOffsetX = std::max(0.0, (1.0 - scale.x()) / 2.0);
    double maxOffsetY = std::max(0.0, (1.0 - scale.y()) / 2.0);

    m_panOffset.setX(std::clamp(m_panOffset.x(), -maxOffsetX, maxOffsetX));
    m_panOffset.setY(std::clamp(m_panOffset.y(), -maxOffsetY, maxOffsetY));
}