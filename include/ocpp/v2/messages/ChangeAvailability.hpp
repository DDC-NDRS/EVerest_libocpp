// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
// This code is generated using the generator in 'src/code_generator/common`, please do not edit manually

#ifndef OCPP_V2_CHANGEAVAILABILITY_HPP
#define OCPP_V2_CHANGEAVAILABILITY_HPP

#include <nlohmann/json_fwd.hpp>
#include <optional>

#include <ocpp/common/types.hpp>
#include <ocpp/v2/ocpp_enums.hpp>
#include <ocpp/v2/ocpp_types.hpp>

namespace ocpp {
namespace v2 {

/// \brief Contains a OCPP ChangeAvailability message
struct ChangeAvailabilityRequest : public ocpp::Message {
    OperationalStatusEnum operationalStatus;
    std::optional<EVSE> evse;
    std::optional<CustomData> customData;

    /// \brief Provides the type of this ChangeAvailability message as a human readable string
    /// \returns the message type as a human readable string
    std::string get_type() const override;
};

/// \brief Conversion from a given ChangeAvailabilityRequest \p k to a given json object \p j
void to_json(json& j, const ChangeAvailabilityRequest& k);

/// \brief Conversion from a given json object \p j to a given ChangeAvailabilityRequest \p k
void from_json(const json& j, ChangeAvailabilityRequest& k);

/// \brief Writes the string representation of the given ChangeAvailabilityRequest \p k to the given output stream \p os
/// \returns an output stream with the ChangeAvailabilityRequest written to
std::ostream& operator<<(std::ostream& os, const ChangeAvailabilityRequest& k);

/// \brief Contains a OCPP ChangeAvailabilityResponse message
struct ChangeAvailabilityResponse : public ocpp::Message {
    ChangeAvailabilityStatusEnum status;
    std::optional<StatusInfo> statusInfo;
    std::optional<CustomData> customData;

    /// \brief Provides the type of this ChangeAvailabilityResponse message as a human readable string
    /// \returns the message type as a human readable string
    std::string get_type() const override;
};

/// \brief Conversion from a given ChangeAvailabilityResponse \p k to a given json object \p j
void to_json(json& j, const ChangeAvailabilityResponse& k);

/// \brief Conversion from a given json object \p j to a given ChangeAvailabilityResponse \p k
void from_json(const json& j, ChangeAvailabilityResponse& k);

/// \brief Writes the string representation of the given ChangeAvailabilityResponse \p k to the given output stream \p
/// os \returns an output stream with the ChangeAvailabilityResponse written to
std::ostream& operator<<(std::ostream& os, const ChangeAvailabilityResponse& k);

} // namespace v2
} // namespace ocpp

#endif // OCPP_V2_CHANGEAVAILABILITY_HPP
