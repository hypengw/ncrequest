#include <gtest/gtest.h>

import ncrequest;

int main(int, char**) {
    testing::InitGoogleTest();
    ncrequest::global_init();

    return RUN_ALL_TESTS();
}