import rstd.test;
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
#    include <QCoreApplication>
#endif

import ncrequest;

int main(int argc, char** argv) {
#ifdef NCREQUEST_CLIENT_BACKEND_QT_NETWORK
    QCoreApplication app(argc, argv);
#endif
    ncrequest::global_init();

    return rstd::test::run_registered().to_primitive();
}
