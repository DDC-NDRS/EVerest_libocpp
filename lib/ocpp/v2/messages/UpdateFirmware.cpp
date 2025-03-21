// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
// This code is generated using the generator in 'src/code_generator/common`, please do not edit manually

#include <ocpp/v2/messages/UpdateFirmware.hpp>

#include <optional>
#include <ostream>
#include <string>

using json = nlohmann::json;

namespace ocpp {
namespace v2 {

std::string UpdateFirmwareRequest::get_type() const {
    return "UpdateFirmware";
}

void to_json(json& j, const UpdateFirmwareRequest& k) {
    // the required parts of the message
    j = json{
        {"requestId", k.requestId},
        {"firmware", k.firmware},
    };
    // the optional parts of the message
    if (k.retries) {
        j["retries"] = k.retries.value();
    }
    if (k.retryInterval) {
        j["retryInterval"] = k.retryInterval.value();
    }
    if (k.customData) {
        j["customData"] = k.customData.value();
    }
}

void from_json(const json& j, UpdateFirmwareRequest& k) {
    // the required parts of the message
    k.requestId = j.at("requestId");
    k.firmware = j.at("firmware");

    // the optional parts of the message
    if (j.contains("retries")) {
        k.retries.emplace(j.at("retries"));
    }
    if (j.contains("retryInterval")) {
        k.retryInterval.emplace(j.at("retryInterval"));
    }
    if (j.contains("customData")) {
        k.customData.emplace(j.at("customData"));
    }
}

/// \brief Writes the string representation of the given UpdateFirmwareRequest \p k to the given output stream \p os
/// \returns an output stream with the UpdateFirmwareRequest written to
std::ostream& operator<<(std::ostream& os, const UpdateFirmwareRequest& k) {
    os << json(k).dump(4);
    return os;
}

std::string UpdateFirmwareResponse::get_type() const {
    return "UpdateFirmwareResponse";
}

void to_json(json& j, const UpdateFirmwareResponse& k) {
    // the required parts of the message
    j = json{
        {"status", conversions::update_firmware_status_enum_to_string(k.status)},
    };
    // the optional parts of the message
    if (k.statusInfo) {
        j["statusInfo"] = k.statusInfo.value();
    }
    if (k.customData) {
        j["customData"] = k.customData.value();
    }
}

void from_json(const json& j, UpdateFirmwareResponse& k) {
    // the required parts of the message
    k.status = conversions::string_to_update_firmware_status_enum(j.at("status"));

    // the optional parts of the message
    if (j.contains("statusInfo")) {
        k.statusInfo.emplace(j.at("statusInfo"));
    }
    if (j.contains("customData")) {
        k.customData.emplace(j.at("customData"));
    }
}

/// \brief Writes the string representation of the given UpdateFirmwareResponse \p k to the given output stream \p os
/// \returns an output stream with the UpdateFirmwareResponse written to
std::ostream& operator<<(std::ostream& os, const UpdateFirmwareResponse& k) {
    os << json(k).dump(4);
    return os;
}

} // namespace v2
} // namespace ocpp
