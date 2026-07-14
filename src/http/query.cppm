export module ncrequest:http_query;
export import :http_error;
export import rstd;

namespace ncrequest::http
{

using namespace rstd::prelude;
using rstd::string::String;
using rstd::vec::Vec;

export class QueryPair : public DefaultInClass<QueryPair, Clone> {
public:
    QueryPair(String name, String value) noexcept;

    [[nodiscard]]
    auto name() const noexcept -> ref<str>;

    [[nodiscard]]
    auto value() const noexcept -> ref<str>;

    [[nodiscard]]
    auto clone() const -> QueryPair;

private:
    String name_;
    String value_;
};

export class QueryIter : public DefaultInClass<QueryIter, Iterator> {
public:
    using Item = ref<QueryPair>;

    QueryIter(const QueryPair* current, const QueryPair* end) noexcept;

    [[nodiscard]]
    auto next() noexcept -> Option<Item>;

private:
    const QueryPair* current_;
    const QueryPair* end_;
};

export class QueryValues : public DefaultInClass<QueryValues, Iterator> {
public:
    using Item = ref<str>;

    QueryValues(const QueryPair* current,
                const QueryPair* end,
                const QueryPair* match) noexcept;

    [[nodiscard]]
    auto next() noexcept -> Option<Item>;

private:
    const QueryPair* current_;
    const QueryPair* end_;
    const QueryPair* match_;
};

export class QueryParams : public DefaultInClass<QueryParams, Clone> {
public:
    QueryParams() noexcept = default;
    QueryParams(QueryParams&&) noexcept = default;
    auto operator=(QueryParams&&) noexcept -> QueryParams& = default;

    [[nodiscard]]
    static auto parse_query(ref<str> input) -> Result<QueryParams, QueryError>;

    [[nodiscard]]
    static auto parse_form(ref<str> input) -> Result<QueryParams, QueryError>;

    void add(ref<str> name, ref<str> value);
    void set(ref<str> name, ref<str> value);

    [[nodiscard]]
    auto get(ref<str> name) const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto values(ref<str> name) const noexcept -> QueryValues;

    [[nodiscard]]
    auto remove(ref<str> name) noexcept -> usize;

    [[nodiscard]]
    auto len() const noexcept -> usize;

    [[nodiscard]]
    auto is_empty() const noexcept -> bool;

    [[nodiscard]]
    auto iter() const noexcept -> QueryIter;

    [[nodiscard]]
    auto encode_query() const -> String;

    [[nodiscard]]
    auto encode_form() const -> String;

    [[nodiscard]]
    auto clone() const -> QueryParams;

private:
    static auto parse_with_mode(ref<str> input, bool form)
        -> Result<QueryParams, QueryError>;

    auto encode_with_mode(bool form) const -> String;

    Vec<QueryPair> pairs_;
};

export [[nodiscard]] auto encode_component(ref<str> input) -> String;

export [[nodiscard]] auto decode_component(ref<str> input) -> Result<String, QueryError>;

export [[nodiscard]] auto encode_form_component(ref<str> input) -> String;

export [[nodiscard]] auto decode_form_component(ref<str> input) -> Result<String, QueryError>;

} // namespace ncrequest::http

namespace rstd
{

export template<>
struct Impl<str_::FromStr, ncrequest::http::QueryParams>
    : ImplBase<ncrequest::http::QueryParams> {
    using Err = ncrequest::http::QueryError;

    static auto from_str(ref<str> input) -> Result<ncrequest::http::QueryParams, Err> {
        return ncrequest::http::QueryParams::parse_query(input);
    }
};

export template<>
struct Impl<fmt::Display, ncrequest::http::QueryParams>
    : ImplBase<ncrequest::http::QueryParams> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto encoded = this->self().encode_query();
        return formatter.write_raw(encoded.as_raw_ptr(), encoded.size());
    }
};

} // namespace rstd

namespace ncrequest::http
{

static_assert(Impled<QueryPair, Clone>);
static_assert(Impled<QueryIter, Iterator>);
static_assert(Impled<QueryValues, Iterator>);
static_assert(Impled<QueryParams, Clone>);
static_assert(Impled<QueryParams, rstd::str_::FromStr>);
static_assert(Impled<QueryParams, rstd::fmt::Display>);

} // namespace ncrequest::http
