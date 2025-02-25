module;
#include <system_error>
#include <cstring>
#include <memory_resource>
#include <curl/curl.h>

module ncrequest.curl;
import :init;
import :error;

namespace
{
std::pmr::memory_resource* g_resource = nullptr;

void* curl_malloc_fn(size_t size) { return g_resource->allocate(size, alignof(std::max_align_t)); }

void* curl_realloc_fn(void* ptr, size_t size) {
    if (! ptr) return curl_malloc_fn(size);
    // reallocate with new size
    void* new_ptr = g_resource->allocate(size, alignof(std::max_align_t));
    std::memcpy(new_ptr, ptr, size);
    g_resource->deallocate(ptr, size, alignof(std::max_align_t));
    return new_ptr;
}

void curl_free_fn(void* ptr) {
    if (ptr) {
        g_resource->deallocate(ptr, 0, alignof(std::max_align_t));
    }
}

char* curl_strdup_fn(const char* str) {
    if (! str) return nullptr;
    size_t len     = std::strlen(str) + 1;
    char*  new_str = (char*)curl_malloc_fn(len);
    if (new_str) {
        std::memcpy(new_str, str, len);
    }
    return new_str;
}

void* curl_calloc_fn(size_t nmemb, size_t size) {
    void* ptr = curl_malloc_fn(nmemb * size);
    if (ptr) {
        std::memset(ptr, 0, nmemb * size);
    }
    return ptr;
}
} // namespace

auto ncrequest::curl_init(std::pmr::memory_resource* resource) -> std::error_code {
    if (resource == nullptr) {
        return ::make_error_code(curl_global_init(CURL_GLOBAL_ALL));
    } else {
        g_resource = resource;
        return ::make_error_code(curl_global_init_mem(CURL_GLOBAL_ALL,
                                                      curl_malloc_fn,
                                                      curl_free_fn,
                                                      curl_realloc_fn,
                                                      curl_strdup_fn,
                                                      curl_calloc_fn));
    }
}