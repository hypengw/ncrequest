export module ncrequest:response_reader;
export import :request;
export import ncrequest.coro;
export import lihttpto;
using namespace rstd::prelude;
using rstd::bytes::Bytes;
export namespace ncrequest
{
struct ResponseReader {
    template<class Self, class Delegate = void>
    struct Api {
        using Trait = ResponseReader;
        auto next_chunk() -> coro<Result<Option<Bytes>>> { return rstd::trait_call<0>(this); }
        auto trailers() const -> Option<ref<lihttpto::Headers>> {
            return rstd::trait_call<1>(this);
        }
        auto is_finished() const -> bool { return rstd::trait_call<2>(this); }
        void cancel() { rstd::trait_call<3>(this); }
        auto request() const -> Option<ref<Request>> { return rstd::trait_call<4>(this); }
    };
    template<class T>
    using Funcs =
        TraitFuncs<&T::next_chunk, &T::trailers, &T::is_finished, &T::cancel, &T::request>;
};
using ResponseSource = rstd::sync::Arc<dyn<ResponseReader>>;
} // namespace ncrequest
namespace rstd
{
export template<class T>
    requires requires(T& value, const T& view) {
        {
            value.next_chunk()
        } -> mtp::same_as<ncrequest::coro<ncrequest::Result<Option<bytes::Bytes>>>>;
        { view.trailers() } -> mtp::same_as<Option<ref<lihttpto::Headers>>>;
        { view.is_finished() } -> mtp::same_as<bool>;
        value.cancel();
    }
struct Impl<ncrequest::ResponseReader, T> : ImplBase<T> {
    auto next_chunk() { return this->self().next_chunk(); }
    auto trailers() const { return this->self().trailers(); }
    auto is_finished() const -> bool { return this->self().is_finished(); }
    void cancel() { this->self().cancel(); }
    auto request() const -> Option<ref<ncrequest::Request>> {
        if constexpr (requires { this->self().request(); })
            return Some(
                ref<ncrequest::Request>::from_raw_parts(rstd::addressof(this->self().request())));
        else
            return None();
    }
};
} // namespace rstd
