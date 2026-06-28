module;
#include <concepts>
#include <future>
#include <span>
#include <string>
#include <string_view>

export module ncrequest:client_websocket_backend;
export import ncrequest.type;

namespace ncrequest::client
{

export template<typename T>
concept WebSocketBackend = requires(
    T client,
    const T const_client,
    const std::string& url,
    std::string_view text,
    std::span<const rstd::byte> bytes,
    typename T::ConnectedCallback connected,
    typename T::MessageCallback message,
    typename T::ErrorCallback error) {
    { client.connect(url) } -> std::same_as<std::future<bool>>;
    { client.disconnect() } -> std::same_as<void>;
    { const_client.is_connected() } -> std::convertible_to<bool>;
    { client.send(text) } -> std::same_as<void>;
    { client.send(bytes) } -> std::same_as<void>;
    { client.set_on_connected_callback(rstd::move(connected)) } -> std::same_as<void>;
    { client.set_on_message_callback(rstd::move(message)) } -> std::same_as<void>;
    { client.set_on_error_callback(rstd::move(error)) } -> std::same_as<void>;
    { client.on_message_callback() } -> std::same_as<const typename T::MessageCallback&>;
};

} // namespace ncrequest::client
