import rstd.test;
#ifdef LITO_FEAT_QT
#    include <QCoreApplication>
#endif

import ncrequest;

int main(int argc, char** argv) {
#ifdef LITO_FEAT_QT
    QCoreApplication app(argc, argv);
#endif
    ncrequest::global_init();

    return rstd::test::run_registered().to_primitive();
}
