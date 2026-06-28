#include <gtest/gtest.h>
#include <QCoreApplication>

import ncrequest;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    ncrequest::global_init();

    return RUN_ALL_TESTS();
}
