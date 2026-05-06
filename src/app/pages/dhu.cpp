#include "app/pages/dhu.hpp"
#include "app/arbiter.hpp"

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

static QString dhuConfigPath()
{
    return QFileInfo(QSettings().fileName()).dir().filePath("dhu.ini");
}

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

AAWorker::AAWorker(std::function<void(bool, QString)> callback, QObject *parent)
    : QObject(parent)
    , callback(callback)
    , work(io_service)
    , usb_wrapper((libusb_init(&usb_context), usb_context))
    , query_factory(usb_wrapper, io_service)
    , query_chain_factory(usb_wrapper, io_service, query_factory)
    , usb_hub(std::make_shared<aasdk::usb::USBHub>(usb_wrapper, io_service, query_chain_factory))
    , connected_accessories_enumerator(std::make_shared<aasdk::usb::ConnectedAccessoriesEnumerator>(usb_wrapper, io_service, query_chain_factory))
    , strand_(io_service)
    , acceptor_(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 5000))
{
    this->create_usb_workers();
    this->create_io_service_workers();
    this->waitForDevice();
}

AAWorker::~AAWorker()
{
    this->acceptor_.close();
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
    this->waitForUSBDevice();
    this->waitForWirelessDevice();
}

void AAWorker::waitForUSBDevice()
{
    auto promise = aasdk::usb::IUSBHub::Promise::defer(strand_);
    promise->then(
        [this](aasdk::usb::DeviceHandle deviceHandle) {
            this->acceptor_.cancel();

            libusb_device *dev = libusb_get_device(deviceHandle.get());
            libusb_device_descriptor desc;
            libusb_get_device_descriptor(dev, &desc);

            QString serial;
            libusb_device_handle *handle;
            if (libusb_open(dev, &handle) == 0) {
                unsigned char buf[256];
                libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, buf, sizeof(buf));
                serial = QString((char*)buf);
                libusb_close(handle);
            }

            QMetaObject::invokeMethod(this, [this, serial]() {
                this->callback(true, serial);
            }, Qt::QueuedConnection);
        },
        [this](const aasdk::error::Error& error) {
            if (error != aasdk::error::ErrorCode::OPERATION_ABORTED)
                this->waitForDevice();
        }
    );
    this->usb_hub->start(std::move(promise));
}

void AAWorker::waitForWirelessDevice()
{
    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_service);
    acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code &ec) {
        if (!ec) {
            this->acceptor_.cancel();
            QMetaObject::invokeMethod(this, [this]() {
                this->callback(true, "wireless");
            }, Qt::QueuedConnection);
        } else if (ec != boost::asio::error::operation_aborted) {
            this->waitForDevice();
        }
    });
}

DHUPage::DHUPage(Arbiter &arbiter)
    : QStackedWidget()
    , Page(arbiter, "DHU", "android_auto", false, this)
{
}

QByteArray DHUPage::loadSvg(Session::Theme::Mode mode)
{
    QFile file(":/graphics/dc.svg");
    file.open(QIODevice::ReadOnly);
    QString svgData = file.readAll();
    QString color = mode == Session::Theme::Dark ? "#ffffff" : "#000000";
    svgData.replace("currentColor", color);
    return svgData.toUtf8();
}

void DHUPage::init()
{
    this->arbiter.dhu().page = this;

    connect(&this->arbiter, &Arbiter::curr_page_changed, [this](Page *page) {
        if (this->dhuProcess && this->dhuProcess->state() == QProcess::Running) {
            if (page == this)
                this->dhuProcess->write("focus video on\n");
            else
                this->dhuProcess->write("focus video off\n");
        }
    });

    QWidget *waiting = new QWidget(this);
    waiting->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QVBoxLayout *waitingLayout = new QVBoxLayout(waiting);
    waitingLayout->setContentsMargins(0, 0, 0, 0);
    waitingLayout->setSpacing(0);

    Dialog *dialog = new Dialog(this->arbiter, true, this->window());
    Settings *settingsWidget = new Settings();
    dialog->set_body(settingsWidget);
    QPushButton *save_button = new QPushButton("save");
    connect(save_button, &QPushButton::clicked, [settingsWidget]() {
        settingsWidget->save();
    });
    dialog->set_button(save_button);

    QPushButton *settings_button = new QPushButton(waiting);
    settings_button->setFlat(true);
    this->arbiter.forge().iconize("settings", settings_button, 24);
    connect(settings_button, &QPushButton::clicked, [dialog]() { dialog->open(); });

    QHBoxLayout *topBar = new QHBoxLayout();
    topBar->setContentsMargins(4, 4, 4, 4);
    topBar->addStretch();
    topBar->addWidget(settings_button);
    waitingLayout->addLayout(topBar);

    this->logo = new QSvgWidget(waiting);
    this->logo->load(this->loadSvg(this->arbiter.theme().mode));

    waitingLayout->addStretch();
    waitingLayout->addWidget(this->logo, 0, Qt::AlignCenter);
    waitingLayout->addSpacing(24);

    QLabel *connectLabel = new QLabel("Connect your phone to start Android Auto", waiting);
    connectLabel->setAlignment(Qt::AlignCenter);
    waitingLayout->addWidget(connectLabel, 0, Qt::AlignCenter);
    waitingLayout->addStretch();

    this->addWidget(waiting);

    connect(&this->arbiter, &Arbiter::mode_changed, [this](Session::Theme::Mode mode) {
        this->logo->load(this->loadSvg(mode));

        if (this->dhuProcess && this->dhuProcess->state() == QProcess::Running)
            this->dhuProcess->write((mode == Session::Theme::Dark ? "night" : "day") + QString("\n").toUtf8());
    });

    QWidget *dhuContainer = new QWidget(this);
    QVBoxLayout *dhuLayout = new QVBoxLayout(dhuContainer);
    dhuLayout->setAlignment(Qt::AlignCenter);
    dhuLayout->setContentsMargins(0, 0, 0, 0);
    dhuLayout->setSpacing(0);

    QLabel *loader = new QLabel("Loading...", dhuContainer);
    loader->setAlignment(Qt::AlignCenter);
    dhuLayout->addWidget(loader, 0, Qt::AlignCenter);

    this->addWidget(dhuContainer);
    this->setCurrentIndex(0);

    std::function<void(bool, QString)> callback = [this, dhuContainer, dhuLayout, loader](bool, QString serial) {
        this->arbiter.dhu().connected = true;
        this->setCurrentIndex(1);
        this->launchDHU(dhuContainer, dhuLayout, loader, serial);

        auto icon = this->button()->icon();
        icon.addFile(QString(":/icons/android_auto_color.svg"), QSize(), QIcon::Active, QIcon::On);
        this->button()->setIcon(icon);
    };

    this->worker = new AAWorker(callback, this);
}

