// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
// This code is generated using the generator in 'src/code_generator/common`, please do not edit manually

#ifndef OCPP_V2_TRIGGERMESSAGE_HPP
#define OCPP_V2_TRIGGERMESSAGE_HPP

#include <nlohmann/json_fwd.hpp>
#include <optional>

#include <ocpp/common/types.hpp>
#include <ocpp/v2/ocpp_enums.hpp>
#include <ocpp/v2/ocpp_types.hpp>

namespace ocpp {
namespace v2 {

/// \brief Contains a OCPP TriggerMessage message
struct TriggerMessageRequest : public ocpp::Message {
    MessageTriggerEnum requestedMessage;
    std::optional<EVSE> evse;
    std::optional<CiString<50>> customTrigger;
    std::optional<CustomData> customData;

    /// \brief Provides the type of this TriggerMessage message as a human readable string
    /// \returns the message type as a human readable string
    std::string get_type() const override;
};

/// \brief Conversion from a given TriggerMessageRequest \p k to a given json object \p j
void to_json(json& j, const TriggerMessageRequest& k);

/// \brief Conversion from a given json object \p j to a given TriggerMessageRequest \p k
void from_json(const json& j, TriggerMessageRequest& k);

/// \brief Writes the string representation of the given TriggerMessageRequest \p k to the given output stream \p os
/// \returns an output stream with the TriggerMessageRequest written to
std::ostream& operator<<(std::ostream& os, const TriggerMessageRequest& k);

/// \brief Contains a OCPP TriggerMessageResponse message
struct TriggerMessageResponse : public ocpp::Message {
    TriggerMessageStatusEnum status;
    std::optional<StatusInfo> statusInfo;
    std::optional<CustomData> customData;

    /// \brief Provides the type of this TriggerMessageResponse message as a human readable string
    /// \returns the message type as a human readable string
    std::string get_type() const override;
};

/// \brief Conversion from a given TriggerMessageResponse \p k to a given json object \p j
void to_json(json& j, const TriggerMessageResponse& k);

/// \brief Conversion from a given json object \p j to a given TriggerMessageResponse \p k
void from_json(const json& j, TriggerMessageResponse& k);

/// \brief Writes the string representation of the given TriggerMessageResponse \p k to the given output stream \p os
/// \returns an output stream with the TriggerMessageResponse written to
std::ostream& operator<<(std::ostream& os, const TriggerMessageResponse& k);

} // namespace v2
} // namespace ocpp

#endif // OCPP_V2_TRIGGERMESSAGE_HPP
