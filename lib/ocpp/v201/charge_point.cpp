// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <ocpp/common/constants.hpp>
#include <ocpp/common/types.hpp>
#include <ocpp/v201/charge_point.hpp>
#include <ocpp/v201/ctrlr_component_variables.hpp>
#include <ocpp/v201/device_model_storage_sqlite.hpp>
#include <ocpp/v201/message_dispatcher.hpp>
#include <ocpp/v201/messages/FirmwareStatusNotification.hpp>
#include <ocpp/v201/messages/LogStatusNotification.hpp>
#include <ocpp/v201/notify_report_requests_splitter.hpp>

#include <optional>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

const auto DEFAULT_MAX_CUSTOMER_INFORMATION_DATA_LENGTH = 51200;
const auto DEFAULT_PRICE_NUMBER_OF_DECIMALS = 3;

using DatabaseException = ocpp::common::DatabaseException;

namespace ocpp {
namespace v201 {

const auto DEFAULT_BOOT_NOTIFICATION_RETRY_INTERVAL = std::chrono::seconds(30);
const auto DEFAULT_MESSAGE_QUEUE_SIZE_THRESHOLD = 2E5;
const auto DEFAULT_MAX_MESSAGE_SIZE = 65000;

ChargePoint::ChargePoint(const std::map<int32_t, int32_t>& evse_connector_structure,
                         std::shared_ptr<DeviceModel> device_model, std::shared_ptr<DatabaseHandler> database_handler,
                         std::shared_ptr<MessageQueue<v201::MessageType>> message_queue,
                         const std::string& message_log_path, const std::shared_ptr<EvseSecurity> evse_security,
                         const Callbacks& callbacks) :
    ocpp::ChargingStationBase(evse_security),
    message_queue(message_queue),
    device_model(device_model),
    database_handler(database_handler),
    registration_status(RegistrationStatusEnum::Rejected),
    skip_invalid_csms_certificate_notifications(false),
    reset_scheduled(false),
    reset_scheduled_evseids{},
    firmware_status(FirmwareStatusEnum::Idle),
    upload_log_status(UploadLogStatusEnum::Idle),
    bootreason(BootReasonEnum::PowerUp),
    ocsp_updater(this->evse_security, this->send_callback<GetCertificateStatusRequest, GetCertificateStatusResponse>(
                                          MessageType::GetCertificateStatusResponse)),
    monitoring_updater(
        device_model, [this](const std::vector<EventData>& events) { this->notify_event_req(events); },
        [this]() { return this->is_offline(); }),
    callbacks(callbacks) {

    if (!this->device_model) {
        EVLOG_AND_THROW(std::invalid_argument("Device model should not be null"));
    }

    // Make sure the received callback struct is completely filled early before we actually start running
    if (!this->callbacks.all_callbacks_valid(this->device_model)) {
        EVLOG_AND_THROW(std::invalid_argument("All non-optional callbacks must be supplied"));
    }

    if (!this->database_handler) {
        EVLOG_AND_THROW(std::invalid_argument("Database handler should not be null"));
    }

    initialize(evse_connector_structure, message_log_path);
}

ChargePoint::ChargePoint(const std::map<int32_t, int32_t>& evse_connector_structure,
                         std::unique_ptr<DeviceModelStorageInterface> device_model_storage_interface,
                         const std::string& ocpp_main_path, const std::string& core_database_path,
                         const std::string& sql_init_path, const std::string& message_log_path,
                         const std::shared_ptr<EvseSecurity> evse_security, const Callbacks& callbacks) :
    ChargePoint(
        evse_connector_structure, std::make_shared<DeviceModel>(std::move(device_model_storage_interface)),
        std::make_shared<DatabaseHandler>(
            std::make_unique<common::DatabaseConnection>(fs::path(core_database_path) / "cp.db"), sql_init_path),
        nullptr /* message_queue initialized in this constructor */, message_log_path, evse_security, callbacks) {
}

ChargePoint::ChargePoint(const std::map<int32_t, int32_t>& evse_connector_structure,
                         const std::string& device_model_storage_address, const bool initialize_device_model,
                         const std::string& device_model_migration_path, const std::string& device_model_config_path,
                         const std::string& ocpp_main_path, const std::string& core_database_path,
                         const std::string& sql_init_path, const std::string& message_log_path,
                         const std::shared_ptr<EvseSecurity> evse_security, const Callbacks& callbacks) :
    ChargePoint(evse_connector_structure,
                std::make_unique<DeviceModelStorageSqlite>(device_model_storage_address, device_model_migration_path,
                                                           device_model_config_path, initialize_device_model),
                ocpp_main_path, core_database_path, sql_init_path, message_log_path, evse_security, callbacks) {
}

ChargePoint::~ChargePoint() {
}

void ChargePoint::start(BootReasonEnum bootreason, bool start_connecting) {
    this->message_queue->start();

    this->bootreason = bootreason;
    // Trigger all initial status notifications and callbacks related to component state
    // Should be done before sending the BootNotification.req so that the correct states can be reported
    this->component_state_manager->trigger_all_effective_availability_changed_callbacks();
    // get transaction messages from db (if there are any) so they can be sent again.
    this->message_queue->get_persisted_messages_from_db();
    this->boot_notification_req(bootreason);
    // call clear_invalid_charging_profiles when system boots
    this->clear_invalid_charging_profiles();
    if (start_connecting) {
        this->connectivity_manager->connect();
    }

    const std::string firmware_version =
        this->device_model->get_value<std::string>(ControllerComponentVariables::FirmwareVersion);

    if (this->bootreason == BootReasonEnum::RemoteReset) {
        this->security->security_event_notification_req(
            CiString<50>(ocpp::security_events::RESET_OR_REBOOT),
            std::optional<CiString<255>>("Charging Station rebooted due to requested remote reset!"), true, true);
    } else if (this->bootreason == BootReasonEnum::ScheduledReset) {
        this->security->security_event_notification_req(
            CiString<50>(ocpp::security_events::RESET_OR_REBOOT),
            std::optional<CiString<255>>("Charging Station rebooted due to a scheduled reset!"), true, true);
    } else if (this->bootreason == BootReasonEnum::PowerUp) {
        std::string startup_message = "Charging Station powered up! Firmware version: " + firmware_version;
        this->security->security_event_notification_req(CiString<50>(ocpp::security_events::STARTUP_OF_THE_DEVICE),
                                                        std::optional<CiString<255>>(startup_message), true, true);
    } else if (this->bootreason == BootReasonEnum::FirmwareUpdate) {
        std::string startup_message =
            "Charging station reboot after firmware update. Firmware version: " + firmware_version;
        this->security->security_event_notification_req(CiString<50>(ocpp::security_events::FIRMWARE_UPDATED),
                                                        std::optional<CiString<255>>(startup_message), true, true);
    } else {
        std::string startup_message = "Charging station reset or reboot. Firmware version: " + firmware_version;
        this->security->security_event_notification_req(CiString<50>(ocpp::security_events::RESET_OR_REBOOT),
                                                        std::optional<CiString<255>>(startup_message), true, true);
    }
}

void ChargePoint::stop() {
    this->ocsp_updater.stop();
    this->availability->stop_heartbeat_timer();
    this->boot_notification_timer.stop();
    this->connectivity_manager->disconnect();
    this->security->stop_certificate_expiration_check_timers();
    this->monitoring_updater.stop_monitoring();
    this->message_queue->stop();
    this->security->stop_certificate_signed_timer();
}

void ChargePoint::disconnect_websocket() {
    this->connectivity_manager->disconnect();
}

void ChargePoint::on_network_disconnected(OCPPInterfaceEnum ocpp_interface) {
    this->connectivity_manager->on_network_disconnected(ocpp_interface);
}

void ChargePoint::connect_websocket(std::optional<int32_t> network_profile_slot) {
    this->connectivity_manager->connect(network_profile_slot);
}

void ChargePoint::on_firmware_update_status_notification(int32_t request_id,
                                                         const FirmwareStatusEnum& firmware_update_status) {
    if (this->firmware_status == firmware_update_status) {
        if (request_id == -1 or
            this->firmware_status_id.has_value() and this->firmware_status_id.value() == request_id) {
            // already sent, do not send again
            return;
        }
    }
    FirmwareStatusNotificationRequest req;
    req.status = firmware_update_status;
    // Firmware status and id are stored for future trigger message request.
    this->firmware_status = req.status;

    if (request_id != -1) {
        req.requestId = request_id; // L01.FR.20
        this->firmware_status_id = request_id;
    }

    ocpp::Call<FirmwareStatusNotificationRequest> call(req);
    this->message_dispatcher->dispatch_call_async(call);

    if (req.status == FirmwareStatusEnum::Installed) {
        std::string firmwareVersionMessage = "New firmware succesfully installed! Version: ";
        firmwareVersionMessage.append(
            this->device_model->get_value<std::string>(ControllerComponentVariables::FirmwareVersion));
        this->security->security_event_notification_req(CiString<50>(ocpp::security_events::FIRMWARE_UPDATED),
                                                        std::optional<CiString<255>>(firmwareVersionMessage), true,
                                                        true); // L01.FR.31
    } else if (req.status == FirmwareStatusEnum::InvalidSignature) {
        this->security->security_event_notification_req(
            CiString<50>(ocpp::security_events::INVALIDFIRMWARESIGNATURE),
            std::optional<CiString<255>>("Signature of the provided firmware is not valid!"), true,
            true); // L01.FR.03 - critical because TC_L_06_CS requires this message to be sent
    } else if (req.status == FirmwareStatusEnum::InstallVerificationFailed or
               req.status == FirmwareStatusEnum::InstallationFailed) {
        this->restore_all_connector_states();
    }

    if (this->firmware_status_before_installing == req.status) {
        // FIXME(Kai): This is a temporary workaround, because the EVerest System module does not keep track of
        // transactions and can't inquire about their status from the OCPP modules. If the firmware status is expected
        // to become "Installing", but we still have a transaction running, the update will wait for the transaction to
        // finish, and so we send an "InstallScheduled" status. This is necessary for OCTT TC_L_15_CS to pass.
        const auto transaction_active = this->evse_manager->any_transaction_active(std::nullopt);
        if (transaction_active) {
            this->firmware_status = FirmwareStatusEnum::InstallScheduled;
            req.status = firmware_status;
            ocpp::Call<FirmwareStatusNotificationRequest> call(req);
            this->message_dispatcher->dispatch_call_async(call);
        }
        this->change_all_connectors_to_unavailable_for_firmware_update();
    }
}

void ChargePoint::on_session_started(const int32_t evse_id, const int32_t connector_id) {
    this->evse_manager->get_evse(evse_id).submit_event(connector_id, ConnectorEvent::PlugIn);
}

Get15118EVCertificateResponse
ChargePoint::on_get_15118_ev_certificate_request(const Get15118EVCertificateRequest& request) {

    return this->security->on_get_15118_ev_certificate_request(request);
}

void ChargePoint::on_transaction_started(const int32_t evse_id, const int32_t connector_id,
                                         const std::string& session_id, const DateTime& timestamp,
                                         const ocpp::v201::TriggerReasonEnum trigger_reason,
                                         const MeterValue& meter_start, const std::optional<IdToken>& id_token,
                                         const std::optional<IdToken>& group_id_token,
                                         const std::optional<int32_t>& reservation_id,
                                         const std::optional<int32_t>& remote_start_id,
                                         const ChargingStateEnum charging_state) {

    auto& evse_handle = this->evse_manager->get_evse(evse_id);
    evse_handle.open_transaction(session_id, connector_id, timestamp, meter_start, id_token, group_id_token,
                                 reservation_id, charging_state);

    const auto meter_value = utils::get_meter_value_with_measurands_applied(
        meter_start, utils::get_measurands_vec(this->device_model->get_value<std::string>(
                         ControllerComponentVariables::SampledDataTxStartedMeasurands)));

    const auto& enhanced_transaction = evse_handle.get_transaction();
    Transaction transaction{enhanced_transaction->transactionId};
    transaction.chargingState = charging_state;
    if (remote_start_id.has_value()) {
        transaction.remoteStartId = remote_start_id.value();
        enhanced_transaction->remoteStartId = remote_start_id.value();
    }

    EVSE evse{evse_id};
    evse.connectorId.emplace(connector_id);

    std::optional<std::vector<MeterValue>> opt_meter_value;
    if (!meter_value.sampledValue.empty()) {
        opt_meter_value.emplace(1, meter_value);
    }

    this->transaction_event_req(TransactionEventEnum::Started, timestamp, transaction, trigger_reason,
                                enhanced_transaction->get_seq_no(), std::nullopt, evse, id_token, opt_meter_value,
                                std::nullopt, this->is_offline(), reservation_id);
}

void ChargePoint::on_transaction_finished(const int32_t evse_id, const DateTime& timestamp,
                                          const MeterValue& meter_stop, const ReasonEnum reason,
                                          const TriggerReasonEnum trigger_reason,
                                          const std::optional<IdToken>& id_token,
                                          const std::optional<std::string>& signed_meter_value,
                                          const ChargingStateEnum charging_state) {
    auto& evse_handle = this->evse_manager->get_evse(evse_id);
    auto& enhanced_transaction = evse_handle.get_transaction();
    if (enhanced_transaction == nullptr) {
        EVLOG_warning << "Received notification of finished transaction while no transaction was active";
        return;
    }

    enhanced_transaction->chargingState = charging_state;
    evse_handle.close_transaction(timestamp, meter_stop, reason);
    const auto transaction = enhanced_transaction->get_transaction();
    const auto transaction_id = enhanced_transaction->transactionId.get();

    std::optional<std::vector<ocpp::v201::MeterValue>> meter_values = std::nullopt;
    try {
        meter_values = std::make_optional(utils::get_meter_values_with_measurands_applied(
            this->database_handler->transaction_metervalues_get_all(enhanced_transaction->transactionId.get()),
            utils::get_measurands_vec(
                this->device_model->get_value<std::string>(ControllerComponentVariables::SampledDataTxEndedMeasurands)),
            utils::get_measurands_vec(
                this->device_model->get_value<std::string>(ControllerComponentVariables::AlignedDataTxEndedMeasurands)),
            timestamp,
            this->device_model->get_optional_value<bool>(ControllerComponentVariables::SampledDataSignReadings)
                .value_or(false),
            this->device_model->get_optional_value<bool>(ControllerComponentVariables::AlignedDataSignReadings)
                .value_or(false)));

        if (meter_values.value().empty()) {
            meter_values.reset();
        }
    } catch (const DatabaseException& e) {
        EVLOG_warning << "Could not get metervalues of transaction: " << e.what();
    }

    // E07.FR.02 The field idToken is provided when the authorization of the transaction has been ended
    const std::optional<IdToken> transaction_id_token =
        trigger_reason == ocpp::v201::TriggerReasonEnum::StopAuthorized ? id_token : std::nullopt;

    this->transaction_event_req(TransactionEventEnum::Ended, timestamp, enhanced_transaction->get_transaction(),
                                trigger_reason, enhanced_transaction->get_seq_no(), std::nullopt, std::nullopt,
                                transaction_id_token, meter_values, std::nullopt, this->is_offline(), std::nullopt);

    // K02.FR.05 The transaction is over, so delete the TxProfiles associated with the transaction.
    smart_charging_handler->delete_transaction_tx_profiles(enhanced_transaction->get_transaction().transactionId);
    evse_handle.release_transaction();

    bool send_reset = false;
    if (this->reset_scheduled) {
        // Check if this evse needs to be reset or set to inoperative.
        if (!this->reset_scheduled_evseids.empty()) {
            // There is an evse id in the 'reset scheduled' list, it needs to be
            // reset because it has finished charging.
            if (this->reset_scheduled_evseids.find(evse_id) != this->reset_scheduled_evseids.end()) {
                send_reset = true;
            }
        } else {
            // No evse id is given, whole charging station needs a reset. Wait
            // for last evse id to stop charging.
            bool is_charging = false;
            for (auto const& evse : *this->evse_manager) {
                if (evse.has_active_transaction()) {
                    is_charging = true;
                    break;
                }
            }

            if (is_charging) {
                set_evse_connectors_unavailable(evse_handle, false);
            } else {
                send_reset = true;
            }
        }
    }

    if (send_reset) {
        // Reset evse.
        if (reset_scheduled_evseids.empty()) {
            // This was the last evse that was charging, whole charging station
            // should be rest, send reset.
            this->callbacks.reset_callback(std::nullopt, ResetEnum::OnIdle);
            this->reset_scheduled = false;
        } else {
            // Reset evse that just stopped the transaction.
            this->callbacks.reset_callback(evse_id, ResetEnum::OnIdle);
            // Remove evse id that is just reset.
            this->reset_scheduled_evseids.erase(evse_id);

            // Check if there are more evse's that should be reset.
            if (reset_scheduled_evseids.empty()) {
                // No other evse's should be reset
                this->reset_scheduled = false;
            }
        }

        this->reset_scheduled_evseids.erase(evse_id);
    }

    this->availability->handle_scheduled_change_availability_requests(evse_id);
    this->availability->handle_scheduled_change_availability_requests(0);
}

void ChargePoint::on_session_finished(const int32_t evse_id, const int32_t connector_id) {
    this->evse_manager->get_evse(evse_id).submit_event(connector_id, ConnectorEvent::PlugOut);
}

void ChargePoint::on_authorized(const int32_t evse_id, const int32_t connector_id, const IdToken& id_token) {
    auto& evse = this->evse_manager->get_evse(evse_id);
    if (!evse.has_active_transaction()) {
        // nothing to report in case transaction is not yet open
        return;
    }

    std::unique_ptr<EnhancedTransaction>& transaction = evse.get_transaction();
    if (transaction->id_token_sent) {
        // if transactions id_token_sent is set, it is assumed it has already been reported
        return;
    }

    // set id_token of enhanced_transaction and send TransactionEvent(Updated) with id_token
    transaction->set_id_token_sent();
    this->transaction_event_req(TransactionEventEnum::Updated, ocpp::DateTime(), transaction->get_transaction(),
                                TriggerReasonEnum::Authorized, transaction->get_seq_no(), std::nullopt, std::nullopt,
                                id_token, std::nullopt, std::nullopt, this->is_offline(), std::nullopt);
}

void ChargePoint::on_meter_value(const int32_t evse_id, const MeterValue& meter_value) {
    if (evse_id == 0) {
        // if evseId = 0 then store in the chargepoint metervalues
        this->aligned_data_evse0.set_values(meter_value);
    } else {
        this->evse_manager->get_evse(evse_id).on_meter_value(meter_value);
        this->update_dm_evse_power(evse_id, meter_value);
    }
}

std::string ChargePoint::get_customer_information(const std::optional<CertificateHashDataType> customer_certificate,
                                                  const std::optional<IdToken> id_token,
                                                  const std::optional<CiString<64>> customer_identifier) {
    std::stringstream s;

    // Retrieve possible customer information from application that uses this library
    if (this->callbacks.get_customer_information_callback.has_value()) {
        s << this->callbacks.get_customer_information_callback.value()(customer_certificate, id_token,
                                                                       customer_identifier);
    }

    // Retrieve information from auth cache
    if (id_token.has_value()) {
        const auto hashed_id_token = utils::generate_token_hash(id_token.value());
        try {
            const auto entry = this->authorization->authorization_cache_get_entry(hashed_id_token);
            if (entry.has_value()) {
                s << "Hashed id_token stored in cache: " + hashed_id_token + "\n";
                s << "IdTokenInfo: " << entry->id_token_info;
            }
        } catch (const DatabaseException& e) {
            EVLOG_warning << "Could not get authorization cache entry from database";
        } catch (const json::exception& e) {
            EVLOG_warning << "Could not parse data of IdTokenInfo: " << e.what();
        } catch (const std::exception& e) {
            EVLOG_error << "Unknown Error while parsing IdTokenInfo: " << e.what();
        }
    }

    return s.str();
}

void ChargePoint::clear_customer_information(const std::optional<CertificateHashDataType> customer_certificate,
                                             const std::optional<IdToken> id_token,
                                             const std::optional<CiString<64>> customer_identifier) {
    if (this->callbacks.clear_customer_information_callback.has_value()) {
        this->callbacks.clear_customer_information_callback.value()(customer_certificate, id_token,
                                                                    customer_identifier);
    }

    if (id_token.has_value()) {
        const auto hashed_id_token = utils::generate_token_hash(id_token.value());
        try {
            this->authorization->authorization_cache_delete_entry(hashed_id_token);
        } catch (const DatabaseException& e) {
            EVLOG_error << "Could not delete from table: " << e.what();
        } catch (const std::exception& e) {
            EVLOG_error << "Exception while deleting from auth cache table: " << e.what();
        }
        this->authorization->update_authorization_cache_size();
    }
}

void ChargePoint::configure_message_logging_format(const std::string& message_log_path) {
    auto log_formats = this->device_model->get_value<std::string>(ControllerComponentVariables::LogMessagesFormat);
    bool log_to_console = log_formats.find("console") != log_formats.npos;
    bool detailed_log_to_console = log_formats.find("console_detailed") != log_formats.npos;
    bool log_to_file = log_formats.find("log") != log_formats.npos;
    bool log_to_html = log_formats.find("html") != log_formats.npos;
    bool log_security = log_formats.find("security") != log_formats.npos;
    bool session_logging = log_formats.find("session_logging") != log_formats.npos;
    bool message_callback = log_formats.find("callback") != log_formats.npos;
    std::function<void(const std::string& message, MessageDirection direction)> logging_callback = nullptr;
    bool log_rotation =
        this->device_model->get_optional_value<bool>(ControllerComponentVariables::LogRotation).value_or(false);
    bool log_rotation_date_suffix =
        this->device_model->get_optional_value<bool>(ControllerComponentVariables::LogRotationDateSuffix)
            .value_or(false);
    uint64_t log_rotation_maximum_file_size =
        this->device_model->get_optional_value<uint64_t>(ControllerComponentVariables::LogRotationMaximumFileSize)
            .value_or(0);
    uint64_t log_rotation_maximum_file_count =
        this->device_model->get_optional_value<uint64_t>(ControllerComponentVariables::LogRotationMaximumFileCount)
            .value_or(0);

    if (message_callback) {
        logging_callback = this->callbacks.ocpp_messages_callback.value_or(nullptr);
    }

    if (log_rotation) {
        this->logging = std::make_shared<ocpp::MessageLogging>(
            !log_formats.empty(), message_log_path, "libocpp_201", log_to_console, detailed_log_to_console, log_to_file,
            log_to_html, log_security, session_logging, logging_callback,
            ocpp::LogRotationConfig(log_rotation_date_suffix, log_rotation_maximum_file_size,
                                    log_rotation_maximum_file_count),
            [this](ocpp::LogRotationStatus status) {
                if (status == ocpp::LogRotationStatus::RotatedWithDeletion) {
                    const auto& security_event = ocpp::security_events::SECURITYLOGWASCLEARED;
                    std::string tech_info = "Security log was rotated and an old log was deleted in the process";
                    this->security->security_event_notification_req(CiString<50>(security_event),
                                                                    CiString<255>(tech_info), true,
                                                                    utils::is_critical(security_event));
                }
            });
    } else {
        this->logging = std::make_shared<ocpp::MessageLogging>(
            !log_formats.empty(), message_log_path, DateTime().to_rfc3339(), log_to_console, detailed_log_to_console,
            log_to_file, log_to_html, log_security, session_logging, logging_callback);
    }
}

void ChargePoint::handle_cost_and_tariff(const TransactionEventResponse& response,
                                         const TransactionEventRequest& original_message,
                                         const json& original_transaction_event_response) {
    const bool tariff_enabled = this->is_tariff_enabled();

    const bool cost_enabled = this->is_cost_enabled();

    std::vector<DisplayMessageContent> cost_messages;

    // Check if there is a tariff message and if 'Tariff' is available and enabled
    if (response.updatedPersonalMessage.has_value() and tariff_enabled) {
        MessageContent personal_message = response.updatedPersonalMessage.value();
        DisplayMessageContent message = message_content_to_display_message_content(personal_message);
        cost_messages.push_back(message);

        // If cost is enabled, the message will be sent to the running cost callback. But if it is not enabled, the
        // tariff message will be sent using the display message callback.
        if (!cost_enabled and this->callbacks.set_display_message_callback.has_value() and
            this->callbacks.set_display_message_callback != nullptr) {
            DisplayMessage display_message;
            display_message.message = message;
            display_message.identifier_id = original_message.transactionInfo.transactionId;
            display_message.identifier_type = IdentifierType::TransactionId;
            this->callbacks.set_display_message_callback.value()({display_message});
        }
    }

    // Check if cost is available and enabled, and if there is a totalcost message.
    if (cost_enabled and response.totalCost.has_value() and this->callbacks.set_running_cost_callback.has_value()) {
        RunningCost running_cost;
        std::string total_cost;
        // We use the original string and convert it to a double ourselves, as the nlohmann library converts it to a
        // float first and then multiply by 10^5 for example (5 decimals) will give some rounding errors. With a initial
        // double instead of float, we have (a bit) more accuracy.
        if (original_transaction_event_response.contains("totalCost")) {
            total_cost = original_transaction_event_response.at("totalCost").dump();
            running_cost.cost = stod(total_cost);
        } else {
            running_cost.cost = static_cast<double>(response.totalCost.value());
        }

        if (original_message.eventType == TransactionEventEnum::Ended) {
            running_cost.state = RunningCostState::Finished;
        } else {
            running_cost.state = RunningCostState::Charging;
        }

        running_cost.transaction_id = original_message.transactionInfo.transactionId;

        if (original_message.meterValue.has_value()) {
            const auto& meter_value = original_message.meterValue.value();
            std::optional<float> max_meter_value;
            for (const MeterValue& mv : meter_value) {
                auto it = std::find_if(mv.sampledValue.begin(), mv.sampledValue.end(), [](const SampledValue& value) {
                    return value.measurand == MeasurandEnum::Energy_Active_Import_Register and !value.phase.has_value();
                });
                if (it != mv.sampledValue.end()) {
                    // Found a sampled metervalue we are searching for!
                    if (!max_meter_value.has_value() or max_meter_value.value() < it->value) {
                        max_meter_value = it->value;
                    }
                }
            }
            if (max_meter_value.has_value()) {
                running_cost.meter_value = static_cast<int32_t>(max_meter_value.value());
            }
        }

        running_cost.timestamp = original_message.timestamp;

        if (response.customData.has_value()) {
            // With the current spec, it is not possible to send a qr code as well as a multi language personal
            // message, because there can only be one vendor id in custom data. If you not check the vendor id, it
            // is just possible for a csms to include them both.
            const json& custom_data = response.customData.value();
            if (/*custom_data.contains("vendorId") and
                (custom_data.at("vendorId").get<std::string>() == "org.openchargealliance.org.qrcode") and */
                custom_data.contains("qrCodeText") and
                device_model->get_optional_value<bool>(ControllerComponentVariables::DisplayMessageQRCodeDisplayCapable)
                    .value_or(false)) {
                running_cost.qr_code_text = custom_data.at("qrCodeText");
            }

            // Add multilanguage messages
            if (custom_data.contains("updatedPersonalMessageExtra") and is_multilanguage_enabled()) {
                // Get supported languages, which is stored in the values list of "Language" of
                // "DisplayMessageCtrlr"
                std::optional<VariableMetaData> metadata = device_model->get_variable_meta_data(
                    ControllerComponentVariables::DisplayMessageLanguage.component,
                    ControllerComponentVariables::DisplayMessageLanguage.variable.value());

                std::vector<std::string> supported_languages;

                if (metadata.has_value() and metadata.value().characteristics.valuesList.has_value()) {
                    supported_languages =
                        ocpp::split_string(metadata.value().characteristics.valuesList.value(), ',', true);
                } else {
                    EVLOG_error << "DisplayMessageCtrlr variable Language should have a valuesList with supported "
                                   "languages";
                }

                for (const auto& m : custom_data.at("updatedPersonalMessageExtra").items()) {
                    DisplayMessageContent c = message_content_to_display_message_content(m.value());
                    if (!c.language.has_value()) {
                        EVLOG_warning
                            << "updated personal message extra sent but language unknown: Can not show message.";
                        continue;
                    }

                    if (supported_languages.empty()) {
                        EVLOG_warning << "Can not show personal message as the supported languages are unknown "
                                         "(please set the `valuesList` of `DisplayMessageCtrlr` variable `Language` to "
                                         "set the supported languages)";
                        // Break loop because the next iteration, the supported languages will also not be there.
                        break;
                    }

                    if (std::find(supported_languages.begin(), supported_languages.end(), c.language.value()) !=
                        supported_languages.end()) {
                        cost_messages.push_back(c);
                    } else {
                        EVLOG_warning << "Can not send a personal message text in language " << c.language.value()
                                      << " as it is not supported by the charging station.";
                    }
                }
            }
        }

        if (tariff_enabled and !cost_messages.empty()) {
            running_cost.cost_messages = cost_messages;
        }

        const int number_of_decimals =
            this->device_model->get_optional_value<int>(ControllerComponentVariables::NumberOfDecimalsForCostValues)
                .value_or(DEFAULT_PRICE_NUMBER_OF_DECIMALS);
        uint32_t decimals =
            (number_of_decimals < 0 ? DEFAULT_PRICE_NUMBER_OF_DECIMALS : static_cast<uint32_t>(number_of_decimals));
        const std::optional<std::string> currency =
            this->device_model->get_value<std::string>(ControllerComponentVariables::TariffCostCtrlrCurrency);
        this->callbacks.set_running_cost_callback.value()(running_cost, decimals, currency);
    }
}

bool ChargePoint::is_multilanguage_enabled() const {
    return this->device_model
        ->get_optional_value<bool>(ControllerComponentVariables::CustomImplementationMultiLanguageEnabled)
        .value_or(false);
}

bool ChargePoint::is_tariff_enabled() const {
    return this->device_model->get_optional_value<bool>(ControllerComponentVariables::TariffCostCtrlrAvailableTariff)
               .value_or(false) and
           this->device_model->get_optional_value<bool>(ControllerComponentVariables::TariffCostCtrlrEnabledTariff)
               .value_or(false);
}

bool ChargePoint::is_cost_enabled() const {
    return this->device_model->get_optional_value<bool>(ControllerComponentVariables::TariffCostCtrlrAvailableCost)
               .value_or(false) and
           this->device_model->get_optional_value<bool>(ControllerComponentVariables::TariffCostCtrlrEnabledCost)
               .value_or(false);
}

void ChargePoint::on_unavailable(const int32_t evse_id, const int32_t connector_id) {
    this->evse_manager->get_evse(evse_id).submit_event(connector_id, ConnectorEvent::Unavailable);
}

void ChargePoint::on_enabled(const int32_t evse_id, const int32_t connector_id) {
    this->evse_manager->get_evse(evse_id).submit_event(connector_id, ConnectorEvent::UnavailableCleared);
}

void ChargePoint::on_faulted(const int32_t evse_id, const int32_t connector_id) {
    this->evse_manager->get_evse(evse_id).submit_event(connector_id, ConnectorEvent::Error);
}

void ChargePoint::on_fault_cleared(const int32_t evse_id, const int32_t connector_id) {
    this->evse_manager->get_evse(evse_id).submit_event(connector_id, ConnectorEvent::ErrorCleared);
}

void ChargePoint::on_reserved(const int32_t evse_id, const int32_t connector_id) {
    if (this->reservation != nullptr) {
        this->reservation->on_reserved(evse_id, connector_id);
    }
}

void ChargePoint::on_reservation_cleared(const int32_t evse_id, const int32_t connector_id) {
    if (this->reservation != nullptr) {
        this->reservation->on_reservation_cleared(evse_id, connector_id);
    }
}

bool ChargePoint::on_charging_state_changed(const uint32_t evse_id, const ChargingStateEnum charging_state,
                                            const TriggerReasonEnum trigger_reason) {
    auto& evse = this->evse_manager->get_evse(evse_id);

    std::unique_ptr<EnhancedTransaction>& transaction = evse.get_transaction();
    if (transaction == nullptr) {
        EVLOG_warning << "Can not change charging state: no transaction for evse id " << evse_id;
        return false;
    }

    if (transaction->chargingState == charging_state) {
        EVLOG_debug << "Trying to send charging state changed without actual change, dropping message";
    } else {
        transaction->chargingState = charging_state;
        this->transaction_event_req(TransactionEventEnum::Updated, DateTime(), transaction->get_transaction(),
                                    trigger_reason, transaction->get_seq_no(), std::nullopt, std::nullopt, std::nullopt,
                                    std::nullopt, std::nullopt, this->is_offline(), std::nullopt);
    }
    return true;
}

std::optional<std::string> ChargePoint::get_evse_transaction_id(int32_t evse_id) {
    auto& evse = this->evse_manager->get_evse(evse_id);
    if (!evse.has_active_transaction()) {
        return std::nullopt;
    }
    return evse.get_transaction()->transactionId.get();
}

AuthorizeResponse ChargePoint::validate_token(const IdToken id_token, const std::optional<CiString<5500>>& certificate,
                                              const std::optional<std::vector<OCSPRequestData>>& ocsp_request_data) {
    return this->authorization->validate_token(id_token, certificate, ocsp_request_data);
}

void ChargePoint::on_event(const std::vector<EventData>& events) {
    this->notify_event_req(events);
}

void ChargePoint::on_log_status_notification(UploadLogStatusEnum status, int32_t requestId) {
    LogStatusNotificationRequest request;
    request.status = status;
    request.requestId = requestId;

    // Store for use by the triggerMessage
    this->upload_log_status = status;
    this->upload_log_status_id = requestId;

    ocpp::Call<LogStatusNotificationRequest> call(request);
    this->message_dispatcher->dispatch_call(call);
}

void ChargePoint::on_security_event(const CiString<50>& event_type, const std::optional<CiString<255>>& tech_info,
                                    const std::optional<bool>& critical, const std::optional<DateTime>& timestamp) {
    auto critical_security_event = true;
    if (critical.has_value()) {
        critical_security_event = critical.value();
    } else {
        critical_security_event = utils::is_critical(event_type);
    }
    this->security->security_event_notification_req(event_type, tech_info, false, critical_security_event, timestamp);
}

void ChargePoint::on_variable_changed(const SetVariableData& set_variable_data) {
    this->handle_variable_changed(set_variable_data);
}

void ChargePoint::on_reservation_status(const int32_t reservation_id, const ReservationUpdateStatusEnum status) {
    if (reservation != nullptr) {
        this->reservation->on_reservation_status(reservation_id, status);
    }
}

void ChargePoint::initialize(const std::map<int32_t, int32_t>& evse_connector_structure,
                             const std::string& message_log_path) {
    this->device_model->check_integrity(evse_connector_structure);
    this->database_handler->open_connection();
    this->component_state_manager = std::make_shared<ComponentStateManager>(
        evse_connector_structure, database_handler,
        [this](auto evse_id, auto connector_id, auto status, bool initiated_by_trigger_message) {
            this->update_dm_availability_state(evse_id, connector_id, status);
            if (this->connectivity_manager == nullptr or !this->connectivity_manager->is_websocket_connected() or
                this->registration_status != RegistrationStatusEnum::Accepted) {
                return false;
            } else {
                this->availability->status_notification_req(evse_id, connector_id, status,
                                                            initiated_by_trigger_message);
                return true;
            }
        });
    if (this->callbacks.cs_effective_operative_status_changed_callback.has_value()) {
        this->component_state_manager->set_cs_effective_availability_changed_callback(
            this->callbacks.cs_effective_operative_status_changed_callback.value());
    }
    if (this->callbacks.evse_effective_operative_status_changed_callback.has_value()) {
        this->component_state_manager->set_evse_effective_availability_changed_callback(
            this->callbacks.evse_effective_operative_status_changed_callback.value());
    }
    this->component_state_manager->set_connector_effective_availability_changed_callback(
        this->callbacks.connector_effective_operative_status_changed_callback);

    auto transaction_meter_value_callback = [this](const MeterValue& _meter_value, EnhancedTransaction& transaction) {
        if (_meter_value.sampledValue.empty() or !_meter_value.sampledValue.at(0).context.has_value()) {
            EVLOG_info << "Not sending MeterValue due to no values";
            return;
        }

        auto type = _meter_value.sampledValue.at(0).context.value();
        if (type != ReadingContextEnum::Sample_Clock and type != ReadingContextEnum::Sample_Periodic) {
            EVLOG_info << "Not sending MeterValue due to wrong context";
            return;
        }

        const auto filter_vec = utils::get_measurands_vec(this->device_model->get_value<std::string>(
            type == ReadingContextEnum::Sample_Clock ? ControllerComponentVariables::AlignedDataMeasurands
                                                     : ControllerComponentVariables::SampledDataTxUpdatedMeasurands));

        const auto filtered_meter_value = utils::get_meter_value_with_measurands_applied(_meter_value, filter_vec);

        if (!filtered_meter_value.sampledValue.empty()) {
            const auto trigger = type == ReadingContextEnum::Sample_Clock ? TriggerReasonEnum::MeterValueClock
                                                                          : TriggerReasonEnum::MeterValuePeriodic;
            this->transaction_event_req(TransactionEventEnum::Updated, DateTime(), transaction, trigger,
                                        transaction.get_seq_no(), std::nullopt, std::nullopt, std::nullopt,
                                        std::vector<MeterValue>(1, filtered_meter_value), std::nullopt,
                                        this->is_offline(), std::nullopt);
        }
    };

    this->evse_manager = std::make_unique<EvseManager>(
        evse_connector_structure, *this->device_model, this->database_handler, component_state_manager,
        transaction_meter_value_callback, this->callbacks.pause_charging_callback);

    this->smart_charging_handler =
        std::make_shared<SmartChargingHandler>(*this->evse_manager, this->device_model, *this->database_handler);

    this->configure_message_logging_format(message_log_path);
    this->monitoring_updater.start_monitoring();

    this->connectivity_manager =
        std::make_unique<ConnectivityManager>(*this->device_model, this->evse_security, this->logging,
                                              std::bind(&ChargePoint::message_callback, this, std::placeholders::_1));

    this->connectivity_manager->set_websocket_connected_callback(
        [this](int configuration_slot, const NetworkConnectionProfile& network_connection_profile, auto) {
            this->websocket_connected_callback(configuration_slot, network_connection_profile);
        });
    this->connectivity_manager->set_websocket_disconnected_callback(
        [this](int configuration_slot, const NetworkConnectionProfile& network_connection_profile, auto) {
            this->websocket_disconnected_callback(configuration_slot, network_connection_profile);
        });
    this->connectivity_manager->set_websocket_connection_failed_callback(
        std::bind(&ChargePoint::websocket_connection_failed, this, std::placeholders::_1));

    if (this->message_queue == nullptr) {
        std::set<v201::MessageType> message_types_discard_for_queueing;
        try {
            const auto message_types_discard_for_queueing_csl = ocpp::split_string(
                this->device_model
                    ->get_optional_value<std::string>(ControllerComponentVariables::MessageTypesDiscardForQueueing)
                    .value_or(""),
                ',');
            std::transform(message_types_discard_for_queueing_csl.begin(), message_types_discard_for_queueing_csl.end(),
                           std::inserter(message_types_discard_for_queueing, message_types_discard_for_queueing.end()),
                           [](const std::string element) { return conversions::string_to_messagetype(element); });
        } catch (const StringToEnumException& e) {
            EVLOG_warning << "Could not convert configured MessageType value of MessageTypesDiscardForQueueing. Please "
                             "check you configuration: "
                          << e.what();
        } catch (...) {
            EVLOG_warning << "Could not apply MessageTypesDiscardForQueueing configuration";
        }

        this->message_queue = std::make_unique<ocpp::MessageQueue<v201::MessageType>>(
            [this](json message) -> bool { return this->connectivity_manager->send_to_websocket(message.dump()); },
            MessageQueueConfig<v201::MessageType>{
                this->device_model->get_value<int>(ControllerComponentVariables::MessageAttempts),
                this->device_model->get_value<int>(ControllerComponentVariables::MessageAttemptInterval),
                this->device_model->get_optional_value<int>(ControllerComponentVariables::MessageQueueSizeThreshold)
                    .value_or(DEFAULT_MESSAGE_QUEUE_SIZE_THRESHOLD),
                this->device_model->get_optional_value<bool>(ControllerComponentVariables::QueueAllMessages)
                    .value_or(false),
                message_types_discard_for_queueing,
                this->device_model->get_value<int>(ControllerComponentVariables::MessageTimeout)},
            this->database_handler);
    }

    this->message_dispatcher =
        std::make_unique<MessageDispatcher>(*this->message_queue, *this->device_model, registration_status);
    this->data_transfer = std::make_unique<DataTransfer>(
        *this->message_dispatcher, this->callbacks.data_transfer_callback, DEFAULT_WAIT_FOR_FUTURE_TIMEOUT);
    this->security = std::make_unique<Security>(*this->message_dispatcher, *this->device_model, *this->logging,
                                                *this->evse_security, *this->connectivity_manager, this->ocsp_updater,
                                                this->callbacks.security_event_callback);

    if (device_model->get_optional_value<bool>(ControllerComponentVariables::ReservationCtrlrAvailable)
            .value_or(false)) {
        this->reservation = std::make_unique<Reservation>(
            *this->message_dispatcher, *this->device_model, *this->evse_manager,
            this->callbacks.reserve_now_callback.value(), this->callbacks.cancel_reservation_callback.value(),
            this->callbacks.is_reservation_for_token_callback);
    }

    this->authorization = std::make_unique<Authorization>(*this->message_dispatcher, *this->device_model,
                                                          *this->connectivity_manager.get(),
                                                          *this->database_handler.get(), *this->evse_security.get());
    this->authorization->start_auth_cache_cleanup_thread();

    if (device_model->get_optional_value<bool>(ControllerComponentVariables::DisplayMessageCtrlrAvailable)
            .value_or(false)) {
        this->display_message = std::make_unique<DisplayMessageBlock>(
            *this->message_dispatcher, *this->device_model, *this->evse_manager,
            this->callbacks.get_display_message_callback.value(), this->callbacks.set_display_message_callback.value(),
            this->callbacks.clear_display_message_callback.value());
    }

    this->availability = std::make_unique<Availability>(
        *this->message_dispatcher, *this->device_model, *this->evse_manager, *this->component_state_manager,
        this->callbacks.time_sync_callback, this->callbacks.all_connectors_unavailable_callback);

    if (this->callbacks.configure_network_connection_profile_callback.has_value()) {
        this->connectivity_manager->set_configure_network_connection_profile_callback(
            this->callbacks.configure_network_connection_profile_callback.value());
    }

    Component ocpp_comm_ctrlr = {"OCPPCommCtrlr"};
    Variable field_length = {"FieldLength"};
    field_length.instance = "Get15118EVCertificateResponse.exiResponse";
    this->device_model->set_value(ocpp_comm_ctrlr, field_length, AttributeEnum::Actual,
                                  std::to_string(ISO15118_GET_EV_CERTIFICATE_EXI_RESPONSE_SIZE),
                                  VARIABLE_ATTRIBUTE_VALUE_SOURCE_INTERNAL, true);
}

void ChargePoint::handle_message(const EnhancedMessage<v201::MessageType>& message) {
    const auto& json_message = message.message;
    try {
        switch (message.messageType) {
        case MessageType::BootNotificationResponse:
            this->handle_boot_notification_response(json_message);
            break;
        case MessageType::SetVariables:
            this->handle_set_variables_req(json_message);
            break;
        case MessageType::GetVariables:
            this->handle_get_variables_req(message);
            break;
        case MessageType::GetBaseReport:
            this->handle_get_base_report_req(json_message);
            break;
        case MessageType::GetReport:
            this->handle_get_report_req(message);
            break;
        case MessageType::Reset:
            this->handle_reset_req(json_message);
            break;
        case MessageType::ChangeAvailability:
        case MessageType::HeartbeatResponse:
            this->availability->handle_message(message);
            break;
        case MessageType::SetNetworkProfile:
            this->handle_set_network_profile_req(json_message);
            break;
        case MessageType::TransactionEventResponse:
            this->handle_transaction_event_response(message);
            break;
        case MessageType::RequestStartTransaction:
            this->handle_remote_start_transaction_request(json_message);
            break;
        case MessageType::RequestStopTransaction:
            this->handle_remote_stop_transaction_request(json_message);
            break;
        case MessageType::DataTransfer:
            this->data_transfer->handle_message(message);
            break;
        case MessageType::GetLog:
            this->handle_get_log_req(json_message);
            break;
        case MessageType::ClearCache:
        case MessageType::SendLocalList:
        case MessageType::GetLocalListVersion:
            this->authorization->handle_message(message);
            break;
        case MessageType::UpdateFirmware:
            this->handle_firmware_update_req(json_message);
            break;
        case MessageType::UnlockConnector:
            this->handle_unlock_connector(json_message);
            break;
        case MessageType::TriggerMessage:
            this->handle_trigger_message(json_message);
            break;
        case MessageType::ReserveNow:
        case MessageType::CancelReservation:
            if (this->reservation != nullptr) {
                this->reservation->handle_message(message);
            } else {
                send_not_implemented_error(message.uniqueId, message.messageTypeId);
            }
            break;
        case MessageType::CertificateSigned:
        case MessageType::SignCertificateResponse:
        case MessageType::GetInstalledCertificateIds:
        case MessageType::InstallCertificate:
        case MessageType::DeleteCertificate:
            this->security->handle_message(message);
            break;
        case MessageType::GetTransactionStatus:
            this->handle_get_transaction_status(json_message);
            break;
        case MessageType::CustomerInformation:
            this->handle_customer_information_req(json_message);
            break;
        case MessageType::SetChargingProfile:
            this->handle_set_charging_profile_req(json_message);
            break;
        case MessageType::ClearChargingProfile:
            this->handle_clear_charging_profile_req(json_message);
            break;
        case MessageType::GetChargingProfiles:
            this->handle_get_charging_profiles_req(json_message);
            break;
        case MessageType::GetCompositeSchedule:
            this->handle_get_composite_schedule_req(json_message);
            break;
        case MessageType::SetMonitoringBase:
            this->handle_set_monitoring_base_req(json_message);
            break;
        case MessageType::SetMonitoringLevel:
            this->handle_set_monitoring_level_req(json_message);
            break;
        case MessageType::SetVariableMonitoring:
            this->handle_set_variable_monitoring_req(message);
            break;
        case MessageType::GetMonitoringReport:
            this->handle_get_monitoring_report_req(json_message);
            break;
        case MessageType::ClearVariableMonitoring:
            this->handle_clear_variable_monitoring_req(json_message);
            break;
        case MessageType::GetDisplayMessages:
        case MessageType::SetDisplayMessage:
        case MessageType::ClearDisplayMessage:
            if (this->display_message != nullptr) {
                this->display_message->handle_message(message);
            } else {
                send_not_implemented_error(message.uniqueId, message.messageTypeId);
            }
            break;
        case MessageType::CostUpdated:
            this->handle_costupdated_req(json_message);
            break;
        default:
            send_not_implemented_error(message.uniqueId, message.messageTypeId);
            break;
        }
    } catch (const MessageTypeNotImplementedException& e) {
        EVLOG_warning << e.what();
        send_not_implemented_error(message.uniqueId, message.messageTypeId);
    }
}

void ChargePoint::message_callback(const std::string& message) {
    EnhancedMessage<v201::MessageType> enhanced_message;
    try {
        enhanced_message = this->message_queue->receive(message);
    } catch (const json::exception& e) {
        this->logging->central_system("Unknown", message);
        EVLOG_error << "JSON exception during reception of message: " << e.what();
        this->message_dispatcher->dispatch_call_error(
            CallError(MessageId("-1"), "RpcFrameworkError", e.what(), json({})));
        const auto& security_event = ocpp::security_events::INVALIDMESSAGES;
        this->security->security_event_notification_req(CiString<50>(security_event), CiString<255>(message), true,
                                                        utils::is_critical(security_event));
        return;
    } catch (const EnumConversionException& e) {
        EVLOG_error << "EnumConversionException during handling of message: " << e.what();
        auto call_error = CallError(MessageId("-1"), "FormationViolation", e.what(), json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
        const auto& security_event = ocpp::security_events::INVALIDMESSAGES;
        this->security->security_event_notification_req(CiString<50>(security_event), CiString<255>(message), true,
                                                        utils::is_critical(security_event));
        return;
    }

    enhanced_message.message_size = message.size();
    auto json_message = enhanced_message.message;
    this->logging->central_system(conversions::messagetype_to_string(enhanced_message.messageType), message);
    try {
        if (this->registration_status == RegistrationStatusEnum::Accepted) {
            this->handle_message(enhanced_message);
        } else if (this->registration_status == RegistrationStatusEnum::Pending) {
            if (enhanced_message.messageType == MessageType::BootNotificationResponse) {
                this->handle_boot_notification_response(json_message);
            } else {
                // TODO(piet): Check what kind of messages we should accept in Pending state
                if (enhanced_message.messageType == MessageType::GetVariables or
                    enhanced_message.messageType == MessageType::SetVariables or
                    enhanced_message.messageType == MessageType::GetBaseReport or
                    enhanced_message.messageType == MessageType::GetReport or
                    enhanced_message.messageType == MessageType::NotifyReportResponse or
                    enhanced_message.messageType == MessageType::TriggerMessage) {
                    this->handle_message(enhanced_message);
                } else if (enhanced_message.messageType == MessageType::RequestStartTransaction) {
                    // Send rejected: B02.FR.05
                    RequestStartTransactionResponse response;
                    response.status = RequestStartStopStatusEnum::Rejected;
                    const ocpp::CallResult<RequestStartTransactionResponse> call_result(response,
                                                                                        enhanced_message.uniqueId);
                    this->message_dispatcher->dispatch_call_result(call_result);
                } else if (enhanced_message.messageType == MessageType::RequestStopTransaction) {
                    // Send rejected: B02.FR.05
                    RequestStopTransactionResponse response;
                    response.status = RequestStartStopStatusEnum::Rejected;
                    const ocpp::CallResult<RequestStopTransactionResponse> call_result(response,
                                                                                       enhanced_message.uniqueId);
                    this->message_dispatcher->dispatch_call_result(call_result);
                } else {
                    std::string const call_error_message =
                        "Received invalid MessageType: " +
                        conversions::messagetype_to_string(enhanced_message.messageType) +
                        " from CSMS while in state Pending";
                    EVLOG_warning << call_error_message;
                    // B02.FR.09 send CALLERROR SecurityError
                    const auto call_error =
                        CallError(enhanced_message.uniqueId, "SecurityError", call_error_message, json({}));
                    this->message_dispatcher->dispatch_call_error(call_error);
                }
            }
        } else if (this->registration_status == RegistrationStatusEnum::Rejected) {
            if (enhanced_message.messageType == MessageType::BootNotificationResponse) {
                this->handle_boot_notification_response(json_message);
            } else if (enhanced_message.messageType == MessageType::TriggerMessage) {
                Call<TriggerMessageRequest> call(json_message);
                if (call.msg.requestedMessage == MessageTriggerEnum::BootNotification) {
                    this->handle_message(enhanced_message);
                } else {
                    const auto error_message =
                        "Received TriggerMessage with requestedMessage != BootNotification before "
                        "having received an accepted BootNotificationResponse";
                    EVLOG_warning << error_message;
                    const auto call_error = CallError(enhanced_message.uniqueId, "SecurityError", "", json({}));
                    this->message_dispatcher->dispatch_call_error(call_error);
                }
            } else {
                const auto error_message = "Received other message than BootNotificationResponse before "
                                           "having received an accepted BootNotificationResponse";
                EVLOG_warning << error_message;
                const auto call_error = CallError(enhanced_message.uniqueId, "SecurityError", "", json({}, true));
                this->message_dispatcher->dispatch_call_error(call_error);
            }
        }
    } catch (const EvseOutOfRangeException& e) {
        EVLOG_error << "Exception during handling of message: " << e.what();
        auto call_error = CallError(enhanced_message.uniqueId, "OccurrenceConstraintViolation", e.what(), json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
    } catch (const ConnectorOutOfRangeException& e) {
        EVLOG_error << "Exception during handling of message: " << e.what();
        auto call_error = CallError(enhanced_message.uniqueId, "OccurrenceConstraintViolation", e.what(), json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
    } catch (const EnumConversionException& e) {
        EVLOG_error << "EnumConversionException during handling of message: " << e.what();
        auto call_error = CallError(enhanced_message.uniqueId, "FormationViolation", e.what(), json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
    } catch (const TimePointParseException& e) {
        EVLOG_error << "Exception during handling of message: " << e.what();
        auto call_error = CallError(enhanced_message.uniqueId, "FormationViolation", e.what(), json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
    } catch (json::exception& e) {
        EVLOG_error << "JSON exception during handling of message: " << e.what();
        if (json_message.is_array() and json_message.size() > MESSAGE_ID) {
            auto call_error = CallError(enhanced_message.uniqueId, "FormationViolation", e.what(), json({}));
            this->message_dispatcher->dispatch_call_error(call_error);
        }
    }
}

MeterValue ChargePoint::get_latest_meter_value_filtered(const MeterValue& meter_value, ReadingContextEnum context,
                                                        const RequiredComponentVariable& component_variable) {
    auto filtered_meter_value = utils::get_meter_value_with_measurands_applied(
        meter_value, utils::get_measurands_vec(this->device_model->get_value<std::string>(component_variable)));
    for (auto& sampled_value : filtered_meter_value.sampledValue) {
        sampled_value.context = context;
    }
    return filtered_meter_value;
}

void ChargePoint::change_all_connectors_to_unavailable_for_firmware_update() {
    ChangeAvailabilityResponse response;
    response.status = ChangeAvailabilityStatusEnum::Scheduled;

    ChangeAvailabilityRequest msg;
    msg.operationalStatus = OperationalStatusEnum::Inoperative;

    const auto transaction_active = this->evse_manager->any_transaction_active(std::nullopt);

    if (!transaction_active) {
        // execute change availability if possible
        for (auto& evse : *this->evse_manager) {
            if (!evse.has_active_transaction()) {
                set_evse_connectors_unavailable(evse, false);
            }
        }
        // Check succeeded, trigger the callback if needed
        if (this->callbacks.all_connectors_unavailable_callback.has_value() and
            this->evse_manager->are_all_connectors_effectively_inoperative()) {
            this->callbacks.all_connectors_unavailable_callback.value()();
        }
    } else if (response.status == ChangeAvailabilityStatusEnum::Scheduled) {
        // put all EVSEs to unavailable that do not have active transaction
        for (auto& evse : *this->evse_manager) {
            if (!evse.has_active_transaction()) {
                set_evse_connectors_unavailable(evse, false);
            } else {
                EVSE e;
                e.id = evse.get_id();
                msg.evse = e;
                this->availability->set_scheduled_change_availability_requests(evse.get_id(), {msg, false});
            }
        }
    }
}

void ChargePoint::restore_all_connector_states() {
    for (auto& evse : *this->evse_manager) {
        uint32_t number_of_connectors = evse.get_number_of_connectors();

        for (uint32_t i = 1; i <= number_of_connectors; ++i) {
            evse.restore_connector_operative_status(static_cast<int32_t>(i));
        }
    }
}

void ChargePoint::update_aligned_data_interval() {
    auto interval =
        std::chrono::seconds(this->device_model->get_value<int>(ControllerComponentVariables::AlignedDataInterval));
    if (interval <= 0s) {
        this->aligned_meter_values_timer.stop();
        return;
    }

    this->aligned_meter_values_timer.interval_starting_from(
        [this, interval]() {
            // J01.FR.20 if AlignedDataSendDuringIdle is true and any transaction is active, don't send clock aligned
            // meter values
            if (this->device_model->get_optional_value<bool>(ControllerComponentVariables::AlignedDataSendDuringIdle)
                    .value_or(false)) {
                for (auto const& evse : *this->evse_manager) {
                    if (evse.has_active_transaction()) {
                        return;
                    }
                }
            }

            const bool align_timestamps =
                this->device_model->get_optional_value<bool>(ControllerComponentVariables::RoundClockAlignedTimestamps)
                    .value_or(false);

            // send evseID = 0 values
            auto meter_value = get_latest_meter_value_filtered(this->aligned_data_evse0.retrieve_processed_values(),
                                                               ReadingContextEnum::Sample_Clock,
                                                               ControllerComponentVariables::AlignedDataMeasurands);

            if (!meter_value.sampledValue.empty()) {
                if (align_timestamps) {
                    meter_value.timestamp = utils::align_timestamp(DateTime{}, interval);
                }
                this->meter_values_req(0, std::vector<ocpp::v201::MeterValue>(1, meter_value));
            }
            this->aligned_data_evse0.clear_values();

            for (auto& evse : *this->evse_manager) {
                if (evse.has_active_transaction()) {
                    continue;
                }

                // this will apply configured measurands and possibly reduce the entries of sampledValue
                // according to the configuration
                auto meter_value =
                    get_latest_meter_value_filtered(evse.get_idle_meter_value(), ReadingContextEnum::Sample_Clock,
                                                    ControllerComponentVariables::AlignedDataMeasurands);

                if (align_timestamps) {
                    meter_value.timestamp = utils::align_timestamp(DateTime{}, interval);
                }

                if (!meter_value.sampledValue.empty()) {
                    // J01.FR.14 this is the only case where we send a MeterValue.req
                    this->meter_values_req(evse.get_id(), std::vector<ocpp::v201::MeterValue>(1, meter_value));
                    // clear the values
                }
                evse.clear_idle_meter_values();
            }
        },
        interval, std::chrono::floor<date::days>(date::utc_clock::to_sys(date::utc_clock::now())));
}

/**
 * Determine for a component variable whether it affects the Websocket Connection Options (cf.
 * get_ws_connection_options); return true if it is furthermore writable and does not require a reconnect
 *
 * @param component_variable
 * @return
 */
static bool component_variable_change_requires_websocket_option_update_without_reconnect(
    const ComponentVariable& component_variable) {

    return component_variable == ControllerComponentVariables::RetryBackOffRandomRange or
           component_variable == ControllerComponentVariables::RetryBackOffRepeatTimes or
           component_variable == ControllerComponentVariables::RetryBackOffWaitMinimum or
           component_variable == ControllerComponentVariables::NetworkProfileConnectionAttempts or
           component_variable == ControllerComponentVariables::WebSocketPingInterval;
}

void ChargePoint::handle_variable_changed(const SetVariableData& set_variable_data) {

    ComponentVariable component_variable = {set_variable_data.component, std::nullopt, set_variable_data.variable};

    if (set_variable_data.attributeType.has_value() and
        set_variable_data.attributeType.value() != AttributeEnum::Actual) {
        return;
    }

    if (component_variable == ControllerComponentVariables::BasicAuthPassword) {
        if (this->device_model->get_value<int>(ControllerComponentVariables::SecurityProfile) < 3) {
            // TODO: A01.FR.11 log the change of BasicAuth in Security Log
            this->connectivity_manager->set_websocket_authorization_key(set_variable_data.attributeValue.get());
        }
    }
    if (component_variable == ControllerComponentVariables::HeartbeatInterval and
        this->registration_status == RegistrationStatusEnum::Accepted) {
        try {
            this->availability->set_heartbeat_timer_interval(
                std::chrono::seconds(std::stoi(set_variable_data.attributeValue.get())));
        } catch (const std::invalid_argument& e) {
            EVLOG_error << "Invalid argument exception while updating the heartbeat interval: " << e.what();
        } catch (const std::out_of_range& e) {
            EVLOG_error << "Out of range exception while updating the heartbeat interval: " << e.what();
        }
    }
    if (component_variable == ControllerComponentVariables::AlignedDataInterval) {
        this->update_aligned_data_interval();
    }

    if (component_variable_change_requires_websocket_option_update_without_reconnect(component_variable)) {
        EVLOG_debug << "Reconfigure websocket due to relevant change of ControllerComponentVariable";
        this->connectivity_manager->set_websocket_connection_options_without_reconnect();
    }

    if (component_variable == ControllerComponentVariables::MessageAttemptInterval) {
        if (component_variable.variable.has_value()) {
            this->message_queue->update_transaction_message_retry_interval(
                this->device_model->get_value<int>(ControllerComponentVariables::MessageAttemptInterval));
        }
    }

    if (component_variable == ControllerComponentVariables::MessageAttempts) {
        if (component_variable.variable.has_value()) {
            this->message_queue->update_transaction_message_attempts(
                this->device_model->get_value<int>(ControllerComponentVariables::MessageAttempts));
        }
    }

    if (component_variable == ControllerComponentVariables::MessageTimeout) {
        if (component_variable.variable.has_value()) {
            this->message_queue->update_message_timeout(
                this->device_model->get_value<int>(ControllerComponentVariables::MessageTimeout));
        }
    }

    // TODO(piet): other special handling of changed variables can be added here...
}

void ChargePoint::handle_variables_changed(const std::map<SetVariableData, SetVariableResult>& set_variable_results) {
    // iterate over set_variable_results
    for (const auto& [set_variable_data, set_variable_result] : set_variable_results) {
        if (set_variable_result.attributeStatus == SetVariableStatusEnum::Accepted) {
            std::optional<MutabilityEnum> mutability =
                this->device_model->get_mutability(set_variable_data.component, set_variable_data.variable,
                                                   set_variable_data.attributeType.value_or(AttributeEnum::Actual));
            // If a nullopt is returned for whatever reason, assume it's write-only to prevent leaking secrets
            if (!mutability.has_value() || (mutability.value() == MutabilityEnum::WriteOnly)) {
                EVLOG_info << "Write-only " << set_variable_data.component.name << ":"
                           << set_variable_data.variable.name << " changed";
            } else {
                EVLOG_info << set_variable_data.component.name << ":" << set_variable_data.variable.name
                           << " changed to " << set_variable_data.attributeValue.get();
            }

            // handles required behavior specified within OCPP2.0.1 (e.g. reconnect when BasicAuthPassword has changed)
            this->handle_variable_changed(set_variable_data);
            // notifies libocpp user application that a variable has changed
            if (this->callbacks.variable_changed_callback.has_value()) {
                this->callbacks.variable_changed_callback.value()(set_variable_data);
            }
        }
    }

    // process all triggered monitors, after a possible disconnect
    this->monitoring_updater.process_triggered_monitors();
}

bool ChargePoint::validate_set_variable(const SetVariableData& set_variable_data) {
    ComponentVariable cv = {set_variable_data.component, std::nullopt, set_variable_data.variable};
    if (cv == ControllerComponentVariables::NetworkConfigurationPriority) {
        const auto network_configuration_priorities = ocpp::split_string(set_variable_data.attributeValue.get(), ',');
        const auto active_security_profile =
            this->device_model->get_value<int>(ControllerComponentVariables::SecurityProfile);

        try {
            const auto network_connection_profiles = json::parse(
                this->device_model->get_value<std::string>(ControllerComponentVariables::NetworkConnectionProfiles));
            for (const auto configuration_slot : network_configuration_priorities) {
                auto network_profile_it =
                    std::find_if(network_connection_profiles.begin(), network_connection_profiles.end(),
                                 [configuration_slot](const SetNetworkProfileRequest& network_profile) {
                                     return network_profile.configurationSlot == std::stoi(configuration_slot);
                                 });

                if (network_profile_it == network_connection_profiles.end()) {
                    EVLOG_warning << "Could not find network profile for configurationSlot: " << configuration_slot;
                    return false;
                }

                auto network_profile = SetNetworkProfileRequest(*network_profile_it).connectionData;

                if (network_profile.securityProfile <= active_security_profile) {
                    continue;
                }

                if (network_profile.securityProfile == 3 and
                    this->evse_security
                            ->get_leaf_certificate_info(ocpp::CertificateSigningUseEnum::ChargingStationCertificate)
                            .status != ocpp::GetCertificateInfoStatus::Accepted) {
                    EVLOG_warning << "SecurityProfile of configurationSlot: " << configuration_slot
                                  << " is 3 but no CSMS Leaf Certificate is installed";
                    return false;
                }
                if (network_profile.securityProfile >= 2 and
                    !this->evse_security->is_ca_certificate_installed(ocpp::CaCertificateType::CSMS)) {
                    EVLOG_warning << "SecurityProfile of configurationSlot: " << configuration_slot
                                  << " is >= 2 but no CSMS Root Certifciate is installed";
                    return false;
                }
            }
        } catch (const std::invalid_argument& e) {
            EVLOG_warning << "NetworkConfigurationPriority contains at least one value which is not an integer: "
                          << set_variable_data.attributeValue.get();
            return false;
        } catch (const json::exception& e) {
            EVLOG_warning << "Could not parse NetworkConnectionProfiles or SetNetworkProfileRequest: " << e.what();
            return false;
        }
    }
    return true;
    // TODO(piet): other special validating of variables requested to change can be added here...
}

std::map<SetVariableData, SetVariableResult>
ChargePoint::set_variables_internal(const std::vector<SetVariableData>& set_variable_data_vector,
                                    const std::string& source, const bool allow_read_only) {
    std::map<SetVariableData, SetVariableResult> response;

    // iterate over the set_variable_data_vector
    for (const auto& set_variable_data : set_variable_data_vector) {
        SetVariableResult set_variable_result;
        set_variable_result.component = set_variable_data.component;
        set_variable_result.variable = set_variable_data.variable;
        set_variable_result.attributeType = set_variable_data.attributeType.value_or(AttributeEnum::Actual);

        // validates variable against business logic of the spec
        if (this->validate_set_variable(set_variable_data)) {
            // attempt to set the value includes device model validation
            set_variable_result.attributeStatus =
                this->device_model->set_value(set_variable_data.component, set_variable_data.variable,
                                              set_variable_data.attributeType.value_or(AttributeEnum::Actual),
                                              set_variable_data.attributeValue.get(), source, allow_read_only);
        } else {
            set_variable_result.attributeStatus = SetVariableStatusEnum::Rejected;
        }
        response[set_variable_data] = set_variable_result;
    }

    return response;
}

ocpp::ReservationCheckStatus
ChargePoint::is_evse_reserved_for_other(EvseInterface& evse, const IdToken& id_token,
                                        const std::optional<IdToken>& group_id_token) const {
    if (this->reservation != nullptr) {
        return this->reservation->is_evse_reserved_for_other(evse, id_token, group_id_token);
    }

    return ReservationCheckStatus::NotReserved;
}

bool ChargePoint::is_evse_connector_available(EvseInterface& evse) const {
    if (evse.has_active_transaction()) {
        // If an EV is connected and has no authorization yet then the status is 'Occupied' and the
        // RemoteStartRequest should still be accepted. So this is the 'occupied' check instead.
        return false;
    }

    const uint32_t connectors = evse.get_number_of_connectors();
    for (uint32_t i = 1; i <= connectors; ++i) {
        const ConnectorStatusEnum status =
            evse.get_connector(static_cast<int32_t>(i))->get_effective_connector_status();

        // At least one of the connectors is available / not faulted.
        if (status != ConnectorStatusEnum::Faulted and status != ConnectorStatusEnum::Unavailable) {
            return true;
        }
    }

    // Connectors are faulted or unavailable.
    return false;
}

bool ChargePoint::does_connector_exist(const uint32_t evse_id, std::optional<ConnectorEnum> connector_type) {
    EvseInterface* evse;
    try {
        evse = &evse_manager->get_evse(static_cast<int32_t>(evse_id));
    } catch (const EvseOutOfRangeException&) {
        EVLOG_error << "Evse id " << evse_id << " is not a valid evse id.";
        return false;
    }

    return evse->does_connector_exist(connector_type.value_or(ConnectorEnum::Unknown));
}

bool ChargePoint::is_offline() {
    return !this->connectivity_manager->is_websocket_connected();
}

void ChargePoint::boot_notification_req(const BootReasonEnum& reason, const bool initiated_by_trigger_message) {
    EVLOG_debug << "Sending BootNotification";
    BootNotificationRequest req;

    ChargingStation charging_station;
    charging_station.model = this->device_model->get_value<std::string>(ControllerComponentVariables::ChargePointModel);
    charging_station.vendorName =
        this->device_model->get_value<std::string>(ControllerComponentVariables::ChargePointVendor);
    charging_station.firmwareVersion.emplace(
        this->device_model->get_value<std::string>(ControllerComponentVariables::FirmwareVersion));
    charging_station.serialNumber.emplace(
        this->device_model->get_value<std::string>(ControllerComponentVariables::ChargeBoxSerialNumber));

    req.reason = reason;
    req.chargingStation = charging_station;

    ocpp::Call<BootNotificationRequest> call(req);
    this->message_dispatcher->dispatch_call(call, initiated_by_trigger_message);
}

void ChargePoint::notify_report_req(const int request_id, const std::vector<ReportData>& report_data) {

    NotifyReportRequest req;
    req.requestId = request_id;
    req.seqNo = 0;
    req.generatedAt = ocpp::DateTime();
    req.reportData.emplace(report_data);
    req.tbc = false;

    if (report_data.size() <= 1) {
        ocpp::Call<NotifyReportRequest> call(req);
        this->message_dispatcher->dispatch_call(call);
    } else {
        NotifyReportRequestsSplitter splitter{
            req,
            this->device_model->get_optional_value<size_t>(ControllerComponentVariables::MaxMessageSize)
                .value_or(DEFAULT_MAX_MESSAGE_SIZE),
            [this]() { return ocpp::create_message_id(); }};
        for (const auto& msg : splitter.create_call_payloads()) {
            this->message_queue->push_call(msg);
        }
    }
}

void ChargePoint::transaction_event_req(const TransactionEventEnum& event_type, const DateTime& timestamp,
                                        const ocpp::v201::Transaction& transaction,
                                        const ocpp::v201::TriggerReasonEnum& trigger_reason, const int32_t seq_no,
                                        const std::optional<int32_t>& cable_max_current,
                                        const std::optional<ocpp::v201::EVSE>& evse,
                                        const std::optional<ocpp::v201::IdToken>& id_token,
                                        const std::optional<std::vector<ocpp::v201::MeterValue>>& meter_value,
                                        const std::optional<int32_t>& number_of_phases_used, const bool offline,
                                        const std::optional<int32_t>& reservation_id,
                                        const bool initiated_by_trigger_message) {
    TransactionEventRequest req;
    req.eventType = event_type;
    req.timestamp = timestamp;
    req.transactionInfo = transaction;
    req.triggerReason = trigger_reason;
    req.seqNo = seq_no;
    req.cableMaxCurrent = cable_max_current;
    req.evse = evse;
    req.idToken = id_token;
    req.meterValue = meter_value;
    req.numberOfPhasesUsed = number_of_phases_used;
    req.offline = offline;
    req.reservationId = reservation_id;

    ocpp::Call<TransactionEventRequest> call(req);

    // Check if id token is in the remote start map, because when a remote
    // start request is done, the first transaction event request should
    // always contain trigger reason 'RemoteStart'.
    auto it = std::find_if(
        remote_start_id_per_evse.begin(), remote_start_id_per_evse.end(),
        [&id_token, &evse](const std::pair<int32_t, std::pair<IdToken, int32_t>>& remote_start_per_evse) {
            if (id_token.has_value() and remote_start_per_evse.second.first.idToken == id_token.value().idToken) {

                if (remote_start_per_evse.first == 0) {
                    return true;
                }

                if (evse.has_value() and evse.value().id == remote_start_per_evse.first) {
                    return true;
                }
            }
            return false;
        });

    if (it != remote_start_id_per_evse.end()) {
        // Found remote start. Set remote start id and the trigger reason.
        call.msg.triggerReason = TriggerReasonEnum::RemoteStart;
        call.msg.transactionInfo.remoteStartId = it->second.second;

        remote_start_id_per_evse.erase(it);
    }

    this->message_dispatcher->dispatch_call(call, initiated_by_trigger_message);

    if (this->callbacks.transaction_event_callback.has_value()) {
        this->callbacks.transaction_event_callback.value()(req);
    }
}

void ChargePoint::meter_values_req(const int32_t evse_id, const std::vector<MeterValue>& meter_values,
                                   const bool initiated_by_trigger_message) {
    MeterValuesRequest req;
    req.evseId = evse_id;
    req.meterValue = meter_values;

    ocpp::Call<MeterValuesRequest> call(req);
    this->message_dispatcher->dispatch_call(call, initiated_by_trigger_message);
}

void ChargePoint::report_charging_profile_req(const int32_t request_id, const int32_t evse_id,
                                              const ChargingLimitSourceEnum source,
                                              const std::vector<ChargingProfile>& profiles, const bool tbc) {
    ReportChargingProfilesRequest req;
    req.requestId = request_id;
    req.evseId = evse_id;
    req.chargingLimitSource = source;
    req.chargingProfile = profiles;
    req.tbc = tbc;

    ocpp::Call<ReportChargingProfilesRequest> call(req);
    this->message_dispatcher->dispatch_call(call);
}

void ChargePoint::report_charging_profile_req(const ReportChargingProfilesRequest& req) {
    ocpp::Call<ReportChargingProfilesRequest> call(req);
    this->message_dispatcher->dispatch_call(call);
}

void ChargePoint::notify_event_req(const std::vector<EventData>& events) {
    NotifyEventRequest req;
    req.eventData = events;
    req.generatedAt = DateTime();
    req.seqNo = 0;

    ocpp::Call<NotifyEventRequest> call(req);
    this->message_dispatcher->dispatch_call(call);
}

void ChargePoint::notify_customer_information_req(const std::string& data, const int32_t request_id) {
    size_t pos = 0;
    int32_t seq_no = 0;
    while (pos < data.length() or pos == 0 and data.empty()) {
        const auto req = [&]() {
            NotifyCustomerInformationRequest req;
            req.data = CiString<512>(data.substr(pos, 512));
            req.seqNo = seq_no;
            req.requestId = request_id;
            req.generatedAt = DateTime();
            req.tbc = data.length() - pos > 512;
            return req;
        }();

        ocpp::Call<NotifyCustomerInformationRequest> call(req);
        this->message_dispatcher->dispatch_call(call);

        pos += 512;
        seq_no++;
    }
}

void ChargePoint::handle_boot_notification_response(CallResult<BootNotificationResponse> call_result) {
    // TODO(piet): B01.FR.06
    // TODO(piet): B01.FR.07
    // TODO(piet): B01.FR.08
    // TODO(piet): B01.FR.09
    // TODO(piet): B01.FR.13
    EVLOG_info << "Received BootNotificationResponse: " << call_result.msg
               << "\nwith messageId: " << call_result.uniqueId;

    const auto msg = call_result.msg;

    this->registration_status = msg.status;

    if (this->registration_status == RegistrationStatusEnum::Accepted) {
        this->message_queue->set_registration_status_accepted();
        // B01.FR.06 Only use boot timestamp if TimeSource contains Heartbeat
        if (this->callbacks.time_sync_callback.has_value() and
            this->device_model->get_value<std::string>(ControllerComponentVariables::TimeSource).find("Heartbeat") !=
                std::string::npos) {
            this->callbacks.time_sync_callback.value()(msg.currentTime);
        }

        this->connectivity_manager->confirm_successful_connection();

        // set timers
        if (msg.interval > 0) {
            this->availability->set_heartbeat_timer_interval(std::chrono::seconds(msg.interval));
        }

        // in case the BootNotification.req was triggered by a TriggerMessage.req the timer might still run
        this->boot_notification_timer.stop();

        this->security->init_certificate_expiration_check_timers();
        this->update_aligned_data_interval();
        this->component_state_manager->send_status_notification_all_connectors();
        this->ocsp_updater.start();
    } else {
        auto retry_interval = DEFAULT_BOOT_NOTIFICATION_RETRY_INTERVAL;
        if (msg.interval > 0) {
            retry_interval = std::chrono::seconds(msg.interval);
        }
        this->boot_notification_timer.timeout(
            [this, msg]() {
                this->boot_notification_req(BootReasonEnum::PowerUp); // FIXME(piet): Choose correct reason here
            },
            retry_interval);
    }

    if (this->callbacks.boot_notification_callback.has_value()) {
        // call the registered boot notification callback
        callbacks.boot_notification_callback.value()(call_result.msg);
    }
}

void ChargePoint::handle_set_variables_req(Call<SetVariablesRequest> call) {
    const auto msg = call.msg;

    SetVariablesResponse response;

    // set variables but do not allow setting ReadOnly variables
    const auto set_variables_response =
        this->set_variables_internal(msg.setVariableData, VARIABLE_ATTRIBUTE_VALUE_SOURCE_CSMS, false);
    for (const auto& [single_set_variable_data, single_set_variable_result] : set_variables_response) {
        response.setVariableResult.push_back(single_set_variable_result);
    }

    ocpp::CallResult<SetVariablesResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    // post handling of changed variables after the SetVariables.conf has been queued
    this->handle_variables_changed(set_variables_response);
}

void ChargePoint::handle_get_variables_req(const EnhancedMessage<v201::MessageType>& message) {
    Call<GetVariablesRequest> call = message.call_message;
    const auto msg = call.msg;

    const auto max_variables_per_message =
        this->device_model->get_value<int>(ControllerComponentVariables::ItemsPerMessageGetVariables);
    const auto max_bytes_per_message =
        this->device_model->get_value<int>(ControllerComponentVariables::BytesPerMessageGetVariables);

    // B06.FR.16
    if (msg.getVariableData.size() > max_variables_per_message) {
        // send a CALLERROR
        const auto call_error = CallError(call.uniqueId, "OccurenceConstraintViolation", "", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
        return;
    }

    // B06.FR.17
    if (message.message_size > max_bytes_per_message) {
        // send a CALLERROR
        const auto call_error = CallError(call.uniqueId, "FormatViolation", "", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
        return;
    }

    GetVariablesResponse response;
    response.getVariableResult = this->get_variables(msg.getVariableData);

    ocpp::CallResult<GetVariablesResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_get_base_report_req(Call<GetBaseReportRequest> call) {
    const auto msg = call.msg;
    GetBaseReportResponse response;
    response.status = GenericDeviceModelStatusEnum::Accepted;

    ocpp::CallResult<GetBaseReportResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    if (response.status == GenericDeviceModelStatusEnum::Accepted) {
        const auto report_data = this->device_model->get_base_report_data(msg.reportBase);
        this->notify_report_req(msg.requestId, report_data);
    }
}

void ChargePoint::handle_get_report_req(const EnhancedMessage<v201::MessageType>& message) {
    Call<GetReportRequest> call = message.call_message;
    const auto msg = call.msg;
    std::vector<ReportData> report_data;
    GetReportResponse response;

    const auto max_items_per_message =
        this->device_model->get_value<int>(ControllerComponentVariables::ItemsPerMessageGetReport);
    const auto max_bytes_per_message =
        this->device_model->get_value<int>(ControllerComponentVariables::BytesPerMessageGetReport);

    // B08.FR.17
    if (msg.componentVariable.has_value() and msg.componentVariable->size() > max_items_per_message) {
        // send a CALLERROR
        const auto call_error = CallError(call.uniqueId, "OccurenceConstraintViolation", "", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
        return;
    }

    // B08.FR.18
    if (message.message_size > max_bytes_per_message) {
        // send a CALLERROR
        const auto call_error = CallError(call.uniqueId, "FormatViolation", "", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
        return;
    }

    // if a criteria is not supported then send a not supported response.
    auto sup_criteria =
        this->device_model->get_optional_value<std::string>(ControllerComponentVariables::SupportedCriteria);
    if (sup_criteria.has_value() and msg.componentCriteria.has_value()) {
        for (const auto& criteria : msg.componentCriteria.value()) {
            const auto variable_ = conversions::component_criterion_enum_to_string(criteria);
            if (sup_criteria.value().find(variable_) == std::string::npos) {
                EVLOG_info << "This criteria is not supported: " << variable_;
                response.status = GenericDeviceModelStatusEnum::NotSupported;
                break;
                // TODO: maybe consider adding the reason why in statusInfo
            }
        }
    }

    if (response.status != GenericDeviceModelStatusEnum::NotSupported) {

        // TODO(piet): Propably split this up into several NotifyReport.req depending on ItemsPerMessage /
        // BytesPerMessage
        report_data = this->device_model->get_custom_report_data(msg.componentVariable, msg.componentCriteria);
        if (report_data.empty()) {
            response.status = GenericDeviceModelStatusEnum::EmptyResultSet;
        } else {
            response.status = GenericDeviceModelStatusEnum::Accepted;
        }
    }

    ocpp::CallResult<GetReportResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    if (response.status == GenericDeviceModelStatusEnum::Accepted) {
        this->notify_report_req(msg.requestId, report_data);
    }
}

void ChargePoint::handle_set_network_profile_req(Call<SetNetworkProfileRequest> call) {
    const auto msg = call.msg;

    SetNetworkProfileResponse response;

    if (!this->callbacks.validate_network_profile_callback.has_value()) {
        EVLOG_warning << "No callback registered to validate network profile";
        response.status = SetNetworkProfileStatusEnum::Rejected;
        ocpp::CallResult<SetNetworkProfileResponse> call_result(response, call.uniqueId);
        this->message_dispatcher->dispatch_call_result(call_result);
        return;
    }

    if (msg.connectionData.securityProfile <
        this->device_model->get_value<int>(ControllerComponentVariables::SecurityProfile)) {
        EVLOG_warning << "CSMS attempted to set a network profile with a lower securityProfile";
        response.status = SetNetworkProfileStatusEnum::Rejected;
        ocpp::CallResult<SetNetworkProfileResponse> call_result(response, call.uniqueId);
        this->message_dispatcher->dispatch_call_result(call_result);
        return;
    }

    if (this->callbacks.validate_network_profile_callback.value()(msg.configurationSlot, msg.connectionData) !=
        SetNetworkProfileStatusEnum::Accepted) {
        EVLOG_warning << "CSMS attempted to set a network profile that could not be validated.";
        response.status = SetNetworkProfileStatusEnum::Rejected;
        ocpp::CallResult<SetNetworkProfileResponse> call_result(response, call.uniqueId);
        this->message_dispatcher->dispatch_call_result(call_result);
        return;
    }

    auto network_connection_profiles = json::parse(
        this->device_model->get_value<std::string>(ControllerComponentVariables::NetworkConnectionProfiles));

    int index_to_override = -1;
    int index = 0;
    for (const SetNetworkProfileRequest network_profile : network_connection_profiles) {
        if (network_profile.configurationSlot == msg.configurationSlot) {
            index_to_override = index;
        }
        index++;
    }

    if (index_to_override != -1) {
        // configurationSlot present, so we override
        network_connection_profiles[index_to_override] = msg;
    } else {
        // configurationSlot not present, so we can append
        network_connection_profiles.push_back(msg);
    }

    if (this->device_model->set_value(ControllerComponentVariables::NetworkConnectionProfiles.component,
                                      ControllerComponentVariables::NetworkConnectionProfiles.variable.value(),
                                      AttributeEnum::Actual, network_connection_profiles.dump(),
                                      VARIABLE_ATTRIBUTE_VALUE_SOURCE_INTERNAL) != SetVariableStatusEnum::Accepted) {
        EVLOG_warning << "CSMS attempted to set a network profile that could not be written to the device model";
        response.status = SetNetworkProfileStatusEnum::Rejected;
        ocpp::CallResult<SetNetworkProfileResponse> call_result(response, call.uniqueId);
        this->message_dispatcher->dispatch_call_result(call_result);
        return;
    }

    std::string tech_info = "Received and stored a new network connection profile at configurationSlot: " +
                            std::to_string(msg.configurationSlot);
    EVLOG_info << tech_info;

    const auto& security_event = ocpp::security_events::RECONFIGURATIONOFSECURITYPARAMETERS;
    this->security->security_event_notification_req(CiString<50>(security_event), CiString<255>(tech_info), true,
                                                    utils::is_critical(security_event));

    response.status = SetNetworkProfileStatusEnum::Accepted;
    ocpp::CallResult<SetNetworkProfileResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_reset_req(Call<ResetRequest> call) {
    // TODO(piet): B11.FR.05

    // TODO(piet): B12.FR.05
    // TODO(piet): B12.FR.06
    EVLOG_debug << "Received ResetRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    const auto msg = call.msg;

    ResetResponse response;

    // Check if there is an active transaction (on the given evse or if not
    // given, on one of the evse's)
    bool transaction_active = false;
    std::set<int32_t> evse_active_transactions;
    std::set<int32_t> evse_no_transactions;
    if (msg.evseId.has_value() and this->evse_manager->get_evse(msg.evseId.value()).has_active_transaction()) {
        transaction_active = true;
        evse_active_transactions.emplace(msg.evseId.value());
    } else {
        for (const auto& evse : *this->evse_manager) {
            if (evse.has_active_transaction()) {
                transaction_active = true;
                evse_active_transactions.emplace(evse.get_id());
            } else {
                evse_no_transactions.emplace(evse.get_id());
            }
        }
    }

    const auto is_reset_allowed = [&]() {
        if (!this->callbacks.is_reset_allowed_callback(msg.evseId, msg.type)) {
            return false;
        }

        // We dont need to check AllowReset if evseId is not set and can directly return true
        if (!msg.evseId.has_value()) {
            return true;
        }

        // B11.FR.10
        const auto allow_reset_cv =
            EvseComponentVariables::get_component_variable(msg.evseId.value(), EvseComponentVariables::AllowReset);
        // allow reset if AllowReset is not set or set to   true
        return this->device_model->get_optional_value<bool>(allow_reset_cv).value_or(true);
    };

    if (is_reset_allowed()) {
        // reset is allowed
        response.status = ResetStatusEnum::Accepted;
    } else {
        response.status = ResetStatusEnum::Rejected;
    }

    if (response.status == ResetStatusEnum::Accepted and transaction_active and msg.type == ResetEnum::OnIdle) {
        if (msg.evseId.has_value()) {
            // B12.FR.07
            this->reset_scheduled_evseids.insert(msg.evseId.value());
        }

        // B12.FR.01: We have to wait until transactions have ended.
        // B12.FR.07
        this->reset_scheduled = true;
        response.status = ResetStatusEnum::Scheduled;
    }

    ocpp::CallResult<ResetResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    // Reset response is sent, now set evse connectors to unavailable and / or
    // stop transaction (depending on reset type)
    if (response.status != ResetStatusEnum::Rejected and transaction_active) {
        if (msg.type == ResetEnum::Immediate) {
            // B12.FR.08 and B12.FR.04
            for (const int32_t evse_id : evse_active_transactions) {
                callbacks.stop_transaction_callback(evse_id, ReasonEnum::ImmediateReset);
            }
        } else if (msg.type == ResetEnum::OnIdle and !evse_no_transactions.empty()) {
            for (const int32_t evse_id : evse_no_transactions) {
                auto& evse = this->evse_manager->get_evse(evse_id);
                set_evse_connectors_unavailable(evse, false);
            }
        }
    }

    if (response.status == ResetStatusEnum::Accepted) {
        this->callbacks.reset_callback(call.msg.evseId, ResetEnum::Immediate);
    }
}

void ChargePoint::handle_transaction_event_response(const EnhancedMessage<v201::MessageType>& message) {
    CallResult<TransactionEventResponse> call_result = message.message;
    const Call<TransactionEventRequest>& original_call = message.call_message;
    const auto& original_msg = original_call.msg;

    if (this->callbacks.transaction_event_response_callback.has_value()) {
        this->callbacks.transaction_event_response_callback.value()(original_msg, call_result.msg);
    }

    this->handle_cost_and_tariff(call_result.msg, original_msg, message.message[CALLRESULT_PAYLOAD]);

    if (original_msg.eventType == TransactionEventEnum::Ended) {
        // nothing to do for TransactionEventEnum::Ended
        return;
    }

    const auto msg = call_result.msg;

    if (!msg.idTokenInfo.has_value()) {
        // nothing to do when the response does not contain idTokenInfo
        return;
    }

    if (!original_msg.idToken.has_value()) {
        EVLOG_error
            << "TransactionEvent.conf contains idTokenInfo when no idToken was part of the TransactionEvent.req";
        return;
    }

    const IdToken& id_token = original_msg.idToken.value();

    // C03.FR.0x and C05.FR.01: We SHALL NOT store central information in the Authorization Cache
    // C10.FR.05
    if (id_token.type != IdTokenEnum::Central and authorization->is_auth_cache_ctrlr_enabled()) {
        try {
            this->authorization->authorization_cache_insert_entry(utils::generate_token_hash(id_token),
                                                                  msg.idTokenInfo.value());
        } catch (const DatabaseException& e) {
            EVLOG_warning << "Could not insert into authorization cache entry: " << e.what();
        }
        this->authorization->trigger_authorization_cache_cleanup();
    }

    if (msg.idTokenInfo.value().status == AuthorizationStatusEnum::Accepted) {
        // nothing to do in case status is accepted
        return;
    }

    for (auto& evse : *this->evse_manager) {
        if (auto& transaction = evse.get_transaction();
            transaction != nullptr and transaction->transactionId == original_msg.transactionInfo.transactionId) {
            // Deal with invalid token for transaction
            auto evse_id = evse.get_id();
            if (this->device_model->get_value<bool>(ControllerComponentVariables::StopTxOnInvalidId)) {
                this->callbacks.stop_transaction_callback(evse_id, ReasonEnum::DeAuthorized);
            } else {
                if (this->device_model->get_optional_value<int32_t>(ControllerComponentVariables::MaxEnergyOnInvalidId)
                        .has_value()) {
                    // Energy delivery to the EV SHALL be allowed until the amount of energy specified in
                    // MaxEnergyOnInvalidId has been reached.
                    evse.start_checking_max_energy_on_invalid_id();
                } else {
                    this->callbacks.pause_charging_callback(evse_id);
                }
            }
            break;
        }
    }
}

void ChargePoint::handle_get_transaction_status(const Call<GetTransactionStatusRequest> call) {
    const auto msg = call.msg;

    GetTransactionStatusResponse response;
    response.messagesInQueue = false;

    if (msg.transactionId.has_value()) {
        if (this->evse_manager->get_transaction_evseid(msg.transactionId.value()).has_value()) {
            response.ongoingIndicator = true;
        } else {
            response.ongoingIndicator = false;
        }
        if (this->message_queue->contains_transaction_messages(msg.transactionId.value())) {
            response.messagesInQueue = true;
        }
    } else if (!this->message_queue->is_transaction_message_queue_empty()) {
        response.messagesInQueue = true;
    }

    ocpp::CallResult<GetTransactionStatusResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_unlock_connector(Call<UnlockConnectorRequest> call) {
    const UnlockConnectorRequest& msg = call.msg;
    UnlockConnectorResponse unlock_response;

    EVSE evse = {msg.evseId, std::nullopt, msg.connectorId};

    if (this->evse_manager->is_valid_evse(evse)) {
        if (!this->evse_manager->get_evse(msg.evseId).has_active_transaction()) {
            unlock_response = callbacks.unlock_connector_callback(msg.evseId, msg.connectorId);
        } else {
            unlock_response.status = UnlockStatusEnum::OngoingAuthorizedTransaction;
        }
    } else {
        unlock_response.status = UnlockStatusEnum::UnknownConnector;
    }

    ocpp::CallResult<UnlockConnectorResponse> call_result(unlock_response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_trigger_message(Call<TriggerMessageRequest> call) {
    const TriggerMessageRequest& msg = call.msg;
    TriggerMessageResponse response;
    EvseInterface* evse_ptr = nullptr;

    response.status = TriggerMessageStatusEnum::Rejected;

    if (msg.evse.has_value()) {
        int32_t evse_id = msg.evse.value().id;
        evse_ptr = &this->evse_manager->get_evse(evse_id);
    }

    // F06.FR.04: First send the TriggerMessageResponse before sending the requested message
    //            so we split the functionality to be able to determine if we need to respond first.
    switch (msg.requestedMessage) {
    case MessageTriggerEnum::BootNotification:
        // F06.FR.17: Respond with rejected in case registration status is already accepted
        if (this->registration_status != RegistrationStatusEnum::Accepted) {
            response.status = TriggerMessageStatusEnum::Accepted;
        }
        break;

    case MessageTriggerEnum::LogStatusNotification:
    case MessageTriggerEnum::Heartbeat:
    case MessageTriggerEnum::FirmwareStatusNotification:
        response.status = TriggerMessageStatusEnum::Accepted;
        break;

    case MessageTriggerEnum::MeterValues:
        if (msg.evse.has_value()) {
            if (evse_ptr != nullptr and
                utils::meter_value_has_any_measurand(
                    evse_ptr->get_meter_value(), utils::get_measurands_vec(this->device_model->get_value<std::string>(
                                                     ControllerComponentVariables::AlignedDataMeasurands)))) {
                response.status = TriggerMessageStatusEnum::Accepted;
            }
        } else {
            const auto measurands = utils::get_measurands_vec(
                this->device_model->get_value<std::string>(ControllerComponentVariables::AlignedDataMeasurands));
            for (auto& evse : *this->evse_manager) {
                if (utils::meter_value_has_any_measurand(evse.get_meter_value(), measurands)) {
                    response.status = TriggerMessageStatusEnum::Accepted;
                    break;
                }
            }
        }
        break;

    case MessageTriggerEnum::TransactionEvent:
        if (msg.evse.has_value()) {
            if (evse_ptr != nullptr and evse_ptr->has_active_transaction()) {
                response.status = TriggerMessageStatusEnum::Accepted;
            }
        } else {
            for (auto const& evse : *this->evse_manager) {
                if (evse.has_active_transaction()) {
                    response.status = TriggerMessageStatusEnum::Accepted;
                    break;
                }
            }
        }
        break;

    case MessageTriggerEnum::StatusNotification:
        if (msg.evse.has_value() and msg.evse.value().connectorId.has_value()) {
            int32_t connector_id = msg.evse.value().connectorId.value();
            if (evse_ptr != nullptr and connector_id > 0 and connector_id <= evse_ptr->get_number_of_connectors()) {
                response.status = TriggerMessageStatusEnum::Accepted;
            }
        } else {
            // F06.FR.12: Reject if evse or connectorId is ommited
        }
        break;

    case MessageTriggerEnum::SignChargingStationCertificate:
        response.status = TriggerMessageStatusEnum::Accepted;
        break;
    case MessageTriggerEnum::SignV2GCertificate:
        if (this->device_model
                ->get_optional_value<bool>(ControllerComponentVariables::V2GCertificateInstallationEnabled)
                .value_or(false)) {
            response.status = TriggerMessageStatusEnum::Accepted;
        } else {
            EVLOG_warning << "CSMS requested SignV2GCertificate but V2GCertificateInstallationEnabled is configured as "
                             "false, so the TriggerMessage is rejected!";
            response.status = TriggerMessageStatusEnum::Rejected;
        }

        break;
        // TODO:
        // PublishFirmwareStatusNotification
        // SignCombinedCertificate

    default:
        response.status = TriggerMessageStatusEnum::NotImplemented;
        break;
    }

    ocpp::CallResult<TriggerMessageResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    if (response.status != TriggerMessageStatusEnum::Accepted) {
        return;
    }

    auto send_evse_message = [&](std::function<void(int32_t evse_id, EvseInterface & evse)> send) {
        if (evse_ptr != nullptr) {
            send(msg.evse.value().id, *evse_ptr);
        } else {
            for (auto& evse : *this->evse_manager) {
                send(evse.get_id(), evse);
            }
        }
    };

    switch (msg.requestedMessage) {
    case MessageTriggerEnum::BootNotification:
        boot_notification_req(BootReasonEnum::Triggered);
        break;

    case MessageTriggerEnum::MeterValues: {
        auto send_meter_value = [&](int32_t evse_id, EvseInterface& evse) {
            const auto meter_value =
                get_latest_meter_value_filtered(evse.get_meter_value(), ReadingContextEnum::Trigger,
                                                ControllerComponentVariables::AlignedDataMeasurands);

            if (!meter_value.sampledValue.empty()) {
                this->meter_values_req(evse_id, std::vector<ocpp::v201::MeterValue>(1, meter_value), true);
            }
        };
        send_evse_message(send_meter_value);
    } break;

    case MessageTriggerEnum::TransactionEvent: {
        auto send_transaction = [&](int32_t evse_id, EvseInterface& evse) {
            if (!evse.has_active_transaction()) {
                return;
            }

            const auto meter_value =
                get_latest_meter_value_filtered(evse.get_meter_value(), ReadingContextEnum::Trigger,
                                                ControllerComponentVariables::SampledDataTxUpdatedMeasurands);

            std::optional<std::vector<MeterValue>> opt_meter_value;
            if (!meter_value.sampledValue.empty()) {
                opt_meter_value.emplace(1, meter_value);
            }
            const auto& enhanced_transaction = evse.get_transaction();
            this->transaction_event_req(TransactionEventEnum::Updated, DateTime(),
                                        enhanced_transaction->get_transaction(), TriggerReasonEnum::Trigger,
                                        enhanced_transaction->get_seq_no(), std::nullopt, std::nullopt, std::nullopt,
                                        opt_meter_value, std::nullopt, this->is_offline(), std::nullopt, true);
        };
        send_evse_message(send_transaction);
    } break;

    case MessageTriggerEnum::StatusNotification:
        if (evse_ptr != nullptr and msg.evse.value().connectorId.has_value()) {
            this->component_state_manager->send_status_notification_single_connector(
                msg.evse.value().id, msg.evse.value().connectorId.value());
        }
        break;

    case MessageTriggerEnum::Heartbeat:
        this->availability->heartbeat_req(true);
        break;

    case MessageTriggerEnum::LogStatusNotification: {
        LogStatusNotificationRequest request;
        if (this->upload_log_status == UploadLogStatusEnum::Uploading) {
            request.status = UploadLogStatusEnum::Uploading;
            request.requestId = this->upload_log_status_id;
        } else {
            request.status = UploadLogStatusEnum::Idle;
        }

        ocpp::Call<LogStatusNotificationRequest> call(request);
        this->message_dispatcher->dispatch_call(call, true);
    } break;

    case MessageTriggerEnum::FirmwareStatusNotification: {
        FirmwareStatusNotificationRequest request;
        switch (this->firmware_status) {
        case FirmwareStatusEnum::Idle:
        case FirmwareStatusEnum::Installed: // L01.FR.25
            request.status = FirmwareStatusEnum::Idle;
            // do not set requestId when idle: L01.FR.20
            break;

        default: // So not Idle or Installed                   // L01.FR.26
            request.status = this->firmware_status;
            request.requestId = this->firmware_status_id;
            break;
        }

        ocpp::Call<FirmwareStatusNotificationRequest> call(request);
        this->message_dispatcher->dispatch_call(call, true);
    } break;

    case MessageTriggerEnum::SignChargingStationCertificate: {
        this->security->sign_certificate_req(ocpp::CertificateSigningUseEnum::ChargingStationCertificate, true);
    } break;

    case MessageTriggerEnum::SignV2GCertificate: {
        this->security->sign_certificate_req(ocpp::CertificateSigningUseEnum::V2GCertificate, true);
    } break;

    default:
        EVLOG_error << "Sent a TriggerMessageResponse::Accepted while not following up with a message";
        break;
    }
}

void ChargePoint::handle_remote_start_transaction_request(Call<RequestStartTransactionRequest> call) {
    auto msg = call.msg;

    RequestStartTransactionResponse response;
    response.status = RequestStartStopStatusEnum::Rejected;

    // Check if evse id is given.
    if (msg.evseId.has_value()) {
        const int32_t evse_id = msg.evseId.value();
        auto& evse = this->evse_manager->get_evse(evse_id);

        // F01.FR.23: Faulted or unavailable. F01.FR.24 / F02.FR.25: Occupied. Send rejected.
        const bool available = is_evse_connector_available(evse);

        // When available but there was a reservation for another token id or group token id:
        //    send rejected (F01.FR.21 & F01.FR.22)
        ocpp::ReservationCheckStatus reservation_status =
            is_evse_reserved_for_other(evse, call.msg.idToken, call.msg.groupIdToken);

        const bool is_reserved = (reservation_status == ocpp::ReservationCheckStatus::ReservedForOtherToken);

        if (!available or is_reserved) {
            // Note: we only support TxStartPoint PowerPathClosed, so we did not implement starting a
            // transaction first (and send TransactionEventRequest (eventType = Started). Only if a transaction
            // is authorized, a TransactionEventRequest will be sent. Because of this, F01.FR.13 is not
            // implemented as well, because in the current situation, this is an impossible state. (TODO: when
            // more TxStartPoints are supported, add implementation for F01.FR.13 as well).
            EVLOG_info << "Remote start transaction requested, but connector is not available or reserved.";
        } else {
            // F02: No active transaction yet and there is an available connector, so just send 'accepted'.
            response.status = RequestStartStopStatusEnum::Accepted;

            remote_start_id_per_evse[evse_id] = {msg.idToken, msg.remoteStartId};
        }

        // F01.FR.26 If a Charging Station with support for Smart Charging receives a
        // RequestStartTransactionRequest with an invalid ChargingProfile: The Charging Station SHALL respond
        // with RequestStartTransactionResponse with status = Rejected and optionally with reasonCode =
        // "InvalidProfile" or "InvalidSchedule".

        bool is_smart_charging_enabled =
            this->device_model->get_optional_value<bool>(ControllerComponentVariables::SmartChargingCtrlrEnabled)
                .value_or(false);

        if (is_smart_charging_enabled) {
            if (msg.chargingProfile.has_value()) {

                auto charging_profile = msg.chargingProfile.value();

                if (charging_profile.chargingProfilePurpose == ChargingProfilePurposeEnum::TxProfile) {

                    const auto add_profile_response = this->smart_charging_handler->conform_validate_and_add_profile(
                        msg.chargingProfile.value(), evse_id, ChargingLimitSourceEnum::CSO,
                        AddChargingProfileSource::RequestStartTransactionRequest);
                    if (add_profile_response.status == ChargingProfileStatusEnum::Accepted) {
                        EVLOG_debug << "Accepting SetChargingProfileRequest";
                    } else {
                        EVLOG_debug << "Rejecting SetChargingProfileRequest:\n reasonCode: "
                                    << add_profile_response.statusInfo->reasonCode.get()
                                    << "\nadditionalInfo: " << add_profile_response.statusInfo->additionalInfo->get();
                        response.statusInfo = add_profile_response.statusInfo;
                    }
                }
            }
        }
    } else {
        // F01.FR.07 RequestStartTransactionRequest does not contain an evseId. The Charging Station MAY reject the
        // RequestStartTransactionRequest. We do this for now (send rejected) (TODO: eventually support the charging
        // station to accept no evse id. If so: add token and remote start id for evse id 0 to
        // remote_start_id_per_evse, so we know for '0' it means 'all evse id's').
        EVLOG_warning << "No evse id given. Can not remote start transaction.";
    }

    if (response.status == RequestStartStopStatusEnum::Accepted) {
        response.status = this->callbacks.remote_start_transaction_callback(
            msg, this->device_model->get_value<bool>(ControllerComponentVariables::AuthorizeRemoteStart));
    }

    const ocpp::CallResult<RequestStartTransactionResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_remote_stop_transaction_request(Call<RequestStopTransactionRequest> call) {
    const auto msg = call.msg;

    RequestStopTransactionResponse response;
    std::optional<int32_t> evseid = this->evse_manager->get_transaction_evseid(msg.transactionId);

    if (evseid.has_value()) {
        // F03.FR.07: send 'accepted' if there was an ongoing transaction with the given transaction id
        response.status = RequestStartStopStatusEnum::Accepted;
    } else {
        // F03.FR.08: send 'rejected' if there was no ongoing transaction with the given transaction id
        response.status = RequestStartStopStatusEnum::Rejected;
    }

    if (response.status == RequestStartStopStatusEnum::Accepted) {
        response.status = this->callbacks.stop_transaction_callback(evseid.value(), ReasonEnum::Remote);
    }

    const ocpp::CallResult<RequestStopTransactionResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_costupdated_req(const Call<CostUpdatedRequest> call) {
    CostUpdatedResponse response;
    ocpp::CallResult<CostUpdatedResponse> call_result(response, call.uniqueId);

    if (!is_cost_enabled() or !this->callbacks.set_running_cost_callback.has_value()) {
        this->message_dispatcher->dispatch_call_result(call_result);
        return;
    }

    RunningCost running_cost;
    TriggerMeterValue triggers;

    if (device_model
            ->get_optional_value<bool>(ControllerComponentVariables::CustomImplementationCaliforniaPricingEnabled)
            .value_or(false) and
        call.msg.customData.has_value()) {
        const json running_cost_json = call.msg.customData.value();

        // California pricing is enabled, which means we have to read the custom data.
        running_cost = running_cost_json;

        if (running_cost_json.contains("triggerMeterValue")) {
            triggers = running_cost_json.at("triggerMeterValue");
        }
    } else {
        running_cost.state = RunningCostState::Charging;
    }

    // In 2.0.1, the cost and transaction id are already part of the CostUpdatedRequest, so they need to be added to
    // the 'RunningCost' struct.
    running_cost.cost = static_cast<double>(call.msg.totalCost);
    running_cost.transaction_id = call.msg.transactionId;

    std::optional<int32_t> transaction_evse_id =
        this->evse_manager->get_transaction_evseid(running_cost.transaction_id);
    if (!transaction_evse_id.has_value()) {
        // We just put an error in the log as the spec does not define what to do here. It is not possible to return
        // a 'Rejected' or something in that manner.
        EVLOG_error << "Received CostUpdatedRequest, but transaction id is not a valid transaction id.";
    }

    const int number_of_decimals =
        this->device_model->get_optional_value<int>(ControllerComponentVariables::NumberOfDecimalsForCostValues)
            .value_or(DEFAULT_PRICE_NUMBER_OF_DECIMALS);
    uint32_t decimals =
        (number_of_decimals < 0 ? DEFAULT_PRICE_NUMBER_OF_DECIMALS : static_cast<uint32_t>(number_of_decimals));
    const std::optional<std::string> currency =
        this->device_model->get_value<std::string>(ControllerComponentVariables::TariffCostCtrlrCurrency);
    this->callbacks.set_running_cost_callback.value()(running_cost, decimals, currency);

    this->message_dispatcher->dispatch_call_result(call_result);

    // In OCPP 2.0.1, the chargepoint status trigger is not used.
    if (!triggers.at_energy_kwh.has_value() and !triggers.at_power_kw.has_value() and !triggers.at_time.has_value()) {
        return;
    }

    const std::optional<int32_t> evse_id_opt = this->evse_manager->get_transaction_evseid(running_cost.transaction_id);
    if (!evse_id_opt.has_value()) {
        EVLOG_warning << "Can not set running cost triggers as there is no evse id found with the transaction id from "
                         "the incoming CostUpdatedRequest";
        return;
    }

    const int32_t evse_id = evse_id_opt.value();
    auto& evse = this->evse_manager->get_evse(evse_id);
    evse.set_meter_value_pricing_triggers(
        triggers.at_power_kw, triggers.at_energy_kwh, triggers.at_time,
        [this, evse_id](const std::vector<MeterValue>& meter_values) {
            this->meter_values_req(evse_id, meter_values, false);
        },
        this->io_service);
}

void ChargePoint::handle_set_charging_profile_req(Call<SetChargingProfileRequest> call) {
    EVLOG_debug << "Received SetChargingProfileRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    auto msg = call.msg;
    SetChargingProfileResponse response;
    response.status = ChargingProfileStatusEnum::Rejected;

    // K01.FR.29: Respond with a CallError if SmartCharging is not available for this Charging Station
    bool is_smart_charging_available =
        this->device_model->get_optional_value<bool>(ControllerComponentVariables::SmartChargingCtrlrAvailable)
            .value_or(false);

    if (!is_smart_charging_available) {
        EVLOG_warning << "SmartChargingCtrlrAvailable is not set for Charging Station. Returning NotSupported error";

        const auto call_error =
            CallError(call.uniqueId, "NotSupported", "Charging Station does not support smart charging", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);

        return;
    }

    // K01.FR.22: Reject ChargingStationExternalConstraints profiles in SetChargingProfileRequest
    if (msg.chargingProfile.chargingProfilePurpose == ChargingProfilePurposeEnum::ChargingStationExternalConstraints) {
        response.statusInfo = StatusInfo();
        response.statusInfo->reasonCode = "InvalidValue";
        response.statusInfo->additionalInfo = "ChargingStationExternalConstraintsInSetChargingProfileRequest";
        EVLOG_debug << "Rejecting SetChargingProfileRequest:\n reasonCode: " << response.statusInfo->reasonCode.get()
                    << "\nadditionalInfo: " << response.statusInfo->additionalInfo->get();

        ocpp::CallResult<SetChargingProfileResponse> call_result(response, call.uniqueId);
        this->message_dispatcher->dispatch_call_result(call_result);

        return;
    }

    response = this->smart_charging_handler->conform_validate_and_add_profile(msg.chargingProfile, msg.evseId);
    if (response.status == ChargingProfileStatusEnum::Accepted) {
        EVLOG_debug << "Accepting SetChargingProfileRequest";
        this->callbacks.set_charging_profiles_callback();
    } else {
        EVLOG_debug << "Rejecting SetChargingProfileRequest:\n reasonCode: " << response.statusInfo->reasonCode.get()
                    << "\nadditionalInfo: " << response.statusInfo->additionalInfo->get();
    }

    ocpp::CallResult<SetChargingProfileResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_clear_charging_profile_req(Call<ClearChargingProfileRequest> call) {
    EVLOG_debug << "Received ClearChargingProfileRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    const auto msg = call.msg;
    ClearChargingProfileResponse response;
    response.status = ClearChargingProfileStatusEnum::Unknown;

    // K10.FR.06
    if (msg.chargingProfileCriteria.has_value() and
        msg.chargingProfileCriteria.value().chargingProfilePurpose.has_value() and
        msg.chargingProfileCriteria.value().chargingProfilePurpose.value() ==
            ChargingProfilePurposeEnum::ChargingStationExternalConstraints) {
        response.statusInfo = StatusInfo();
        response.statusInfo->reasonCode = "InvalidValue";
        response.statusInfo->additionalInfo = "ChargingStationExternalConstraintsInClearChargingProfileRequest";
        EVLOG_debug << "Rejecting SetChargingProfileRequest:\n reasonCode: " << response.statusInfo->reasonCode.get()
                    << "\nadditionalInfo: " << response.statusInfo->additionalInfo->get();
    } else {
        response = this->smart_charging_handler->clear_profiles(msg);
    }

    if (response.status == ClearChargingProfileStatusEnum::Accepted) {
        this->callbacks.set_charging_profiles_callback();
    }

    ocpp::CallResult<ClearChargingProfileResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_get_charging_profiles_req(Call<GetChargingProfilesRequest> call) {
    EVLOG_debug << "Received GetChargingProfilesRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    const auto msg = call.msg;
    GetChargingProfilesResponse response;

    const auto profiles_to_report = this->smart_charging_handler->get_reported_profiles(msg);

    response.status =
        profiles_to_report.empty() ? GetChargingProfileStatusEnum::NoProfiles : GetChargingProfileStatusEnum::Accepted;

    ocpp::CallResult<GetChargingProfilesResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    if (response.status == GetChargingProfileStatusEnum::NoProfiles) {
        return;
    }

    // There are profiles to report.
    // Prepare ReportChargingProfileRequest(s). The message defines the properties evseId and
    // chargingLimitSource as required, so we can not report all profiles in a single
    // ReportChargingProfilesRequest. We need to prepare a single ReportChargingProfilesRequest for each
    // combination of evseId and chargingLimitSource
    std::set<int32_t> evse_ids;                // will contain all evse_ids of the profiles
    std::set<ChargingLimitSourceEnum> sources; // will contain all sources of the profiles

    // fill evse_ids and sources sets
    for (const auto& profile : profiles_to_report) {
        evse_ids.insert(profile.evse_id);
        sources.insert(profile.source);
    }

    std::vector<ReportChargingProfilesRequest> requests_to_send;

    for (const auto evse_id : evse_ids) {
        for (const auto source : sources) {
            std::vector<ChargingProfile> original_profiles;
            for (const auto& reported_profile : profiles_to_report) {
                if (reported_profile.evse_id == evse_id and reported_profile.source == source) {
                    original_profiles.push_back(reported_profile.profile);
                }
            }
            if (not original_profiles.empty()) {
                // prepare a ReportChargingProfilesRequest
                ReportChargingProfilesRequest req;
                req.requestId = msg.requestId; // K09.FR.01
                req.evseId = evse_id;
                req.chargingLimitSource = source;
                req.chargingProfile = original_profiles;
                req.tbc = true;
                requests_to_send.push_back(req);
            }
        }
    }

    requests_to_send.back().tbc = false;

    // requests_to_send are ready, send them and define tbc property
    for (const auto& request_to_send : requests_to_send) {
        this->report_charging_profile_req(request_to_send);
    }
}

void ChargePoint::handle_get_composite_schedule_req(Call<GetCompositeScheduleRequest> call) {
    EVLOG_debug << "Received GetCompositeScheduleRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    const auto response = this->get_composite_schedule_internal(call.msg);

    ocpp::CallResult<GetCompositeScheduleResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_firmware_update_req(Call<UpdateFirmwareRequest> call) {
    EVLOG_debug << "Received UpdateFirmwareRequest: " << call.msg << "\nwith messageId: " << call.uniqueId;
    if (call.msg.firmware.signingCertificate.has_value() or call.msg.firmware.signature.has_value()) {
        this->firmware_status_before_installing = FirmwareStatusEnum::SignatureVerified;
    } else {
        this->firmware_status_before_installing = FirmwareStatusEnum::Downloaded;
    }

    UpdateFirmwareResponse response;
    const auto msg = call.msg;
    bool cert_valid_or_not_set = true;

    // L01.FR.22 check if certificate is valid
    if (msg.firmware.signingCertificate.has_value() and
        this->evse_security->verify_certificate(msg.firmware.signingCertificate.value().get(),
                                                ocpp::LeafCertificateType::MF) !=
            ocpp::CertificateValidationResult::Valid) {
        response.status = UpdateFirmwareStatusEnum::InvalidCertificate;
        cert_valid_or_not_set = false;
    }

    if (cert_valid_or_not_set) {
        // execute firwmare update callback
        response = callbacks.update_firmware_request_callback(msg);
    }

    ocpp::CallResult<UpdateFirmwareResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    if ((response.status == UpdateFirmwareStatusEnum::InvalidCertificate) or
        (response.status == UpdateFirmwareStatusEnum::RevokedCertificate)) {
        // L01.FR.02
        this->security->security_event_notification_req(
            CiString<50>(ocpp::security_events::INVALIDFIRMWARESIGNINGCERTIFICATE),
            std::optional<CiString<255>>("Provided signing certificate is not valid!"), true,
            true); // critical because TC_L_05_CS requires this message to be sent
    }
}

void ChargePoint::handle_get_log_req(Call<GetLogRequest> call) {
    const GetLogResponse response = this->callbacks.get_log_request_callback(call.msg);

    ocpp::CallResult<GetLogResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_customer_information_req(Call<CustomerInformationRequest> call) {
    CustomerInformationResponse response;
    const auto& msg = call.msg;
    response.status = CustomerInformationStatusEnum::Accepted;

    if (!msg.report and !msg.clear) {
        EVLOG_warning << "CSMS sent CustomerInformation.req with both report and clear flags being false";
        response.status = CustomerInformationStatusEnum::Rejected;
    }

    if (!msg.customerCertificate.has_value() and !msg.idToken.has_value() and !msg.customerIdentifier.has_value()) {
        EVLOG_warning << "CSMS sent CustomerInformation.req without setting one of customerCertificate, idToken, "
                         "customerIdentifier fields";
        response.status = CustomerInformationStatusEnum::Invalid;
    }

    ocpp::CallResult<CustomerInformationResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    if (response.status == CustomerInformationStatusEnum::Accepted) {
        std::string data = "";
        if (msg.report) {
            data += this->get_customer_information(msg.customerCertificate, msg.idToken, msg.customerIdentifier);
        }
        if (msg.clear) {
            this->clear_customer_information(msg.customerCertificate, msg.idToken, msg.customerIdentifier);
        }

        const auto max_customer_information_data_length =
            this->device_model->get_optional_value<int>(ControllerComponentVariables::MaxCustomerInformationDataLength)
                .value_or(DEFAULT_MAX_CUSTOMER_INFORMATION_DATA_LENGTH);
        if (data.length() > max_customer_information_data_length) {
            EVLOG_warning << "NotifyCustomerInformation.req data field is too large. Cropping it down to: "
                          << max_customer_information_data_length << "characters";
            data.erase(max_customer_information_data_length);
        }

        this->notify_customer_information_req(data, msg.requestId);
    }
}

void ChargePoint::handle_set_monitoring_base_req(Call<SetMonitoringBaseRequest> call) {
    SetMonitoringBaseResponse response;
    const auto& msg = call.msg;

    auto result = this->device_model->set_value(
        ControllerComponentVariables::ActiveMonitoringBase.component,
        ControllerComponentVariables::ActiveMonitoringBase.variable.value(), AttributeEnum::Actual,
        conversions::monitoring_base_enum_to_string(msg.monitoringBase), VARIABLE_ATTRIBUTE_VALUE_SOURCE_CSMS, true);

    if (result != SetVariableStatusEnum::Accepted) {
        EVLOG_warning << "Could not persist in device model new monitoring base: "
                      << conversions::monitoring_base_enum_to_string(msg.monitoringBase);
        response.status = GenericDeviceModelStatusEnum::Rejected;
    } else {
        response.status = GenericDeviceModelStatusEnum::Accepted;

        if (msg.monitoringBase == MonitoringBaseEnum::HardWiredOnly or
            msg.monitoringBase == MonitoringBaseEnum::FactoryDefault) {
            try {
                this->device_model->clear_custom_monitors();
            } catch (const DeviceModelError& e) {
                EVLOG_warning << "Could not clear custom monitors from DB: " << e.what();
                response.status = GenericDeviceModelStatusEnum::Rejected;
            }
        }
    }

    ocpp::CallResult<SetMonitoringBaseResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_set_monitoring_level_req(Call<SetMonitoringLevelRequest> call) {
    SetMonitoringLevelResponse response;
    const auto& msg = call.msg;

    if (msg.severity < MonitoringLevelSeverity::MIN or msg.severity > MonitoringLevelSeverity::MAX) {
        response.status = GenericStatusEnum::Rejected;
    } else {
        auto result = this->device_model->set_value(
            ControllerComponentVariables::ActiveMonitoringLevel.component,
            ControllerComponentVariables::ActiveMonitoringLevel.variable.value(), AttributeEnum::Actual,
            std::to_string(msg.severity), VARIABLE_ATTRIBUTE_VALUE_SOURCE_CSMS, true);

        if (result != SetVariableStatusEnum::Accepted) {
            EVLOG_warning << "Could not persist in device model new monitoring level: " << msg.severity;
            response.status = GenericStatusEnum::Rejected;
        } else {
            response.status = GenericStatusEnum::Accepted;
        }
    }

    ocpp::CallResult<SetMonitoringLevelResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::handle_set_variable_monitoring_req(const EnhancedMessage<v201::MessageType>& message) {
    Call<SetVariableMonitoringRequest> call = message.call_message;
    SetVariableMonitoringResponse response;
    const auto& msg = call.msg;

    const auto max_items_per_message =
        this->device_model->get_value<int>(ControllerComponentVariables::ItemsPerMessageSetVariableMonitoring);
    const auto max_bytes_message =
        this->device_model->get_value<int>(ControllerComponentVariables::BytesPerMessageSetVariableMonitoring);

    // N04.FR.09
    if (msg.setMonitoringData.size() > max_items_per_message) {
        const auto call_error = CallError(call.uniqueId, "OccurenceConstraintViolation", "", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
        return;
    }

    if (message.message_size > max_bytes_message) {
        const auto call_error = CallError(call.uniqueId, "FormatViolation", "", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
        return;
    }

    try {
        response.setMonitoringResult = this->device_model->set_monitors(msg.setMonitoringData);
    } catch (const DeviceModelError& e) {
        EVLOG_error << "Set monitors failed:" << e.what();
    }

    ocpp::CallResult<SetVariableMonitoringResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

void ChargePoint::notify_monitoring_report_req(const int request_id,
                                               const std::vector<MonitoringData>& montoring_data) {
    static constexpr int32_t MAXIMUM_VARIABLE_SEND = 10;

    if (montoring_data.size() <= MAXIMUM_VARIABLE_SEND) {
        NotifyMonitoringReportRequest req;
        req.requestId = request_id;
        req.seqNo = 0;
        req.generatedAt = ocpp::DateTime();
        req.monitor.emplace(montoring_data);
        req.tbc = false;

        ocpp::Call<NotifyMonitoringReportRequest> call(req);
        this->message_dispatcher->dispatch_call(call);
    } else {
        // Split for larger message sizes
        int32_t sequence_num = 0;
        auto generated_at = ocpp::DateTime();

        for (int32_t i = 0; i < montoring_data.size(); i += MAXIMUM_VARIABLE_SEND) {
            // If our next index is >= than the last index then we're finished
            bool last_part = ((i + MAXIMUM_VARIABLE_SEND) >= montoring_data.size());

            NotifyMonitoringReportRequest req;
            req.requestId = request_id;
            req.seqNo = sequence_num;
            req.generatedAt = generated_at;
            req.tbc = (!last_part);

            // Construct sub-message part
            std::vector<MonitoringData> sub_data;

            for (int32_t j = i; j < MAXIMUM_VARIABLE_SEND and j < montoring_data.size(); ++j) {
                sub_data.push_back(std::move(montoring_data[i + j]));
            }

            req.monitor = sub_data;

            ocpp::Call<NotifyMonitoringReportRequest> call(req);
            this->message_dispatcher->dispatch_call(call);

            sequence_num++;
        }
    }
}

void ChargePoint::handle_get_monitoring_report_req(Call<GetMonitoringReportRequest> call) {
    GetMonitoringReportResponse response;
    const auto& msg = call.msg;

    const auto component_variables = msg.componentVariable.value_or(std::vector<ComponentVariable>());
    const auto max_variable_components_per_message =
        this->device_model->get_value<int>(ControllerComponentVariables::ItemsPerMessageGetReport);

    // N02.FR.07
    if (component_variables.size() > max_variable_components_per_message) {
        const auto call_error = CallError(call.uniqueId, "OccurenceConstraintViolation", "", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
        return;
    }

    auto criteria = msg.monitoringCriteria.value_or(std::vector<MonitoringCriterionEnum>());
    std::vector<MonitoringData> data{};

    try {
        data = this->device_model->get_monitors(criteria, component_variables);

        if (!data.empty()) {
            response.status = GenericDeviceModelStatusEnum::Accepted;
        } else {
            response.status = GenericDeviceModelStatusEnum::EmptyResultSet;
        }
    } catch (const DeviceModelError& e) {
        EVLOG_error << "Get variable monitoring failed:" << e.what();
        response.status = GenericDeviceModelStatusEnum::Rejected;
    }

    ocpp::CallResult<GetMonitoringReportResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);

    if (response.status == GenericDeviceModelStatusEnum::Accepted) {
        // Send the result with splits if required
        notify_monitoring_report_req(msg.requestId, data);
    }
}

void ChargePoint::handle_clear_variable_monitoring_req(Call<ClearVariableMonitoringRequest> call) {
    ClearVariableMonitoringResponse response;
    const auto& msg = call.msg;

    try {
        response.clearMonitoringResult = this->device_model->clear_monitors(msg.id);
    } catch (const DeviceModelError& e) {
        EVLOG_error << "Clear variable monitoring failed:" << e.what();
    }

    ocpp::CallResult<ClearVariableMonitoringResponse> call_result(response, call.uniqueId);
    this->message_dispatcher->dispatch_call_result(call_result);
}

std::optional<DataTransferResponse> ChargePoint::data_transfer_req(const CiString<255>& vendorId,
                                                                   const std::optional<CiString<50>>& messageId,
                                                                   const std::optional<json>& data) {
    return this->data_transfer->data_transfer_req(vendorId, messageId, data);
}

std::optional<DataTransferResponse> ChargePoint::data_transfer_req(const DataTransferRequest& request) {
    return this->data_transfer->data_transfer_req(request);
}

void ChargePoint::websocket_connected_callback(const int configuration_slot,
                                               const NetworkConnectionProfile& network_connection_profile) {
    this->message_queue->resume(this->message_queue_resume_delay);

    if (this->registration_status == RegistrationStatusEnum::Accepted) {
        this->connectivity_manager->confirm_successful_connection();

        if (this->time_disconnected.time_since_epoch() != 0s) {
            // handle offline threshold
            //  Get the current time point using steady_clock
            auto offline_duration = std::chrono::steady_clock::now() - this->time_disconnected;

            // B04.FR.01
            // If offline period exceeds offline threshold then send the status notification for all connectors
            if (offline_duration > std::chrono::seconds(this->device_model->get_value<int>(
                                       ControllerComponentVariables::OfflineThreshold))) {
                EVLOG_debug << "offline for more than offline threshold ";
                this->component_state_manager->send_status_notification_all_connectors();
            } else {
                // B04.FR.02
                // If offline period doesn't exceed offline threshold then send the status notification for all
                // connectors that changed state
                EVLOG_debug << "offline for less than offline threshold ";
                this->component_state_manager->send_status_notification_changed_connectors();
            }
            this->security->init_certificate_expiration_check_timers(); // re-init as timers are stopped on disconnect
        }
    }
    this->time_disconnected = std::chrono::time_point<std::chrono::steady_clock>();

    // We have a connection again so next time it fails we should send the notification again
    this->skip_invalid_csms_certificate_notifications = false;

    if (this->callbacks.connection_state_changed_callback.has_value()) {
        this->callbacks.connection_state_changed_callback.value()(true, configuration_slot, network_connection_profile);
    }
}

void ChargePoint::websocket_disconnected_callback(const int configuration_slot,
                                                  const NetworkConnectionProfile& network_connection_profile) {
    this->message_queue->pause();

    // check if offline threshold has been defined
    if (this->device_model->get_value<int>(ControllerComponentVariables::OfflineThreshold) != 0) {
        // Get the current time point using steady_clock
        this->time_disconnected = std::chrono::steady_clock::now();
    }

    this->security->stop_certificate_expiration_check_timers();
    if (this->callbacks.connection_state_changed_callback.has_value()) {
        this->callbacks.connection_state_changed_callback.value()(false, configuration_slot,
                                                                  network_connection_profile);
    }
}

void ChargePoint::websocket_connection_failed(ConnectionFailedReason reason) {
    switch (reason) {
    case ConnectionFailedReason::InvalidCSMSCertificate:
        if (!this->skip_invalid_csms_certificate_notifications) {
            this->security->security_event_notification_req(CiString<50>(ocpp::security_events::INVALIDCSMSCERTIFICATE),
                                                            std::nullopt, true, true);
            this->skip_invalid_csms_certificate_notifications = true;
        } else {
            EVLOG_debug << "Skipping InvalidCsmsCertificate SecurityEvent since it has been sent already";
        }
        break;
    case ConnectionFailedReason::FailedToAuthenticateAtCsms:
        const auto& security_event = ocpp::security_events::FAILEDTOAUTHENTICATEATCSMS;
        this->security->security_event_notification_req(CiString<50>(security_event), std::nullopt, true,
                                                        utils::is_critical(security_event));
        break;
    }
}

GetCompositeScheduleResponse ChargePoint::get_composite_schedule_internal(const GetCompositeScheduleRequest& request,
                                                                          bool simulate_transaction_active) {
    GetCompositeScheduleResponse response;
    response.status = GenericStatusEnum::Rejected;

    std::vector<std::string> supported_charging_rate_units = ocpp::split_string(
        this->device_model->get_value<std::string>(ControllerComponentVariables::ChargingScheduleChargingRateUnit), ',',
        true);

    std::optional<ChargingRateUnitEnum> charging_rate_unit = std::nullopt;
    if (request.chargingRateUnit.has_value()) {
        bool unit_supported = std::any_of(
            supported_charging_rate_units.begin(), supported_charging_rate_units.end(), [&request](std::string item) {
                return conversions::string_to_charging_rate_unit_enum(item) == request.chargingRateUnit.value();
            });

        if (unit_supported) {
            charging_rate_unit = request.chargingRateUnit.value();
        }
    } else if (supported_charging_rate_units.size() > 0) {
        charging_rate_unit = conversions::string_to_charging_rate_unit_enum(supported_charging_rate_units.at(0));
    }

    // K01.FR.05 & K01.FR.07
    if (this->evse_manager->does_evse_exist(request.evseId) and charging_rate_unit.has_value()) {
        auto start_time = ocpp::DateTime();
        auto end_time = ocpp::DateTime(start_time.to_time_point() + std::chrono::seconds(request.duration));

        auto schedule = this->smart_charging_handler->calculate_composite_schedule(
            start_time, end_time, request.evseId, charging_rate_unit.value(), this->is_offline(),
            simulate_transaction_active);

        response.schedule = schedule;
        response.status = GenericStatusEnum::Accepted;
    } else {
        auto reason = charging_rate_unit.has_value()
                          ? ProfileValidationResultEnum::EvseDoesNotExist
                          : ProfileValidationResultEnum::ChargingScheduleChargingRateUnitUnsupported;
        response.statusInfo = StatusInfo();
        response.statusInfo->reasonCode = conversions::profile_validation_result_to_reason_code(reason);
        response.statusInfo->additionalInfo = conversions::profile_validation_result_to_string(reason);
        EVLOG_debug << "Rejecting SetChargingProfileRequest:\n reasonCode: " << response.statusInfo->reasonCode.get()
                    << "\nadditionalInfo: " << response.statusInfo->additionalInfo->get();
    }
    return response;
}

void ChargePoint::update_dm_availability_state(const int32_t evse_id, const int32_t connector_id,
                                               const ConnectorStatusEnum status) {
    ComponentVariable charging_station = ControllerComponentVariables::ChargingStationAvailabilityState;
    ComponentVariable evse_cv =
        EvseComponentVariables::get_component_variable(evse_id, EvseComponentVariables::AvailabilityState);
    ComponentVariable connector_cv = ConnectorComponentVariables::get_component_variable(
        evse_id, connector_id, ConnectorComponentVariables::AvailabilityState);
    if (evse_cv.variable.has_value()) {
        this->device_model->set_read_only_value(
            evse_cv.component, evse_cv.variable.value(), ocpp::v201::AttributeEnum::Actual,
            conversions::connector_status_enum_to_string(status), VARIABLE_ATTRIBUTE_VALUE_SOURCE_INTERNAL);
    }
    if (connector_cv.variable.has_value()) {
        this->device_model->set_read_only_value(
            connector_cv.component, connector_cv.variable.value(), ocpp::v201::AttributeEnum::Actual,
            conversions::connector_status_enum_to_string(status), VARIABLE_ATTRIBUTE_VALUE_SOURCE_INTERNAL);
    }

    // if applicable to the entire charging station
    if (evse_id == 0 and charging_station.variable.has_value()) {
        this->device_model->set_read_only_value(
            charging_station.component, charging_station.variable.value(), ocpp::v201::AttributeEnum::Actual,
            conversions::connector_status_enum_to_string(status), VARIABLE_ATTRIBUTE_VALUE_SOURCE_INTERNAL);
    }
}

void ChargePoint::update_dm_evse_power(const int32_t evse_id, const MeterValue& meter_value) {
    ComponentVariable evse_power_cv =
        EvseComponentVariables::get_component_variable(evse_id, EvseComponentVariables::Power);

    if (!evse_power_cv.variable.has_value()) {
        return;
    }

    const auto power = utils::get_total_power_active_import(meter_value);
    if (!power.has_value()) {
        return;
    }

    this->device_model->set_read_only_value(evse_power_cv.component, evse_power_cv.variable.value(),
                                            AttributeEnum::Actual, std::to_string(power.value()),
                                            VARIABLE_ATTRIBUTE_VALUE_SOURCE_INTERNAL);
}

void ChargePoint::clear_invalid_charging_profiles() {
    try {
        auto evses = this->database_handler->get_all_charging_profiles_group_by_evse();
        EVLOG_info << "Found " << evses.size() << " evse in the database";
        for (const auto& [evse_id, profiles] : evses) {
            for (auto profile : profiles) {
                try {
                    if (this->smart_charging_handler->conform_and_validate_profile(profile, evse_id) !=
                        ProfileValidationResultEnum::Valid) {
                        this->database_handler->delete_charging_profile(profile.id);
                    }
                } catch (const QueryExecutionException& e) {
                    EVLOG_warning << "Failed database operation for ChargingProfiles: " << e.what();
                }
            }
        }
    } catch (const std::exception& e) {
        EVLOG_warning << "Unknown error while loading charging profiles from database: " << e.what();
    }
}

std::vector<GetVariableResult>
ChargePoint::get_variables(const std::vector<GetVariableData>& get_variable_data_vector) {
    std::vector<GetVariableResult> response;
    for (const auto& get_variable_data : get_variable_data_vector) {
        GetVariableResult get_variable_result;
        get_variable_result.component = get_variable_data.component;
        get_variable_result.variable = get_variable_data.variable;
        get_variable_result.attributeType = get_variable_data.attributeType.value_or(AttributeEnum::Actual);
        const auto request_value_response = this->device_model->request_value<std::string>(
            get_variable_data.component, get_variable_data.variable,
            get_variable_data.attributeType.value_or(AttributeEnum::Actual));
        if (request_value_response.status == GetVariableStatusEnum::Accepted and
            request_value_response.value.has_value()) {
            get_variable_result.attributeValue = request_value_response.value.value();
        }
        get_variable_result.attributeStatus = request_value_response.status;
        response.push_back(get_variable_result);
    }
    return response;
}

std::map<SetVariableData, SetVariableResult>
ChargePoint::set_variables(const std::vector<SetVariableData>& set_variable_data_vector, const std::string& source) {
    // set variables and allow setting of ReadOnly variables
    const auto response = this->set_variables_internal(set_variable_data_vector, source, true);
    this->handle_variables_changed(response);
    return response;
}

GetCompositeScheduleResponse ChargePoint::get_composite_schedule(const GetCompositeScheduleRequest& request) {
    return this->get_composite_schedule_internal(request);
}

std::optional<CompositeSchedule> ChargePoint::get_composite_schedule(int32_t evse_id, std::chrono::seconds duration,
                                                                     ChargingRateUnitEnum unit) {
    GetCompositeScheduleRequest request;
    request.duration = duration.count();
    request.evseId = evse_id;
    request.chargingRateUnit = unit;

    auto composite_schedule_response = this->get_composite_schedule_internal(request, false);
    if (composite_schedule_response.status == GenericStatusEnum::Accepted and
        composite_schedule_response.schedule.has_value()) {
        return composite_schedule_response.schedule.value();
    } else {
        return std::nullopt;
    }
}

std::vector<CompositeSchedule> ChargePoint::get_all_composite_schedules(const int32_t duration_s,
                                                                        const ChargingRateUnitEnum& unit) {
    std::vector<CompositeSchedule> composite_schedules;

    const auto number_of_evses = this->evse_manager->get_number_of_evses();
    // get all composite schedules including the one for evse_id == 0
    for (int32_t evse_id = 0; evse_id <= number_of_evses; evse_id++) {
        GetCompositeScheduleRequest request;
        request.duration = duration_s;
        request.evseId = evse_id;
        request.chargingRateUnit = unit;
        auto composite_schedule_response = this->get_composite_schedule_internal(request);
        if (composite_schedule_response.status == GenericStatusEnum::Accepted and
            composite_schedule_response.schedule.has_value()) {
            composite_schedules.push_back(composite_schedule_response.schedule.value());
        } else {
            EVLOG_warning << "Could not internally retrieve composite schedule for evse id " << evse_id << ": "
                          << composite_schedule_response;
        }
    }

    return composite_schedules;
}

std::optional<NetworkConnectionProfile>
ChargePoint::get_network_connection_profile(const int32_t configuration_slot) const {
    return this->connectivity_manager->get_network_connection_profile(configuration_slot);
}

std::optional<int> ChargePoint::get_priority_from_configuration_slot(const int configuration_slot) const {
    return this->connectivity_manager->get_priority_from_configuration_slot(configuration_slot);
}

const std::vector<int>& ChargePoint::get_network_connection_slots() const {
    return this->connectivity_manager->get_network_connection_slots();
}

void ChargePoint::send_not_implemented_error(const MessageId unique_message_id, const MessageTypeId message_type_id) {
    if (message_type_id == MessageTypeId::CALL) {
        const auto call_error = CallError(unique_message_id, "NotImplemented", "", json({}));
        this->message_dispatcher->dispatch_call_error(call_error);
    }
}

} // namespace v201
} // namespace ocpp
