#include "app/pages/aa.hpp"

#include <QVBoxLayout>
#include <QFile>
#include <QSvgWidget>
#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QDebug>
#include <QWindow>
#include <QEvent>

// X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>

static constexpr int DHU_W = 800;
static constexpr int DHU_H = 480;

// -----------------------------------------------------
// helper: read _NET_WM_PID
// -----------------------------------------------------
static unsigned long getWindowPID(Display *display, Window w)
{
    Atom atom = XInternAtom(display, "_NET_WM_PID", True);
    if (atom == None) return 0;

    Atom actualType;
    int format;
    unsigned long nitems, bytesAfter;
    unsigned char *prop = nullptr;

    if (XGetWindowProperty(display, w,
                           atom, 0, 1, False,
                           AnyPropertyType,
                           &actualType, &format,
                           &nitems, &bytesAfter,
                           &prop) != Success)
        return 0;

    unsigned long pid = 0;
    if (prop) {
        pid = *(unsigned long*)prop;
        XFree(prop);
    }

    return pid;
}

// -----------------------------------------------------
// helper: find DHU window (recursive)
// -----------------------------------------------------
static Window findDHUWindow(Display *display, unsigned long targetPid);

// private recursive helper
static Window findDHUWindowRecursive(Display *display, unsigned long targetPid, Window root)
{
    Window rootReturn, parent;
    Window *children = nullptr;
    unsigned int count = 0;

    if (!XQueryTree(display, root, &rootReturn, &parent, &children, &count))
        return 0;

    Window result = 0;

    for (unsigned int i = 0; i < count; i++) {

        XWindowAttributes attr;
        if (!XGetWindowAttributes(display, children[i], &attr))
            continue;

        if (attr.map_state != IsViewable)
            continue;

        if (attr.width < 200 || attr.height < 200)
            continue;

        unsigned long pid = getWindowPID(display, children[i]);

        if (pid == targetPid) {
            result = children[i];
            break;
        }

        // recursively search children
        result = findDHUWindowRecursive(display, targetPid, children[i]);
        if (result)
            break;
    }

    if (children)
        XFree(children);

    return result;
}

// public entry point (original call still works)
static Window findDHUWindow(Display *display, unsigned long targetPid)
{
    Window root = DefaultRootWindow(display);
    return findDHUWindowRecursive(display, targetPid, root);
}

// -----------------------------------------------------
// constructor
// -----------------------------------------------------
AAPage::AAPage(Arbiter &arbiter)
    : Page(arbiter, "AA", "directions_car", false, new QWidget())
{
    QWidget *root = this->container()->take();

    auto layout = new QVBoxLayout(root);
    layout->setAlignment(Qt::AlignCenter);

    // placeholder UI
    QFile file(":/graphics/dc.svg");
    file.open(QIODevice::ReadOnly);
    QString svgData = file.readAll();

    QString color = "#ffffff"; // TODO: adjust to dark/light mode
    svgData.replace("currentColor", color);

    auto svg = new QSvgWidget();
    svg->load(svgData.toUtf8());

    layout->addWidget(svg, 0, Qt::AlignCenter);

    this->container()->reset();

    // -------------------------------------------------
    // start DHU
    // -------------------------------------------------
    QTimer::singleShot(3000, [=]() {
        QString dhuPath = QDir::homePath()
            + "/Android/Sdk/extras/google/auto/desktop-head-unit";

        auto proc = new QProcess();
        proc->setParent(nullptr);

        proc->start(dhuPath, {"-u"});

        if (!proc->waitForStarted(3000)) {
            qWarning() << "DHU failed to start";
            return;
        }

        unsigned long pid = proc->processId();
        qDebug() << "DHU PID:" << pid;

        // -------------------------------------------------
        // poll X11 until window exists
        // -------------------------------------------------
        QTimer *poll = new QTimer();
        poll->setInterval(300);

        QObject::connect(poll, &QTimer::timeout, [=]() mutable {
            Display *display = XOpenDisplay(nullptr);
            if (!display) {
                qWarning() << "X11 display not available";
                return;
            }

            Window win = findDHUWindow(display, pid);
            if (!win) {
                XCloseDisplay(display);
                return; // keep polling
            }

            // stop polling
            poll->stop();
            poll->deleteLater();

            qDebug() << "DHU window found:" << win;

            // -------------------------------------------------
            // embed via X11
            // -------------------------------------------------
            Window parent = (Window)root->winId();

            XReparentWindow(display, win, parent, 0, 0);
            XResizeWindow(display, win, DHU_W, DHU_H);
            XMapWindow(display, win);
            XFlush(display);

            qDebug() << "root->winId():" << root->winId();
            qDebug() << "win (DHU window):" << win;
            qDebug() << "root->isVisible():" << root->isVisible();

            // -------------------------------------------------
            // Qt wrapper
            // -------------------------------------------------
            QWindow *external = QWindow::fromWinId(win);

            QWidget *container =
                QWidget::createWindowContainer(external, root);

            container->setFixedSize(DHU_W, DHU_H);

            layout->addWidget(container, 0, Qt::AlignCenter);

            svg->deleteLater();

            XCloseDisplay(display);
        });

        poll->start();
    });
}

// -----------------------------------------------------
void AAPage::init() {}
