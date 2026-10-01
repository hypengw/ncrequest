export module ncrequest:client_websocket_backend;
export import rstd;

using namespace rstd::prelude;

using rstd::async::Completion;
using rstd::mtp::convertible_to;
using rstd::mtp::same_as;

namespace ncrequest::client
{

export template<typename T>
concept WebSocketBackend =
    requires(T client, const T const_client, ref<str> url, slice<u8> bytes,
             typename T::ConnectedCallback connected, typename T::DisconnectedCallback disconnected,
             typename T::MessageCallback message, typename T::ErrorCallback error) {
        { client.connect(url) } -> same_as<Completion<bool>>;
        { client.disconnect() } -> same_as<void>;
        { const_client.is_connected() } -> convertible_to<bool>;
        { client.send(url) } -> same_as<void>;
        { client.send(bytes) } -> same_as<void>;
        { client.set_on_connected_callback(rstd::move(connected)) } -> same_as<void>;
        { client.set_on_disconnected_callback(rstd::move(disconnected)) } -> same_as<void>;
        { client.set_on_message_callback(rstd::move(message)) } -> same_as<void>;
        { client.set_on_error_callback(rstd::move(error)) } -> same_as<void>;
    };

} // namespace ncrequest::client