void DHUPage::launchDHU(QWidget *root, QVBoxLayout *layout, QLabel *loader, const QString &serial)
{
    loader->show();

    QSettings cfg(dhuConfigPath(), QSettings::IniFormat);
    QString resolution = cfg.value("resolution").toString();
    QStringList parts = resolution.split("x");
    if (parts.size() == 2) {
        bool ok1, ok2;
        double w = parts[0].toDouble(&ok1);
        double h = parts[1].toDouble(&ok2);
        if (ok1 && ok2 && h > 0)
            this->aspectRatio = w / h;
    }

    // TODO: add desktop-head-unit to the install script and update the path
    QString dhuPath = QDir::homePath() + "/Android/Sdk/extras/google/auto/desktop-head-unit";

    this->dhuProcess = new QProcess(root);
    QStringList args;
    if (serial == "wireless")
        args << "--adb=5000";
    else
        args << "--usb=" + serial;
    args << "--config=" + dhuConfigPath();
    this->dhuProcess->start(dhuPath, args);

    if (!this->dhuProcess->waitForStarted(3000)) {
        qWarning() << "DHU failed to start";
        return;
    }

    QObject::connect(this->dhuProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), [this](int code, QProcess::ExitStatus status) {
        qDebug() << "[DHU] process exited, code:" << code << "status:" << status;

        this->dhuProcess = nullptr;
        this->dhuContainerWidget = nullptr;
        this->arbiter.dhu().connected = false;
        this->setCurrentIndex(0);

        auto icon = this->button()->icon();
        icon.addFile(QString(":/icons/android_auto.svg"), QSize(), QIcon::Active, QIcon::Off);
        this->button()->setIcon(icon);

        this->worker->waitForDevice();
    });

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
        XResizeWindow(display, win, root->width(), root->height());
        XMapWindow(display, win);
        XFlush(display);
        XCloseDisplay(display);

        QWindow *external = QWindow::fromWinId(win);
        this->dhuContainerWidget = QWidget::createWindowContainer(external, root);
        this->dhuContainerWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        this->dhuContainerWidget->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(this->dhuContainerWidget);

        QTimer::singleShot(0, this, [this]() {
            fitDHUToAspectRatio();
        });

        loader->hide();
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

void DHUPage::resizeEvent(QResizeEvent *event)
{
    QStackedWidget::resizeEvent(event);

    if (logo) {
        int logoH = event->size().height() * 0.4;
        int logoW = logoH * (886.24 / 609.4);
        logo->setFixedSize(logoW, logoH);
    }

    if (this->dhuContainerWidget) {
        fitDHUToAspectRatio();
    }
}
void DHUPage::fitDHUToAspectRatio()
{
    if (!this->dhuContainerWidget)
        return;

    bool fitToScreen = QSettings().value("DHU/fitToScreen", false).toBool();

    if (fitToScreen) {
        this->dhuContainerWidget->setFixedSize(this->width(), this->height());
    } else {
        int h = this->height();
        this->dhuContainerWidget->setFixedWidth(static_cast<int>(h * this->aspectRatio));
    }
}

