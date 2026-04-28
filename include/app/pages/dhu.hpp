#pragma once

#include <QWidget>
#include <QPointer>

#include "app/pages/page.hpp"

class DHUPage : public Page {
public:
    DHUPage(Arbiter &arbiter);
    void init() override;

private:
    QWidget *root_ = nullptr;
    QWidget *dhuContainer_ = nullptr;
};
