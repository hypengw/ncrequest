#include <gtest/gtest.h>

import ncrequest;

int main(int argc, char** argv) {
    ncrequest::global_init();

    return RUN_ALL_TESTS();
}