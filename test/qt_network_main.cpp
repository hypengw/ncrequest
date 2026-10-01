import rstd.test;
#include <QCoreApplication>

import ncrequest;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ncrequest::global_init();

    return rstd::test::run_registered().to_primitive();
}
