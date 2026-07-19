module ncrequest.curl;
import :init;
import rstd.core;

using max_align_t = std::max_align_t;
using namespace rstd::prelude;

namespace
{
std::pmr::memory_resource* g_resource = nullptr;

void* curl_malloc_fn(rstd::size_t size) { return g_resource->allocate(size, alignof(max_align_t)); }

void* curl_realloc_fn(void* ptr, rstd::size_t size) {
    if (! ptr) return curl_malloc_fn(size);
    // reallocate with new size
    void* new_ptr = g_resource->allocate(size, alignof(max_align_t));
    rstd::mem::memcpy(new_ptr, ptr, usize(size));
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
    auto  len     = rstd::strlen(str) + 1;
    char* new_str = (char*)curl_malloc_fn(len);
    if (new_str) {
        rstd::mem::memcpy(new_str, str, usize(len));
    }
    return new_str;
}

void* curl_calloc_fn(rstd::size_t nmemb, rstd::size_t size) {
    void* ptr = curl_malloc_fn(nmemb * size);
    if (ptr) {
        rstd::mem::memset(ptr, u8(), usize(nmemb * size));
    }
    return ptr;
}
} // namespace

auto ncrequest::curl_init(std::pmr::memory_resource* resource)
    -> rstd::Result<rstd::empty, curl::CURLcode> {
    auto code = curl::CURLcode::CURLE_OK;
    if (resource == nullptr) {
        code = curl_global_init(CURL_GLOBAL_ALL);
    } else {
        g_resource = resource;
        code       = curl_global_init_mem(CURL_GLOBAL_ALL,
                                          curl_malloc_fn,
                                          curl_free_fn,
                                          curl_realloc_fn,
                                          curl_strdup_fn,
                                          curl_calloc_fn);
    }
    if (code != curl::CURLcode::CURLE_OK) return rstd::Err(code);
    return rstd::Ok(rstd::empty {});
}