DHUPage::Settings::Settings(QWidget *parent)
    : QWidget(parent)
{
    QFile file(dhuConfigPath());
    if (!file.exists()) {
        this->config.resolution = "800x480";
        this->config.dpi = 160;
        this->config.inputMode = "default";
        this->save();
    } else {
        QSettings cfg(dhuConfigPath(), QSettings::IniFormat);
        this->config.resolution = cfg.value("resolution", "800x480").toString();
        this->config.dpi = cfg.value("dpi", 160).toInt();
        this->config.inputMode = cfg.value("inputmode", "default").toString();
    }

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(this->settings_widget());
}

void DHUPage::Settings::save()
{
    QDir().mkpath(QFileInfo(dhuConfigPath()).dir().absolutePath());
    QFile file(dhuConfigPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << "[general]\n";
        out << "resolution = " << this->config.resolution << "\n";
        out << "dpi = " << this->config.dpi << "\n";
        out << "inputmode = " << this->config.inputMode << "\n";
        file.close();
    }
}

QLayout *DHUPage::Settings::settings_widget()
{
    QVBoxLayout *layout = new QVBoxLayout();
    layout->addLayout(this->resolution_row_widget(), 1);
    layout->addWidget(Session::Forge::br(), 1);
    layout->addLayout(this->dpi_row_widget(), 1);
    layout->addWidget(Session::Forge::br(), 1);
    layout->addLayout(this->inputmode_row_widget(), 1);
    layout->addWidget(Session::Forge::br(), 1);
    layout->addLayout(this->fit_to_screen_row_widget(), 1);

    return layout;
}

QLayout *DHUPage::Settings::resolution_row_widget()
{
    QHBoxLayout *layout = new QHBoxLayout();

    QLabel *label = new QLabel("Resolution");
    layout->addWidget(label, 1);

    QGroupBox *group = new QGroupBox();
    QVBoxLayout *group_layout = new QVBoxLayout(group);

    QRadioButton *r480 = new QRadioButton("480p (800x480)", group);
    r480->setChecked(this->config.resolution == "800x480");
    group_layout->addWidget(r480);

    QRadioButton *r720 = new QRadioButton("720p (1280x720)", group);
    r720->setChecked(this->config.resolution == "1280x720");
    group_layout->addWidget(r720);

    QRadioButton *r1080 = new QRadioButton("1080p (1920x1080)", group);
    r1080->setChecked(this->config.resolution == "1920x1080");
    group_layout->addWidget(r1080);

    connect(r480, &QRadioButton::clicked, [this]() { this->config.resolution = "800x480"; });
    connect(r720, &QRadioButton::clicked, [this]() { this->config.resolution = "1280x720"; });
    connect(r1080, &QRadioButton::clicked, [this]() { this->config.resolution = "1920x1080"; });

    layout->addWidget(group, 1, Qt::AlignHCenter);

    return layout;
}

QLayout *DHUPage::Settings::dpi_row_widget()
{
    QHBoxLayout *layout = new QHBoxLayout();

    QLabel *label = new QLabel("DPI");
    layout->addWidget(label, 1);


    QHBoxLayout *inner = new QHBoxLayout();
    QSlider *slider = new QSlider(Qt::Horizontal);
    slider->setTracking(false);
    slider->setRange(100, 320);
    slider->setValue(this->config.dpi);

    QLabel *value = new QLabel(QString::number(this->config.dpi));
    connect(slider, &QSlider::valueChanged, [this, value](int v) {
        value->setText(QString::number(v));
        this->config.dpi = v;
    });

    inner->addStretch(2);
    inner->addWidget(slider, 4);
    inner->addWidget(value, 2);

    layout->addLayout(inner, 1);

    return layout;
}

QLayout *DHUPage::Settings::inputmode_row_widget()
{
    QHBoxLayout *layout = new QHBoxLayout();

    QLabel *label = new QLabel("Input Mode");
    layout->addWidget(label, 1);

    QGroupBox *group = new QGroupBox();
    QVBoxLayout *group_layout = new QVBoxLayout(group);

    for (const QString mode : {"default", "touch", "rotary", "hybrid"}) {
        QRadioButton *btn = new QRadioButton(mode, group);
        btn->setChecked(this->config.inputMode == mode);
        connect(btn, &QRadioButton::clicked, [this, mode]() {
            this->config.inputMode = mode;
        });
        group_layout->addWidget(btn);
    }

    layout->addWidget(group, 1, Qt::AlignHCenter);

    return layout;
}

QLayout *DHUPage::Settings::fit_to_screen_row_widget()
{
    QHBoxLayout *layout = new QHBoxLayout();

    QLabel *label = new QLabel("Fit to Screen");
    layout->addWidget(label, 1);

    Switch *toggle = new Switch();
    toggle->setChecked(QSettings().value("DHU/fitToScreen", false).toBool());
    connect(toggle, &Switch::stateChanged, [](bool state) {
        QSettings().setValue("DHU/fitToScreen", state);
    });
    layout->addWidget(toggle, 1, Qt::AlignHCenter);

    return layout;
}

void DHUPage::sendKey(const QString &key)
{
    if (this->dhuProcess && this->dhuProcess->state() == QProcess::Running)
        this->dhuProcess->write(("keycode " + key + "\n").toUtf8());
}
