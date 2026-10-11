#include <rstd/test/gtest.hpp>
import ncrequest;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace ncrequest;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
struct CacheTestBody {
    bool done {};
    auto next_chunk() -> ncrequest::coro<ncrequest::Result<Option<Bytes>>> {
        if (done) co_return Ok(None<Bytes>());
        done = true;
        co_return Ok(Some(Bytes::copy_from_slice("cached"_bytes)));
    }
    auto trailers() const -> Option<ref<lihttpto::Headers>> { return None(); }
    auto is_finished() const -> bool { return done; }
    void cancel() { done = true; }
};
struct CacheTestStore {
    bool fresh;
    auto open(CacheQuery, Arc<RequestControl>) -> ncrequest::coro<ncrequest::Result<CacheAccess>> {
        lihttpto::Headers headers;
        headers.push(lihttpto::Header::make("cache-control"_str,
                                            fresh ? "max-age=60"_bytes : "max-age=0"_bytes)
                         .unwrap());
        auto          policy = CachePolicy::make(headers, cache_now(), cache_now());
        CacheMetadata meta {
            { lihttpto::StatusCode::make(u16(200)).unwrap(), rstd::move(headers), None(), None() },
            rstd::move(policy)
        };
        co_return Ok(CacheAccess {
            Some(CacheRecord { rstd::move(meta), ResponseSource::make(CacheTestBody {}) }),
            None() });
    }
    void report(ErrorKind) {}
};
TEST(HttpCache, AgeAndRequestRestrictions) {
    lihttpto::Headers headers;
    headers.push(
        lihttpto::Header::make("cache-control"_str, "max-age=60, must-revalidate"_bytes).unwrap());
    headers.push(lihttpto::Header::make("age"_str, "10"_bytes).unwrap());
    auto policy = CachePolicy::make(headers, i64(100), i64(102));
    EXPECT_EQ(policy.age(i64(110)), i64(20));
    EXPECT_TRUE(policy.fresh(i64(110), {}));
    EXPECT_FALSE(policy.fresh(i64(150), {}));
    lihttpto::CacheControl request;
    request.no_cache = true;
    EXPECT_FALSE(policy.fresh(i64(110), request));
    auto out = policy.browser_headers(headers, i64(110));
    EXPECT_EQ(*lihttpto::header_text(out, "age"_str), "20"_str);
    EXPECT_EQ(*lihttpto::header_text(out, "cache-control"_str),
              "private, max-age=60, must-revalidate"_str);
}
TEST(HttpCache, SameResponseBodyAndOnlyIfCached) {
    auto store = Arc<dyn<CacheStorage>>::make(CacheTestStore { true });
    auto session =
        Session::make({},
                      Some(CacheConfiguration { rstd::move(store), String::make("account"_str) }))
            .unwrap();
    auto           request = Request::from_url("http://127.0.0.1:1/test#fragment"_str).unwrap();
    RequestOptions options;
    options.cache_mode = CacheMode::OnlyIfCached;
    request.set_options(rstd::move(options));
    auto control = Arc<RequestControl>::make();
    auto result  = rstd::async::block_on(session->send(rstd::move(request), control.clone()));
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ((*result)->code(), Some(i32(200)));
    auto body = (*result)->take_body();
    ASSERT_TRUE(body.is_ok());
    EXPECT_TRUE((*result)->take_body().is_err());
    auto bytes = rstd::async::block_on(body->collect());
    ASSERT_TRUE(bytes.is_ok());
    EXPECT_EQ(rstd::str_::from_utf8(bytes->as_slice()).unwrap(), "cached"_str);
    EXPECT_TRUE(rstd::async::block_on(body->next()).is_err());
    auto duplicate = Request::from_url("http://127.0.0.1:1/"_str).unwrap();
    EXPECT_TRUE(
        rstd::async::block_on(session->send(rstd::move(duplicate), control.clone())).is_err());
    auto stale =
        Session::make(
            {},
            Some(CacheConfiguration { Arc<dyn<CacheStorage>>::make(CacheTestStore { false }),
                                      String::make("account"_str) }))
            .unwrap();
    auto           only = Request::from_url("http://127.0.0.1:1/"_str).unwrap();
    RequestOptions cached;
    cached.cache_mode = CacheMode::OnlyIfCached;
    only.set_options(rstd::move(cached));
    EXPECT_TRUE(rstd::async::block_on(stale->send(rstd::move(only))).is_err());
}

TEST(HttpCache, CloseCancelsCachedBody) {
    auto session =
        Session::make(
            {},
            Some(CacheConfiguration { Arc<dyn<CacheStorage>>::make(CacheTestStore { true }),
                                      String::make("account"_str) }))
            .unwrap();
    auto response =
        rstd::async::block_on(session->send(Request::from_url("http://127.0.0.1:1/"_str).unwrap()))
            .unwrap();
    session->close();
    auto result = rstd::async::block_on(response->bytes());
    ASSERT_TRUE(result.is_err());
    EXPECT_TRUE(result.unwrap_err().is_Canceled());
}

TEST(HttpCache, ExpiresWithoutDateKeepsOriginalAnchor) {
    lihttpto::Headers headers;
    headers.push(
        lihttpto::Header::make("expires"_str, "Thu, 01 Jan 1970 00:03:20 GMT"_bytes).unwrap());
    headers.push(lihttpto::Header::make("age"_str, "50"_bytes).unwrap());
    auto policy = CachePolicy::make(headers, i64(150), i64(150), Some(i64(100)));
    EXPECT_EQ(policy.lifetime(), i64(100));
    EXPECT_EQ(policy.age(i64(150)), i64(50));
    EXPECT_TRUE(policy.fresh(i64(199), {}));
    EXPECT_FALSE(policy.fresh(i64(200), {}));
}
