#include "main_window.hpp"

#include <QString>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("LocalVault"));
    resize(1280, 720);
}
