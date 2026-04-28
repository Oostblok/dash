#pragma once

#include <QWidget>
#include <QPointer>

#include "app/pages/page.hpp"

class AAPage : public Page {
public:
    AAPage(Arbiter &arbiter);
    void init() override;

private:
    QWidget *root_ = nullptr;
    QWidget *dhuContainer_ = nullptr;
};
