#include "app/pages/aa.hpp"
#include <QVBoxLayout>
#include <QFile>
#include <QSvgWidget>
#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QDebug>
#include <QWindow>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

static constexpr int DHU_W = 800;
static constexpr int DHU_H = 480;

static unsigned long getWindowPID(Display *display, Window w)
{
    Atom atom = XInternAtom(display, "_NET_WM_PID", True);
    if (atom == None) return 0;

    Atom actualType;
    int format;
    unsigned long nitems, bytesAfter;
    unsigned char *prop = nullptr;

    if (XGetWindowProperty(display, w, atom, 0, 1, False, AnyPropertyType, &actualType, &format, &nitems, &bytesAfter, &prop) != Success) {
        return 0;
    }

    unsigned long pid = 0;
    if (prop) {
        pid = *(unsigned long*)prop;
        XFree(prop);
    }

    return pid;
}

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

        result = findDHUWindowRecursive(display, targetPid, children[i]);
        if (result)
            break;
    }

    if (children)
        XFree(children);

    return result;
}

static Window findDHUWindow(Display *display, unsigned long targetPid) {
    return findDHUWindowRecursive(display, targetPid, DefaultRootWindow(display));
}

AAPage::AAPage(Arbiter &arbiter)
    : Page(arbiter, "AA", "directions_car", false, new QWidget())
{
    QWidget *root = this->container()->take();

    auto layout = new QVBoxLayout(root);
    layout->setAlignment(Qt::AlignCenter);

    QFile file(":/graphics/dc.svg");
    file.open(QIODevice::ReadOnly);
    QString svgData = file.readAll();

    QString color = "#ffffff"; // TODO: adjust to dark/light mode
    svgData.replace("currentColor", color);

    auto svg = new QSvgWidget();
    svg->load(svgData.toUtf8());

    layout->addWidget(svg, 0, Qt::AlignCenter);

    this->container()->reset();

    QTimer::singleShot(3000, [=]() {
        QString dhuPath = QDir::homePath()
            + "/Android/Sdk/extras/google/auto/desktop-head-unit";

        auto proc = new QProcess(root);
        proc->start(dhuPath, {"-u"});

        if (!proc->waitForStarted(3000)) {
            qWarning() << "DHU failed to start";
            return;
        }

        unsigned long pid = proc->processId();

        QTimer *poll = new QTimer(root);
        poll->setInterval(300);

        QObject::connect(poll, &QTimer::timeout, [=]() mutable {
            Display *display = XOpenDisplay(nullptr);
            if (!display) {
                qWarning() << "display not available";
                return;
            }

            Window win = findDHUWindow(display, pid);
            if (!win) {
                XCloseDisplay(display);
                return;
            }

            poll->stop();
            poll->deleteLater();

            Atom hints = XInternAtom(display, "_MOTIF_WM_HINTS", False);
            struct {
                unsigned long flags, functions, decorations;
                long input_mode;
                unsigned long status;
            } mwmHints = {2, 0, 0, 0, 0};
            XChangeProperty(display, win, hints, hints, 32, PropModeReplace, (unsigned char *)&mwmHints, 5);

            XUnmapWindow(display, win);
            XFlush(display);

            XReparentWindow(display, win, (Window)root->winId(), 0, 0);
            XResizeWindow(display, win, DHU_W, DHU_H);
            XMapWindow(display, win);
            XFlush(display);
            XCloseDisplay(display);

            QWindow *external = QWindow::fromWinId(win);
            QWidget *container = QWidget::createWindowContainer(external, root);
            container->setFixedSize(DHU_W, DHU_H);
            container->setFocusPolicy(Qt::NoFocus);
            layout->addWidget(container, 0, Qt::AlignCenter);

            svg->deleteLater();
        });

        poll->start();
    });
}

void AAPage::init() {}
