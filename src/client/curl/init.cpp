module ncrequest.curl;
import :init;
import :error;
import rstd.core;

using max_align_t = rstd::cppstd::max_align_t;
using namespace rstd;

namespace
{
rstd::cppstd::pmr::memory_resource* g_resource = nullptr;

void* curl_malloc_fn(usize size) { return g_resource->allocate(size, alignof(max_align_t)); }

void* curl_realloc_fn(void* ptr, usize size) {
    if (! ptr) return curl_malloc_fn(size);
    // reallocate with new size
    void* new_ptr = g_resource->allocate(size, alignof(max_align_t));
    rstd::memcpy(new_ptr, ptr, size);
    g_resource->deallocate(ptr, size, alignof(max_align_t));
    return new_ptr;
}

void curl_free_fn(void* ptr) {
    if (ptr) {
        g_resource->deallocate(ptr, 0, alignof(max_align_t));
    }
}

char* curl_strdup_fn(const char* str) {
    if (! str) return nullptr;
    usize len     = rstd::strlen(str) + 1;
    char*  new_str = (char*)curl_malloc_fn(len);
    if (new_str) {
        rstd::memcpy(new_str, str, len);
    }
    return new_str;
}

void* curl_calloc_fn(usize nmemb, usize size) {
    void* ptr = curl_malloc_fn(nmemb * size);
    if (ptr) {
        rstd::memset(ptr, 0, nmemb * size);
    }
    return ptr;
}
} // namespace

auto ncrequest::curl_init(cppstd::pmr::memory_resource* resource) -> rstd::error_code {
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