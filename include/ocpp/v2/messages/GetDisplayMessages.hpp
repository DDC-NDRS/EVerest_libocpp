// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
// This code is generated using the generator in 'src/code_generator/common`, please do not edit manually

#ifndef OCPP_V2_GETDISPLAYMESSAGES_HPP
#define OCPP_V2_GETDISPLAYMESSAGES_HPP

#include <nlohmann/json_fwd.hpp>
#include <optional>

#include <ocpp/common/types.hpp>
#include <ocpp/v2/ocpp_enums.hpp>
#include <ocpp/v2/ocpp_types.hpp>

namespace ocpp {
namespace v2 {

/// \brief Contains a OCPP GetDisplayMessages message
struct GetDisplayMessagesRequest : public ocpp::Message {
    int32_t requestId;
    std::optional<std::vector<int32_t>> id;
    std::optional<MessagePriorityEnum> priority;
    std::optional<MessageStateEnum> state;
    std::optional<CustomData> customData;

    /// \brief Provides the type of this GetDisplayMessages message as a human readable string
    /// \returns the message type as a human readable string
    std::string get_type() const override;
};

/// \brief Conversion from a given GetDisplayMessagesRequest \p k to a given json object \p j
void to_json(json& j, const GetDisplayMessagesRequest& k);

/// \brief Conversion from a given json object \p j to a given GetDisplayMessagesRequest \p k
void from_json(const json& j, GetDisplayMessagesRequest& k);

/// \brief Writes the string representation of the given GetDisplayMessagesRequest \p k to the given output stream \p os
/// \returns an output stream with the GetDisplayMessagesRequest written to
std::ostream& operator<<(std::ostream& os, const GetDisplayMessagesRequest& k);

/// \brief Contains a OCPP GetDisplayMessagesResponse message
struct GetDisplayMessagesResponse : public ocpp::Message {
    GetDisplayMessagesStatusEnum status;
    std::optional<StatusInfo> statusInfo;
    std::optional<CustomData> customData;

    /// \brief Provides the type of this GetDisplayMessagesResponse message as a human readable string
    /// \returns the message type as a human readable string
    std::string get_type() const override;
};

/// \brief Conversion from a given GetDisplayMessagesResponse \p k to a given json object \p j
void to_json(json& j, const GetDisplayMessagesResponse& k);

/// \brief Conversion from a given json object \p j to a given GetDisplayMessagesResponse \p k
void from_json(const json& j, GetDisplayMessagesResponse& k);

/// \brief Writes the string representation of the given GetDisplayMessagesResponse \p k to the given output stream \p
/// os \returns an output stream with the GetDisplayMessagesResponse written to
std::ostream& operator<<(std::ostream& os, const GetDisplayMessagesResponse& k);

} // namespace v2
} // namespace ocpp

#endif // OCPP_V2_GETDISPLAYMESSAGES_HPP
