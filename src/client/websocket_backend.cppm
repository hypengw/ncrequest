export module ncrequest:client_websocket_backend;
export import ncrequest.type;

namespace ncrequest::client
{

export template<typename T>
concept WebSocketBackend =
    requires(T client, const T const_client, ref<str> url, slice<u8> bytes,
             typename T::ConnectedCallback connected, typename T::DisconnectedCallback disconnected,
             typename T::MessageCallback message, typename T::ErrorCallback error) {
        { client.connect(url) } -> rstd::mtp::same_as<rstd::async::Completion<bool>>;
        { client.disconnect() } -> rstd::mtp::same_as<void>;
        { const_client.is_connected() } -> rstd::mtp::convertible_to<bool>;
        { client.send(url) } -> rstd::mtp::same_as<void>;
        { client.send(bytes) } -> rstd::mtp::same_as<void>;
        { client.set_on_connected_callback(rstd::move(connected)) } -> rstd::mtp::same_as<void>;
        {
            client.set_on_disconnected_callback(rstd::move(disconnected))
        } -> rstd::mtp::same_as<void>;
        { client.set_on_message_callback(rstd::move(message)) } -> rstd::mtp::same_as<void>;
        { client.set_on_error_callback(rstd::move(error)) } -> rstd::mtp::same_as<void>;
    };

} // namespace ncrequest::client
