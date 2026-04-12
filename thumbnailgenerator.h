#pragma once

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QMap>
#include <QImage>
#include <QString>
#include <atomic>

// ── Генератор превью ключевых кадров ─────────────────────────────────────────
//
// Фоновый поток, который открывает видеофайл параллельно основному декодеру
// и извлекает I-frames в низком разрешении (160×90).
//
// Кэш: QMap<int64_t, QImage> где ключ = PTS в миллисекундах.
// Lookup: ближайший keyframe к заданному PTS.
//
// Использование:
//   generator->start();
//   generator->openFile(path);
//   // ... позже:
//   QImage thumb = generator->thumbnailAt(pts);  // thread-safe
//
class ThumbnailGenerator : public QThread
{
    Q_OBJECT

public:
    explicit ThumbnailGenerator(QObject* parent = nullptr);
    ~ThumbnailGenerator() override;

    // Открыть файл для генерации превью (thread-safe, можно вызывать из main)
    void openFile(const QString& path);

    // Остановить поток
    void stopThread();

    // Получить превью ближайшего keyframe к заданному PTS (секунды).
    // Thread-safe. Возвращает null QImage если превью ещё нет.
    QImage thumbnailAt(double pts) const;

    // Прогресс генерации
    int totalKeyframes() const { return m_totalKeyframes.load(); }
    int generatedCount() const { return m_generatedCount.load(); }
    bool isReady()       const { return m_ready.load(); }

signals:
    void generationStarted(int totalKeyframes);
    void generationProgress(int current, int total);
    void generationFinished();

protected:
    void run() override;

private:
    void generateThumbnails(const QString& path);

    // ── Команды ──────────────────────────────────────────────────────────────
    QMutex         m_mutex;
    QWaitCondition m_cond;
    QString        m_pendingPath;
    std::atomic<bool> m_running{true};
    std::atomic<bool> m_abort{false};  // прерывание текущей генерации

    // ── Кэш превью ──────────────────────────────────────────────────────────
    mutable QMutex     m_cacheMutex;
    QMap<int64_t, QImage> m_cache;     // ключ = PTS в миллисекундах
    std::atomic<int>   m_totalKeyframes{0};
    std::atomic<int>   m_generatedCount{0};
    std::atomic<bool>  m_ready{false};

    // ── Размер превью ────────────────────────────────────────────────────────
    static constexpr int THUMB_W = 160;
    static constexpr int THUMB_H = 90;
};