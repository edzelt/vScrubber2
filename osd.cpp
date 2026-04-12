#include "osd.h"
#include <QFont>
#include <cmath>

// ── Обновление данных ────────────────────────────────────────────────────────

void OSD::setTimecode(double pts, double duration, double fps)
{
    m_pts      = pts;
    m_duration = duration;
    m_fps      = fps;
}

void OSD::setSpeed(double speed)
{
    m_speed = speed;
}

void OSD::setZoom(double zoom)
{
    m_zoom = zoom;
}

void OSD::setWheelMode(int mode)
{
    m_wheelMode = mode;
}

// ── Форматирование таймкода ──────────────────────────────────────────────────
QString OSD::formatTimecode(double pts, double fps)
{
    if (pts < 0.0) pts = 0.0;
    if (fps <= 0.0) fps = 25.0;

    int totalSec = static_cast<int>(pts);
    int minutes  = totalSec / 60;
    int seconds  = totalSec % 60;
    int frames   = static_cast<int>(std::fmod(pts, 1.0) * fps + 0.5);
    if (frames >= static_cast<int>(fps)) {
        frames = 0;
        seconds++;
        if (seconds >= 60) { seconds = 0; minutes++; }
    }

    return QString("%1:%2.%3")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(frames,  2, 10, QChar('0'));
}

// ── Отрисовка OSD ────────────────────────────────────────────────────────────
void OSD::draw(QPainter& painter, int widgetW, int widgetH)
{
    Q_UNUSED(widgetH)
    if (widgetW <= 0) return;

    // Шрифт — моноширинный, размер адаптивный
    int fontSize = std::max(9, widgetW / 120);
    QFont font("Consolas", fontSize);
    font.setStyleHint(QFont::Monospace);
    painter.setFont(font);

    int lineH = fontSize + 4;
    int x = 10;
    int y = lineH;

    // Полупрозрачный фон для читаемости
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 140));

    // Собираем строки
    QStringList lines;

    // Строка 1: Таймкод / Длительность
    QString timecode = formatTimecode(m_pts, m_fps);
    QString durStr   = formatTimecode(m_duration, m_fps);
    lines << QString("%1 / %2").arg(timecode, durStr);

    // Строка 2: Скорость (всегда показываем для информативности)
    QString speedStr;
    if (m_speed < 0.0)
        speedStr = QString("%1x").arg(m_speed, 0, 'f', 1);
    else
        speedStr = QString("%1x").arg(m_speed, 0, 'f', 1);

    // Режим колеса
    QString modeStr = (m_wheelMode == 1) ? "SHT" : "JOG";
    lines << QString("%1  %2").arg(speedStr, modeStr);

    // Строка 3: Масштаб (только если != 1.0)
    if (std::abs(m_zoom - 1.0) > 0.01) {
        lines << QString("x%1").arg(m_zoom, 0, 'f', 1);
    }

    // Рисуем фон
    int maxW = 0;
    QFontMetrics fm(font);
    for (const auto& line : lines)
        maxW = std::max(maxW, fm.horizontalAdvance(line));

    painter.drawRect(x - 4, 4, maxW + 12, static_cast<int>(lines.size()) * lineH + 4);

    // Рисуем текст
    painter.setPen(QColor(220, 220, 220));
    for (const auto& line : lines) {
        painter.drawText(x, y, line);
        y += lineH;
    }
}