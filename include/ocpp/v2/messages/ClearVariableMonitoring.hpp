// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
// This code is generated using the generator in 'src/code_generator/common`, please do not edit manually

#ifndef OCPP_V2_CLEARVARIABLEMONITORING_HPP
#define OCPP_V2_CLEARVARIABLEMONITORING_HPP

#include <nlohmann/json_fwd.hpp>
#include <optional>

#include <ocpp/common/types.hpp>
#include <ocpp/v2/ocpp_enums.hpp>
#include <ocpp/v2/ocpp_types.hpp>

namespace ocpp {
namespace v2 {

/// \brief Contains a OCPP ClearVariableMonitoring message
struct ClearVariableMonitoringRequest : public ocpp::Message {
    std::vector<int32_t> id;
    std::optional<CustomData> customData;

    /// \brief Provides the type of this ClearVariableMonitoring message as a human readable string
    /// \returns the message type as a human readable string
    std::string get_type() const override;
};

/// \brief Conversion from a given ClearVariableMonitoringRequest \p k to a given json object \p j
void to_json(json& j, const ClearVariableMonitoringRequest& k);

/// \brief Conversion from a given json object \p j to a given ClearVariableMonitoringRequest \p k
void from_json(const json& j, ClearVariableMonitoringRequest& k);

/// \brief Writes the string representation of the given ClearVariableMonitoringRequest \p k to the given output stream
/// \p os \returns an output stream with the ClearVariableMonitoringRequest written to
std::ostream& operator<<(std::ostream& os, const ClearVariableMonitoringRequest& k);

/// \brief Contains a OCPP ClearVariableMonitoringResponse message
struct ClearVariableMonitoringResponse : public ocpp::Message {
    std::vector<ClearMonitoringResult> clearMonitoringResult;
    std::optional<CustomData> customData;

    /// \brief Provides the type of this ClearVariableMonitoringResponse message as a human readable string
    /// \returns the message type as a human readable string
    std::string get_type() const override;
};

/// \brief Conversion from a given ClearVariableMonitoringResponse \p k to a given json object \p j
void to_json(json& j, const ClearVariableMonitoringResponse& k);

/// \brief Conversion from a given json object \p j to a given ClearVariableMonitoringResponse \p k
void from_json(const json& j, ClearVariableMonitoringResponse& k);

/// \brief Writes the string representation of the given ClearVariableMonitoringResponse \p k to the given output stream
/// \p os \returns an output stream with the ClearVariableMonitoringResponse written to
std::ostream& operator<<(std::ostream& os, const ClearVariableMonitoringResponse& k);

} // namespace v2
} // namespace ocpp

#endif // OCPP_V2_CLEARVARIABLEMONITORING_HPP
