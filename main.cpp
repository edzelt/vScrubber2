#include <QApplication>
#include <QSurfaceFormat>
#include "mainwindow.h"

int main(int argc, char* argv[])
{
    // ── OpenGL 4.5 Core ──────────────────────────────────────────────────────
    QSurfaceFormat fmt;
    fmt.setVersion(4, 5);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapInterval(1);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("vScrubber2");

    MainWindow window;
    window.show();

    // Открытие файла из командной строки
    if (argc > 1)
        window.openFile(QString::fromLocal8Bit(argv[1]));

    return app.exec();
}