#include "app/pages/aa.hpp"

#include <QSvgWidget>
#include <QVBoxLayout>
#include <QFile>
#include <QProcess>
#include <QWindow>

// For testing/debugging
#include <QDebug>
#include <QTimer>
#include <QDir>

AAPage::AAPage(Arbiter &arbiter)
    : Page(arbiter, "AA", "directions_car", false, new QWidget())
{
    auto root = this->container()->take();
    auto layout = new QVBoxLayout(root);

    QFile file(":/graphics/dc.svg");
    file.open(QIODevice::ReadOnly);
    QString svgData = file.readAll();

    QString color = "#ffffff"; // TODO: adjust to dark/light mode
    svgData.replace("currentColor", color);

    auto svg = new QSvgWidget();
    svg->load(svgData.toUtf8());

    layout->addWidget(svg, 0, Qt::AlignCenter);

    this->container()->reset();

    // ---------------------------
    // START DHU AFTER 10 SECONDS
    // ---------------------------
    QTimer::singleShot(10000, [this, layout, svg]() {
        QString dhuPath = QDir::homePath() + "/Android/Sdk/extras/google/auto/desktop-head-unit";

        auto dhuProcess = new QProcess();
        dhuProcess->setParent(nullptr);
        dhuProcess->start(dhuPath, QStringList() << "-u -fullscreen");

        if (!dhuProcess->waitForStarted(3000)) {
            qWarning() << "Failed to start DHU";
            return;
        }

        qint64 pid = dhuProcess->processId();
        qDebug() << "DHU process started with PID:" << pid;

        QTimer::singleShot(2000, [layout, svg, pid]() {
            QProcess find;
            find.start("xdotool", QStringList()
                << "search"
                << "--pid"
                << QString::number(pid));

            find.waitForFinished();

            QString output = find.readAllStandardOutput().trimmed();
            if (output.isEmpty()) {
                qWarning() << "DHU window not found";
                return;
            }


            WId winId = output.split('\n').first().toULongLong();

            qDebug() << "DHU window found:" << windId;

            QWindow *win = QWindow::fromWinId(winId);
            win->setFlags(Qt::FramelessWindowHint | Qt::CustomizeWindowHint);
            QWidget *container = QWidget::createWindowContainer(win);

            layout->removeWidget(svg);
            svg->deleteLater();

            layout->addWidget(container);
        });
    });
}

void AAPage::init() {}
