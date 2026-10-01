#include <QCoreApplication>
import rstd.test;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return rstd::test::run_registered().to_primitive();
}
