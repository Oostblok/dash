#pragma once

#include <QtWidgets>
#include <QStackedWidget>
#include <QProcess>
#include <QVBoxLayout>
#include <QtSvg/QSvgWidget>
#include <functional>
#include <thread>
#include <vector>

#include <libusb-1.0/libusb.h>
#include <boost/asio.hpp>

#include "aasdk/USB/USBWrapper.hpp"
#include "aasdk/USB/AccessoryModeQueryFactory.hpp"
#include "aasdk/USB/AccessoryModeQueryChainFactory.hpp"
#include "aasdk/USB/ConnectedAccessoriesEnumerator.hpp"
#include "aasdk/USB/USBHub.hpp"
#include "aasdk/TCP/TCPWrapper.hpp"
#include "openauto/App.hpp"
#include "openauto/Service/IAndroidAutoEntityFactory.hpp"
#include "openauto/Service/IAndroidAutoEntity.hpp"

#include "app/pages/page.hpp"
#include "app/session.hpp"

class Arbiter;

class DummyAndroidAutoEntity : public openauto::service::IAndroidAutoEntity {
public:
    void start(openauto::service::IAndroidAutoEntityEventHandler&) override {}
    void stop() override {}
};

class DummyAndroidAutoEntityFactory : public openauto::service::IAndroidAutoEntityFactory {
public:
    openauto::service::IAndroidAutoEntity::Pointer create(aasdk::usb::IAOAPDevice::Pointer) override {
        return std::make_shared<DummyAndroidAutoEntity>();
    }
    openauto::service::IAndroidAutoEntity::Pointer create(aasdk::tcp::ITCPEndpoint::Pointer) override {
        return std::make_shared<DummyAndroidAutoEntity>();
    }
};

class AAWorker : public QObject {
    Q_OBJECT

public:
    AAWorker(std::function<void(bool)> callback, QObject *parent = nullptr);
    ~AAWorker();
    void waitForDevice();

private:
    void create_usb_workers();
    void create_io_service_workers();

    std::function<void(bool)> callback;
    libusb_context *usb_context = nullptr;
    boost::asio::io_service io_service;
    boost::asio::io_service::work work;
    aasdk::tcp::TCPWrapper tcp_wrapper;
    aasdk::usb::USBWrapper usb_wrapper;
    aasdk::usb::AccessoryModeQueryFactory query_factory;
    aasdk::usb::AccessoryModeQueryChainFactory query_chain_factory;
    std::shared_ptr<aasdk::usb::USBHub> usb_hub;
    std::shared_ptr<aasdk::usb::ConnectedAccessoriesEnumerator> connected_accessories_enumerator;
    boost::asio::io_service::strand strand_;
    std::vector<std::thread> thread_pool;
};

class DHUPage : public QStackedWidget, public Page {
    Q_OBJECT

public:
    DHUPage(Arbiter &arbiter);
    void init() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void launchDHU(QWidget *root, QVBoxLayout *layout, QLabel *loader);
    void killDHU();

    AAWorker *worker = nullptr;
    QProcess *dhuProcess = nullptr;
    QWidget *dhuContainerWidget = nullptr;
    QSvgWidget *logo = nullptr;
    QByteArray loadSvg(Session::Theme::Mode mode);
};

