#if LITO_FEAT_QT
#    include <QCoreApplication>
#endif
import rstd.test;
auto main(int argc, char** argv) -> int {
#if LITO_FEAT_QT
    QCoreApplication application(argc, argv);
#endif
    return rstd::test::run_registered().to_primitive();
}
