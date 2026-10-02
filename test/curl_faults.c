#define _GNU_SOURCE
#include <curl/curl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

static int should_fail(const char* operation) {
    static int failed;
    const char* selected = getenv("NCREQUEST_TEST_CURL_FAILURE");
    if (selected && strcmp(selected, "remove-always") == 0 && strcmp(operation, "remove") == 0)
        return 1;
    if (!failed && selected && strcmp(selected, operation) == 0) {
        failed = 1;
        return 1;
    }
    return 0;
}

CURLSH* curl_share_init(void) {
    if (should_fail("share-init")) return NULL;
    CURLSH* (*real)(void) = dlsym(RTLD_NEXT, "curl_share_init");
    return real();
}

CURLMcode curl_multi_perform(CURLM* multi, int* running) {
    if (should_fail("perform")) return CURLM_INTERNAL_ERROR;
    CURLMcode (*real)(CURLM*, int*) = dlsym(RTLD_NEXT, "curl_multi_perform");
    return real(multi, running);
}

CURLMcode curl_multi_poll(CURLM* multi, struct curl_waitfd* fds, unsigned int count,
                          int timeout, int* ready) {
    if (should_fail("poll")) return CURLM_INTERNAL_ERROR;
    CURLMcode (*real)(CURLM*, struct curl_waitfd*, unsigned int, int, int*) =
        dlsym(RTLD_NEXT, "curl_multi_poll");
    return real(multi, fds, count, timeout, ready);
}

CURLMcode curl_multi_remove_handle(CURLM* multi, CURL* easy) {
    if (should_fail("remove")) return CURLM_INTERNAL_ERROR;
    CURLMcode (*real)(CURLM*, CURL*) = dlsym(RTLD_NEXT, "curl_multi_remove_handle");
    CURLMcode result = real(multi, easy);
    return should_fail("remove-after") ? CURLM_INTERNAL_ERROR : result;
}
