#include "zoompanstate.h"
#include <algorithm>
#include <cmath>

void ZoomPanState::setAspects(float videoAspect, float screenAspect)
{
    m_videoAspect  = videoAspect;
    m_screenAspect = screenAspect;
}

// ── Zoom в точку курсора ─────────────────────────────────────────────────────
void ZoomPanState::zoom(double delta, const QPointF& cursorPos, const QSizeF& widgetSize)
{
    if (widgetSize.width() <= 0 || widgetSize.height() <= 0) return;

    double oldZoom = m_zoomLevel;

    // delta < 0 (колесо от себя) = приближение, delta > 0 (к себе) = отдаление
    if (delta > 0)
        m_zoomLevel *= ZOOM_STEP;
    else
        m_zoomLevel /= ZOOM_STEP;

    m_zoomLevel = std::clamp(m_zoomLevel, MIN_ZOOM, MAX_ZOOM);

    if (std::abs(m_zoomLevel - oldZoom) < 0.001) return;

    // Пересчёт panOffset чтобы точка под курсором осталась на месте
    double cx = cursorPos.x() / widgetSize.width();
    double cy = cursorPos.y() / widgetSize.height();

    // Шейдер: uv = (vUV - 0.5 - pan) / (letterbox * zoom) + 0.5
    // Условие: uvOld == uvNew
    // → panNew = pivot * (1 - zNew/zOld) + panOld * (zNew/zOld)
    double zRatio = m_zoomLevel / oldZoom;
    double pivotX = cx - 0.5;
    double pivotY = cy - 0.5;

    m_panOffset.setX(pivotX * (1.0 - zRatio) + m_panOffset.x() * zRatio);
    m_panOffset.setY(pivotY * (1.0 - zRatio) + m_panOffset.y() * zRatio);

    clampPan();
}

void ZoomPanState::resetZoom()
{
    m_zoomLevel = 1.0;
    m_panOffset = {0.0, 0.0};
}

// ── Pan ──────────────────────────────────────────────────────────────────────
void ZoomPanState::pan(const QPointF& deltaPx, const QSizeF& widgetSize)
{
    if (m_zoomLevel <= 1.001) return;
    if (widgetSize.width() <= 0 || widgetSize.height() <= 0) return;

    // Пиксели → нормализованные координаты
    // uOffset сдвигает точку отсчёта vUV:
    //   uOffset.x > 0 → картинка сдвигается вправо
    //   uOffset.y > 0 → картинка сдвигается вниз (UV.y=0 это верх)
    double dx = deltaPx.x() / widgetSize.width();
    double dy = deltaPx.y() / widgetSize.height();

    m_panOffset.setX(m_panOffset.x() + dx);
    m_panOffset.setY(m_panOffset.y() + dy);

    clampPan();
}

// ── Вычислить uScale для шейдера ─────────────────────────────────────────────
QPointF ZoomPanState::calcShaderScale() const
{
    float scaleX = 1.0f, scaleY = 1.0f;
    if (m_screenAspect > m_videoAspect)
        scaleX = m_videoAspect / m_screenAspect;
    else
        scaleY = m_screenAspect / m_videoAspect;

    float z = static_cast<float>(m_zoomLevel);
    return QPointF(scaleX * z, scaleY * z);
}

// ── Ограничение pan — картинка не выходит за границы ─────────────────────────
// При zoom > 1 видимая область текстуры = 1/(letterbox*zoom).
// Край картинки должен быть на краю экрана или дальше.
// В терминах panOffset: |offset| ≤ 0.5 - 0.5/(letterbox*zoom)
void ZoomPanState::clampPan()
{
    if (m_zoomLevel <= 1.001) {
        m_panOffset = {0.0, 0.0};
        return;
    }

    QPointF scale = calcShaderScale();

    // Максимальное смещение по каждой оси
    // Видимая ширина в UV = 1/uScale. Чтобы край не оторвался от экрана:
    // |offset| ≤ 0.5 - 0.5/uScale = 0.5 * (1 - 1/uScale)
    double maxX = std::max(0.0, 0.5 * (1.0 - 1.0 / scale.x()));
    double maxY = std::max(0.0, 0.5 * (1.0 - 1.0 / scale.y()));

    m_panOffset.setX(std::clamp(m_panOffset.x(), -maxX, maxX));
    m_panOffset.setY(std::clamp(m_panOffset.y(), -maxY, maxY));
}