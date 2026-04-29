#include "app/pages/dhu.hpp"
#include <QVBoxLayout>
#include <QLabel>
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

    if (XGetWindowProperty(display, w, atom, 0, 1, False, AnyPropertyType, &actualType, &format, &nitems, &bytesAfter, &prop) != Success)
        return 0;

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

static Window findDHUWindow(Display *display, unsigned long targetPid)
{
    return findDHUWindowRecursive(display, targetPid, DefaultRootWindow(display));
}

AAWorker::AAWorker(std::function<void(bool)> callback, QObject *parent)
    : QObject(parent)
    , callback(callback)
    , work(io_service)
    , usb_wrapper((libusb_init(&usb_context), usb_context))
    , query_factory(usb_wrapper, io_service)
    , query_chain_factory(usb_wrapper, io_service, query_factory)
    , usb_hub(std::make_shared<aasdk::usb::USBHub>(usb_wrapper, io_service, query_chain_factory))
    , connected_accessories_enumerator(std::make_shared<aasdk::usb::ConnectedAccessoriesEnumerator>(usb_wrapper, io_service, query_chain_factory))
    , strand_(io_service)
{
    this->create_usb_workers();
    this->create_io_service_workers();
    this->waitForDevice();
}

AAWorker::~AAWorker()
{
    this->usb_hub->cancel();
    this->io_service.stop();
    std::for_each(this->thread_pool.begin(), this->thread_pool.end(), std::bind(&std::thread::join, std::placeholders::_1));
    libusb_exit(this->usb_context);
}

void AAWorker::create_usb_workers()
{
    std::function<void()> worker = [this]() {
        timeval event_timeout = {180, 0};
        while (!this->io_service.stopped())
            libusb_handle_events_timeout_completed(this->usb_context, &event_timeout, nullptr);
    };

    this->thread_pool.emplace_back(worker);
    this->thread_pool.emplace_back(worker);
    this->thread_pool.emplace_back(worker);
    this->thread_pool.emplace_back(worker);
}

void AAWorker::create_io_service_workers()
{
    std::function<void()> worker = [this]() { this->io_service.run(); };

    this->thread_pool.emplace_back(worker);
    this->thread_pool.emplace_back(worker);
    this->thread_pool.emplace_back(worker);
    this->thread_pool.emplace_back(worker);
}

void AAWorker::waitForDevice()
{
    auto promise = aasdk::usb::IUSBHub::Promise::defer(strand_);
    promise->then(
        [this](aasdk::usb::DeviceHandle) {
            QMetaObject::invokeMethod(this, [this]() {
                this->callback(true);
            }, Qt::QueuedConnection);
        },
        [this](const aasdk::error::Error& error) {
            if (error != aasdk::error::ErrorCode::OPERATION_ABORTED)
                this->waitForDevice();
        }
    );
    this->usb_hub->start(std::move(promise));
}

// ---- DHUPage ----

DHUPage::DHUPage(Arbiter &arbiter)
    : QStackedWidget()
    , Page(arbiter, "AA", "directions_car", false, this)
{
}

void DHUPage::init()
{
    QWidget *waiting = new QWidget(this);
    QVBoxLayout *waitingLayout = new QVBoxLayout(waiting);
    waitingLayout->setAlignment(Qt::AlignCenter);

    QFile file(":/graphics/dc.svg");
    file.open(QIODevice::ReadOnly);
    QString svgData = file.readAll();
    QString color = "#ffffff"; // TODO: adjust to dark/light mode from Session
    svgData.replace("currentColor", color);

    auto logo = new QSvgWidget(waiting);
    logo->load(svgData.toUtf8());
    logo->setFixedSize(128, 88); // TODO: make size dynamic
    waitingLayout->addWidget(logo, 0, Qt::AlignCenter);

    waitingLayout->addSpacing(12);

    QLabel *connectLabel = new QLabel("Connect your phone to start Android Auto", waiting);
    connectLabel->setAlignment(Qt::AlignCenter);
    waitingLayout->addWidget(connectLabel, 0, Qt::AlignCenter);

    this->addWidget(waiting);

    QWidget *dhuContainer = new QWidget(this);
    QVBoxLayout *dhuLayout = new QVBoxLayout(dhuContainer);
    dhuLayout->setAlignment(Qt::AlignCenter);

    // TODO: animate "blocks" in DC logo --> svg css animation by class?
    QLabel *loader = new QLabel("Loading...", dhuContainer);
    loader->setAlignment(Qt::AlignCenter);
    dhuLayout->addWidget(loader, 0, Qt::AlignCenter);

    this->addWidget(dhuContainer);
    this->setCurrentIndex(0);

    std::function<void(bool)> callback = [this, dhuContainer, dhuLayout, loader](bool connected) {
        if (connected) {
            this->setCurrentIndex(1);
            this->launchDHU(dhuContainer, dhuLayout, loader);
        } else {
            this->killDHU();
            this->setCurrentIndex(0);
        }
    };

    this->worker = new AAWorker(callback, this);
}

void DHUPage::launchDHU(QWidget *root, QVBoxLayout *layout, QLabel *loader)
{
    QString dhuPath = QDir::homePath() + "/Android/Sdk/extras/google/auto/desktop-head-unit";

    this->dhuProcess = new QProcess(root);
    this->dhuProcess->start(dhuPath, {"-u"});

    if (!this->dhuProcess->waitForStarted(3000)) {
        qWarning() << "DHU failed to start";
        return;
    }

    unsigned long pid = this->dhuProcess->processId();

    QTimer *poll = new QTimer(root);
    poll->setInterval(300);

    QObject::connect(poll, &QTimer::timeout, [=]() mutable {
        Display *display = XOpenDisplay(nullptr);
        if (!display) return;

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

        loader->deleteLater();
    });

    poll->start();
}

void DHUPage::killDHU()
{
    if (this->dhuProcess) {
        this->dhuProcess->terminate();
        this->dhuProcess->waitForFinished(2000);
        this->dhuProcess = nullptr;
    }
}
