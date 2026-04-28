#pragma once

#include <QLabel>
#include <QVBoxLayout>

#include "app/pages/page.hpp"

class AAPage : public Page {
public:
    AAPage(Arbiter &arbiter);
    void init() override;

private:
    QLabel *label_;
};
