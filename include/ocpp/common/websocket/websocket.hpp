// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2023 Pionix GmbH and Contributors to EVerest
#ifndef OCPP_WEBSOCKET_HPP
#define OCPP_WEBSOCKET_HPP

#include <ocpp/common/evse_security.hpp>
#include <ocpp/common/ocpp_logging.hpp>

#include <ocpp/common/websocket/websocket_base.hpp>

namespace ocpp {
///
/// \brief contains a websocket abstraction that can connect to TLS and non-TLS websocket endpoints
///
class Websocket {
private:
    // unique_ptr holds address of base - requires WebSocketBase to have a virtual destructor
    std::unique_ptr<WebsocketBase> websocket;
    std::function<void(OcppProtocolVersion protocol)> connected_callback;
    std::function<void()> disconnected_callback;
    std::function<void(const WebsocketCloseReason reason)> stopped_connecting_callback;
    std::function<void(const std::string& message)> message_callback;
    std::shared_ptr<MessageLogging> logging;

public:
    /// \brief Creates a new Websocket object with the provided \p connection_options
    explicit Websocket(const WebsocketConnectionOptions& connection_options,
                       std::shared_ptr<EvseSecurity> evse_security, std::shared_ptr<MessageLogging> logging);
    ~Websocket();

    /// \brief Starts the connection attempts. It will init the websocket processing thread
    /// \returns true if the websocket is successfully initialized, false otherwise. Does
    ///          not wait for a successful connection
    bool start_connecting();

    void set_connection_options(const WebsocketConnectionOptions& connection_options);

    /// \brief disconnect the websocket
    void disconnect(const WebsocketCloseReason code);

    // \brief reconnects the websocket after the delay
    void reconnect(long delay);

    /// \brief indicates if the websocket is connected
    bool is_connected();

    /// \brief register a \p callback that is called when the websocket is connected successfully
    void register_connected_callback(const std::function<void(OcppProtocolVersion protocol)>& callback);

    /// \brief register a \p callback that is called when the websocket connection is disconnected
    void register_disconnected_callback(const std::function<void()>& callback);

    /// \brief register a \p callback that is called when the websocket connection has been stopped and will not attempt
    /// to reconnect
    void register_stopped_connecting_callback(const std::function<void(const WebsocketCloseReason)>& callback);

    /// \brief register a \p callback that is called when the websocket receives a message
    void register_message_callback(const std::function<void(const std::string& message)>& callback);

    /// \brief register a \p callback that is called when the websocket could not connect with a specific reason
    void register_connection_failed_callback(const std::function<void(ConnectionFailedReason)>& callback);

    /// \brief send a \p message over the websocket
    /// \returns true if the message was sent successfully
    bool send(const std::string& message);

    /// \brief set the websocket ping interval \p ping_interval_s in seconds and pong timeout \p pong_interval_s in
    /// seconds
    void set_websocket_ping_interval(int32_t ping_interval_s, int32_t pong_interval_s);

    /// \brief set the \p authorization_key of the connection_options
    void set_authorization_key(const std::string& authorization_key);
};

} // namespace ocpp
#endif // OCPP_WEBSOCKET_HPP
