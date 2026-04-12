#pragma once

#include <QPainter>
#include <QString>

// ── OSD (On-Screen Display) ──────────────────────────────────────────────────
//
// Рисует текстовую информацию поверх видео через QPainter:
//   - Таймкод / общая длительность
//   - Скорость воспроизведения (если ≠ 1.0x)
//   - Масштаб (если > 1.0x)
//
// Рисуется в левом верхнем углу виджета.
// Вызывается из VideoWidget::paintGL() после отрисовки видео.
//
class OSD
{
public:
    OSD() = default;

    // ── Обновление данных ────────────────────────────────────────────────────
    void setTimecode(double pts, double duration, double fps);
    void setSpeed(double speed);
    void setZoom(double zoom);
    void setWheelMode(int mode);  // 0=Jog, 1=Shuttle

    // ── Отрисовка ────────────────────────────────────────────────────────────
    // Вызывать после endNativePainting или через QPainter overlay.
    // widgetW/H — размер виджета в пикселях (не device pixels).
    void draw(QPainter& painter, int widgetW, int widgetH);

private:
    // Форматирование таймкода MM:SS.FF
    static QString formatTimecode(double pts, double fps);

    double m_pts      = 0.0;
    double m_duration = 0.0;
    double m_fps      = 25.0;
    double m_speed    = 1.0;
    double m_zoom     = 1.0;
    int    m_wheelMode = 0;  // 0=Jog, 1=Shuttle
};