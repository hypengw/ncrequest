module;
#include <curl/curl.h>
#include <curl/websockets.h>
export module ncrequest.curl:curl;

namespace curl
{
export using ::CURL;
export using ::CURLcode;
export using ::CURLM;
export using ::CURLMcode;
export using ::CURLSH;
export using ::CURLoption;
export using ::CURLINFO;
export using ::CURLMSG;
export using ::CURLMsg;
export using ::CURLSHoption;
export using ::curl_lock_data;

export using ::curl_global_init;
export using ::curl_global_init_mem;

#undef CURL_GLOBAL_SSL
#undef CURL_GLOBAL_WIN32
#undef CURL_GLOBAL_ALL
#undef CURL_GLOBAL_NOTHING
#undef CURL_GLOBAL_DEFAULT
#undef CURL_GLOBAL_ACK_EINTR
export constexpr auto CURL_GLOBAL_SSL       = (1 << 0);
export constexpr auto CURL_GLOBAL_WIN32     = (1 << 1);
export constexpr auto CURL_GLOBAL_ALL       = (CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32);
export constexpr auto CURL_GLOBAL_NOTHING   = 0;
export constexpr auto CURL_GLOBAL_DEFAULT   = CURL_GLOBAL_ALL;
export constexpr auto CURL_GLOBAL_ACK_EINTR = (1 << 2);

export using ::curl_easy_init;
export using ::curl_easy_cleanup;
export using ::curl_easy_setopt;
export using ::curl_easy_pause;
export using ::curl_easy_perform;
export using ::curl_easy_strerror;
export using ::curl_easy_getinfo;

export using ::curl_multi_strerror;
export using ::curl_multi_init;
export using ::curl_multi_setopt;
export using ::curl_multi_cleanup;
export using ::curl_multi_add_handle;
export using ::curl_multi_perform;
export using ::curl_multi_poll;
export using ::curl_multi_remove_handle;
export using ::curl_multi_info_read;
export using ::curl_multi_wakeup;

export using ::curl_share_init;
export using ::curl_share_setopt;
export using ::curl_share_cleanup;

export using ::curl_slist;
export using ::curl_slist_append;
export using ::curl_slist_free_all;

export using ::curl_lock_data;
export using ::curl_lock_access;

export using ::curl_socket_t;
export using ::curl_ws_frame;
export using ::curl_ws_send;
export using ::curl_ws_recv;

#undef CURLWS_TEXT
#undef CURLWS_BINARY
#undef CURLWS_CONT
#undef CURLWS_CLOSE
#undef CURLWS_PING
#undef CURLWS_OFFSET

export constexpr auto CURLWS_TEXT   = (1 << 0);
export constexpr auto CURLWS_BINARY = (1 << 1);
export constexpr auto CURLWS_CONT   = (1 << 2);
export constexpr auto CURLWS_CLOSE  = (1 << 3);
export constexpr auto CURLWS_PING   = (1 << 4);
export constexpr auto CURLWS_OFFSET = (1 << 5);

#undef CURL_READFUNC_ABORT
#undef CURL_READFUNC_PAUSE
export constexpr auto CURL_READFUNC_ABORT = 0x10000001;
export constexpr auto CURL_READFUNC_PAUSE = 0x10000001;

#undef CURL_WRITEFUNC_PAUSE
#undef CURL_WRITEFUNC_ERROR
export constexpr auto CURL_WRITEFUNC_PAUSE = 0x10000001;
export constexpr auto CURL_WRITEFUNC_ERROR = 0xFFFFFFFF;
} // namespace curl
