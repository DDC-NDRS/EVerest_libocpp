// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2023 Pionix GmbH and Contributors to EVerest

#include "date/tz.h"
#include "ocpp/common/types.hpp"
#include "ocpp/v201/ctrlr_component_variables.hpp"
#include "ocpp/v201/device_model.hpp"
#include "ocpp/v201/device_model_storage_sqlite.hpp"
#include "ocpp/v201/ocpp_types.hpp"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <sqlite3.h>

#include <component_state_manager_mock.hpp>
#include <device_model_storage_mock.hpp>
#include <evse_mock.hpp>
#include <evse_security_mock.hpp>
#include <ocpp/common/call_types.hpp>
#include <ocpp/v201/enums.hpp>
#include <ocpp/v201/evse.hpp>
#include <ocpp/v201/smart_charging.hpp>
#include <optional>

#include <sstream>
#include <vector>

namespace ocpp::v201 {

static const int STATION_WIDE_ID = 0;
static const int DEFAULT_EVSE_ID = 1;
static const int DEFAULT_PROFILE_ID = 1;
static const int DEFAULT_STACK_LEVEL = 1;
static const std::string DEFAULT_TX_ID = "10c75ff7-74f5-44f5-9d01-f649f3ac7b78";

class TestSmartChargingHandler : public SmartChargingHandler {
public:
    using SmartChargingHandler::validate_charging_station_max_profile;
    using SmartChargingHandler::validate_evse_exists;
    using SmartChargingHandler::validate_profile_schedules;
    using SmartChargingHandler::validate_tx_default_profile;
    using SmartChargingHandler::validate_tx_profile;

    TestSmartChargingHandler(std::map<int32_t, std::unique_ptr<EvseInterface>>& evses,
                             std::shared_ptr<DeviceModel>& device_model) :
        SmartChargingHandler(evses, device_model) {
    }
};

class ChargepointTestFixtureV201 : public testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
        sqlite3_close(this->db_handle);
    }

    ChargingSchedule create_charge_schedule(ChargingRateUnitEnum charging_rate_unit) {
        int32_t id;
        std::vector<ChargingSchedulePeriod> charging_schedule_period;
        std::optional<CustomData> custom_data;
        std::optional<ocpp::DateTime> start_schedule;
        std::optional<int32_t> duration;
        std::optional<float> min_charging_rate;
        std::optional<SalesTariff> sales_tariff;

        return ChargingSchedule{
            id,
            charging_rate_unit,
            charging_schedule_period,
            custom_data,
            start_schedule,
            duration,
            min_charging_rate,
            sales_tariff,
        };
    }

    ChargingSchedule create_charge_schedule(ChargingRateUnitEnum charging_rate_unit,
                                            std::vector<ChargingSchedulePeriod> charging_schedule_period,
                                            std::optional<ocpp::DateTime> start_schedule = std::nullopt) {
        int32_t id;
        std::optional<CustomData> custom_data;
        std::optional<int32_t> duration;
        std::optional<float> min_charging_rate;
        std::optional<SalesTariff> sales_tariff;

        return ChargingSchedule{
            id,
            charging_rate_unit,
            charging_schedule_period,
            custom_data,
            start_schedule,
            duration,
            min_charging_rate,
            sales_tariff,
        };
    }

    std::vector<ChargingSchedulePeriod>
    create_charging_schedule_periods(int32_t start_period, std::optional<int32_t> number_phases = std::nullopt,
                                     std::optional<int32_t> phase_to_use = std::nullopt) {
        auto charging_schedule_period = ChargingSchedulePeriod{
            .startPeriod = start_period,
            .numberPhases = number_phases,
            .phaseToUse = phase_to_use,
        };

        return {charging_schedule_period};
    }

    std::vector<ChargingSchedulePeriod> create_charging_schedule_periods(std::vector<int32_t> start_periods) {
        auto charging_schedule_periods = std::vector<ChargingSchedulePeriod>();
        for (auto start_period : start_periods) {
            auto charging_schedule_period = ChargingSchedulePeriod{
                .startPeriod = start_period,
            };
            charging_schedule_periods.push_back(charging_schedule_period);
        }

        return charging_schedule_periods;
    }

    std::vector<ChargingSchedulePeriod>
    create_charging_schedule_periods_with_phases(int32_t start_period, int32_t numberPhases, int32_t phaseToUse) {
        auto charging_schedule_period =
            ChargingSchedulePeriod{.startPeriod = start_period, .numberPhases = numberPhases, .phaseToUse = phaseToUse};

        return {charging_schedule_period};
    }

    ChargingProfile
    create_charging_profile(int32_t charging_profile_id, ChargingProfilePurposeEnum charging_profile_purpose,
                            ChargingSchedule charging_schedule, std::optional<std::string> transaction_id = {},
                            ChargingProfileKindEnum charging_profile_kind = ChargingProfileKindEnum::Absolute,
                            int stack_level = DEFAULT_STACK_LEVEL, std::optional<ocpp::DateTime> validFrom = {},
                            std::optional<ocpp::DateTime> validTo = {}) {
        auto recurrency_kind = RecurrencyKindEnum::Daily;
        std::vector<ChargingSchedule> charging_schedules = {charging_schedule};
        return ChargingProfile{.id = charging_profile_id,
                               .stackLevel = stack_level,
                               .chargingProfilePurpose = charging_profile_purpose,
                               .chargingProfileKind = charging_profile_kind,
                               .chargingSchedule = charging_schedules,
                               .customData = {},
                               .recurrencyKind = recurrency_kind,
                               .validFrom = validFrom,
                               .validTo = validTo,
                               .transactionId = transaction_id};
    }

    void create_device_model_db(const std::string& path) {
        sqlite3* source_handle;
        sqlite3_open(DEVICE_MODEL_DB_LOCATION_V201, &source_handle);
        sqlite3_open(path.c_str(), &db_handle);

        auto* backup = sqlite3_backup_init(db_handle, "main", source_handle, "main");
        sqlite3_backup_step(backup, -1);
        sqlite3_backup_finish(backup);

        sqlite3_close(source_handle);
    }

    std::shared_ptr<DeviceModel>
    create_device_model(const std::string& path = "file:device_model?mode=memory&cache=shared",
                        const std::optional<std::string> ac_phase_switching_supported = "true") {
        create_device_model_db(path);
        auto device_model_storage = std::make_unique<DeviceModelStorageSqlite>(path);
        auto device_model = std::make_shared<DeviceModel>(std::move(device_model_storage));

        // Defaults
        const auto& charging_rate_unit_cv = ControllerComponentVariables::ChargingScheduleChargingRateUnit;
        device_model->set_value(charging_rate_unit_cv.component, charging_rate_unit_cv.variable.value(),
                                AttributeEnum::Actual, "A,W", true);

        const auto& ac_phase_switching_cv = ControllerComponentVariables::ACPhaseSwitchingSupported;
        device_model->set_value(ac_phase_switching_cv.component, ac_phase_switching_cv.variable.value(),
                                AttributeEnum::Actual, ac_phase_switching_supported.value_or(""), true);

        return device_model;
    }

    void create_evse_with_id(int id) {
        testing::MockFunction<void(const MeterValue& meter_value, const Transaction& transaction, const int32_t seq_no,
                                   const std::optional<int32_t> reservation_id)>
            transaction_meter_value_req_mock;
        testing::MockFunction<void()> pause_charging_callback_mock;
        auto e1 = std::make_unique<Evse>(
            id, 1, *device_model, database_handler, std::make_shared<ComponentStateManagerMock>(),
            transaction_meter_value_req_mock.AsStdFunction(), pause_charging_callback_mock.AsStdFunction());
        evses[id] = std::move(e1);
    }

    TestSmartChargingHandler create_smart_charging_handler() {
        return TestSmartChargingHandler(evses, device_model);
    }

    std::string uuid() {
        std::stringstream s;
        s << uuid_generator();
        return s.str();
    }

    void open_evse_transaction(int evse_id, std::string transaction_id) {
        auto connector_id = 1;
        auto meter_start = MeterValue();
        auto id_token = IdToken();
        auto date_time = ocpp::DateTime("2024-01-17T17:00:00");
        evses[evse_id]->open_transaction(
            transaction_id, connector_id, date_time, meter_start, id_token, {}, {},
            std::chrono::seconds(static_cast<int64_t>(1)), std::chrono::seconds(static_cast<int64_t>(1)),
            std::chrono::seconds(static_cast<int64_t>(1)), std::chrono::seconds(static_cast<int64_t>(1)));
    }

    void install_profile_on_evse(int evse_id, int profile_id,
                                 std::optional<ocpp::DateTime> validFrom = ocpp::DateTime("2024-01-01T17:00:00"),
                                 std::optional<ocpp::DateTime> validTo = ocpp::DateTime("2024-02-01T17:00:00")) {
        if (evse_id != STATION_WIDE_ID) {
            create_evse_with_id(evse_id);
        }
        auto existing_profile = create_charging_profile(
            profile_id, ChargingProfilePurposeEnum::TxDefaultProfile, create_charge_schedule(ChargingRateUnitEnum::A),
            {}, ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL, validFrom, validTo);
        handler.add_profile(evse_id, existing_profile);
    }

    // Default values used within the tests
    std::map<int32_t, std::unique_ptr<EvseInterface>> evses;
    std::shared_ptr<DatabaseHandler> database_handler;

    sqlite3* db_handle;

    bool ignore_no_transaction = true;
    std::shared_ptr<DeviceModel> device_model = create_device_model();
    TestSmartChargingHandler handler = create_smart_charging_handler();
    boost::uuids::random_generator uuid_generator = boost::uuids::random_generator();
};

TEST_F(ChargepointTestFixtureV201, K01FR03_IfTxProfileIsMissingTransactionId_ThenProfileIsInvalid) {
    create_evse_with_id(DEFAULT_EVSE_ID);
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), {});
    auto sut = handler.validate_tx_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::TxProfileMissingTransactionId));
}

TEST_F(ChargepointTestFixtureV201, K01FR16_IfTxProfileHasEvseIdNotGreaterThanZero_ThenProfileIsInvalid) {
    auto wrong_evse_id = STATION_WIDE_ID;
    create_evse_with_id(wrong_evse_id);
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID);
    auto sut = handler.validate_tx_profile(profile, wrong_evse_id);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::TxProfileEvseIdNotGreaterThanZero));
}

TEST_F(ChargepointTestFixtureV201, K01FR33_IfTxProfileTransactionIsNotOnEvse_ThenProfileIsInvalid) {
    create_evse_with_id(DEFAULT_EVSE_ID);
    open_evse_transaction(DEFAULT_EVSE_ID, "wrong transaction id");
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID);
    auto sut = handler.validate_tx_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::TxProfileTransactionNotOnEvse));
}

TEST_F(ChargepointTestFixtureV201, K01FR09_IfTxProfileEvseHasNoActiveTransaction_ThenProfileIsInvalid) {
    auto connector_id = 1;
    auto meter_start = MeterValue();
    auto id_token = IdToken();
    auto date_time = ocpp::DateTime("2024-01-17T17:00:00");
    create_evse_with_id(DEFAULT_EVSE_ID);
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID);
    auto sut = handler.validate_tx_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::TxProfileEvseHasNoActiveTransaction));
}

TEST_F(ChargepointTestFixtureV201, K01FR19_NumberPhasesOtherThan1AndPhaseToUseSet_ThenProfileInvalid) {
    auto periods = create_charging_schedule_periods_with_phases(0, 0, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID,
        ChargingProfileKindEnum::Absolute, 1);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodInvalidPhaseToUse));
}

TEST_F(ChargepointTestFixtureV201, K01FR20_IfPhaseToUseSetAndACPhaseSwitchingSupportedUndefined_ThenProfileIsInvalid) {
    auto device_model_without_ac_phase_switching =
        create_device_model("file:device_model2?mode=memory&cache=shared", {});
    device_model = std::move(device_model_without_ac_phase_switching);

    auto periods = create_charging_schedule_periods_with_phases(0, 1, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID,
        ChargingProfileKindEnum::Absolute, 1);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut,
                testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodPhaseToUseACPhaseSwitchingUnsupported));
}

TEST_F(ChargepointTestFixtureV201, K01FR20_IfPhaseToUseSetAndACPhaseSwitchingSupportedFalse_ThenProfileIsInvalid) {
    auto device_model_with_false_ac_phase_switching =
        create_device_model("file:device_model2?mode=memory&cache=shared", "false");
    device_model = std::move(device_model_with_false_ac_phase_switching);

    auto periods = create_charging_schedule_periods_with_phases(0, 1, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID,
        ChargingProfileKindEnum::Absolute, 1);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut,
                testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodPhaseToUseACPhaseSwitchingUnsupported));
}

TEST_F(ChargepointTestFixtureV201, K01FR20_IfPhaseToUseSetAndACPhaseSwitchingSupportedTrue_ThenProfileIsNotInvalid) {
    auto periods = create_charging_schedule_periods_with_phases(0, 1, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID,
        ChargingProfileKindEnum::Absolute, 1);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Not(ProfileValidationResultEnum::ChargingSchedulePeriodExtraneousPhaseValues));
}

TEST_F(ChargepointTestFixtureV201,
       K01FR26_IfChargingRateUnitIsNotInChargingScheduleChargingRateUnits_ThenProfileIsInvalid) {
    const auto& charging_rate_unit_cv = ControllerComponentVariables::ChargingScheduleChargingRateUnit;
    device_model->set_value(charging_rate_unit_cv.component, charging_rate_unit_cv.variable.value(),
                            AttributeEnum::Actual, "W", true);

    auto periods = create_charging_schedule_periods(0, 1, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID,
        ChargingProfileKindEnum::Absolute, 1);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingScheduleChargingRateUnitUnsupported));
}

TEST_F(ChargepointTestFixtureV201, K01_IfChargingSchedulePeriodsAreMissing_ThenProfileIsInvalid) {
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingProfileNoChargingSchedulePeriods));
}

TEST_F(ChargepointTestFixtureV201, K01FR31_IfStartPeriodOfFirstChargingSchedulePeriodIsNotZero_ThenProfileIsInvalid) {
    auto periods = create_charging_schedule_periods(1);
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A, periods), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingProfileFirstStartScheduleIsNotZero));
}

TEST_F(ChargepointTestFixtureV201, K01FR35_IfChargingSchedulePeriodsAreNotInChonologicalOrder_ThenProfileIsInvalid) {
    auto periods = create_charging_schedule_periods({0, 2, 1});
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A, periods), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodsOutOfOrder));
}

TEST_F(ChargepointTestFixtureV201, K01_ValidateChargingStationMaxProfile_NotChargingStationMaxProfile_Invalid) {
    create_evse_with_id(STATION_WIDE_ID);
    auto periods = create_charging_schedule_periods({0, 1, 2});
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxDefaultProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A));

    auto sut = handler.validate_charging_station_max_profile(profile, STATION_WIDE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::InvalidProfileType));
}

TEST_F(ChargepointTestFixtureV201, K04FR03_ValidateChargingStationMaxProfile_EvseIDgt0_Invalid) {
    const int EVSE_ID_1 = DEFAULT_EVSE_ID;
    auto periods = create_charging_schedule_periods({0, 1, 2});
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::ChargingStationMaxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A, periods));

    auto sut = handler.validate_charging_station_max_profile(profile, EVSE_ID_1);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingStationMaxProfileEvseIdGreaterThanZero));
}

TEST_F(ChargepointTestFixtureV201, K01FR38_ChargingProfilePurposeIsChargingStationMaxProfile_KindIsAbsolute_Valid) {
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::ChargingStationMaxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A));

    auto sut = handler.validate_charging_station_max_profile(profile, STATION_WIDE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201, K01FR38_ChargingProfilePurposeIsChargingStationMaxProfile_KindIsRecurring_Valid) {
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::ChargingStationMaxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), {},
                                           ChargingProfileKindEnum::Recurring);

    auto sut = handler.validate_charging_station_max_profile(profile, STATION_WIDE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201, K01FR38_ChargingProfilePurposeIsChargingStationMaxProfile_KindIsRelative_Invalid) {
    auto profile =
        create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::ChargingStationMaxProfile,
                                create_charge_schedule(ChargingRateUnitEnum::A), {}, ChargingProfileKindEnum::Relative);

    auto sut = handler.validate_charging_station_max_profile(profile, STATION_WIDE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingStationMaxProfileCannotBeRelative));
}

TEST_F(ChargepointTestFixtureV201,
       K01FR39_IfTxProfileHasSameTransactionAndStackLevelAsAnotherTxProfile_ThenProfileIsInvalid) {
    create_evse_with_id(DEFAULT_EVSE_ID);
    open_evse_transaction(DEFAULT_EVSE_ID, DEFAULT_TX_ID);

    auto same_stack_level = 42;
    auto profile_1 = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                             create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID,
                                             ChargingProfileKindEnum::Absolute, same_stack_level);
    auto profile_2 = create_charging_profile(DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxProfile,
                                             create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID,
                                             ChargingProfileKindEnum::Absolute, same_stack_level);
    handler.add_profile(DEFAULT_EVSE_ID, profile_2);
    auto sut = handler.validate_tx_profile(profile_1, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::TxProfileConflictingStackLevel));
}

TEST_F(ChargepointTestFixtureV201,
       K01FR39_IfTxProfileHasDifferentTransactionButSameStackLevelAsAnotherTxProfile_ThenProfileIsValid) {
    create_evse_with_id(DEFAULT_EVSE_ID);
    std::string different_transaction_id = uuid();
    open_evse_transaction(DEFAULT_EVSE_ID, DEFAULT_TX_ID);

    auto same_stack_level = 42;
    auto profile_1 = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                             create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID,
                                             ChargingProfileKindEnum::Absolute, same_stack_level);
    auto profile_2 = create_charging_profile(DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxProfile,
                                             create_charge_schedule(ChargingRateUnitEnum::A), different_transaction_id,
                                             ChargingProfileKindEnum::Absolute, same_stack_level);
    handler.add_profile(DEFAULT_EVSE_ID, profile_2);
    auto sut = handler.validate_tx_profile(profile_1, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201,
       K01FR39_IfTxProfileHasSameTransactionButDifferentStackLevelAsAnotherTxProfile_ThenProfileIsValid) {
    create_evse_with_id(DEFAULT_EVSE_ID);
    open_evse_transaction(DEFAULT_EVSE_ID, DEFAULT_TX_ID);

    auto stack_level_1 = 42;
    auto stack_level_2 = 43;

    auto profile_1 = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                             create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID,
                                             ChargingProfileKindEnum::Absolute, stack_level_1);
    auto profile_2 = create_charging_profile(DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxProfile,
                                             create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID,
                                             ChargingProfileKindEnum::Absolute, stack_level_2);

    handler.add_profile(DEFAULT_EVSE_ID, profile_2);
    auto sut = handler.validate_tx_profile(profile_1, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201,
       K01FR40_IfChargingProfileKindIsAbsoluteAndStartScheduleDoesNotExist_ThenProfileIsInvalid) {
    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A, periods), DEFAULT_TX_ID,
                                           ChargingProfileKindEnum::Absolute, 1);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingProfileMissingRequiredStartSchedule));
}

TEST_F(ChargepointTestFixtureV201,
       K01FR40_IfChargingProfileKindIsRecurringAndStartScheduleDoesNotExist_ThenProfileIsInvalid) {
    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A, periods), DEFAULT_TX_ID,
                                           ChargingProfileKindEnum::Recurring, 1);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingProfileMissingRequiredStartSchedule));
}

TEST_F(ChargepointTestFixtureV201,
       K01FR41_IfChargingProfileKindIsRelativeAndStartScheduleDoesExist_ThenProfileIsInvalid) {
    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID,
        ChargingProfileKindEnum::Relative, 1);

    auto sut = handler.validate_profile_schedules(profile);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingProfileExtraneousStartSchedule));
}

TEST_F(ChargepointTestFixtureV201, K01FR28_WhenEvseDoesNotExistThenReject) {
    auto sut = handler.validate_evse_exists(DEFAULT_EVSE_ID);
    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::EvseDoesNotExist));
}

TEST_F(ChargepointTestFixtureV201, K01FR28_WhenEvseDoesExistThenAccept) {
    create_evse_with_id(DEFAULT_EVSE_ID);
    auto sut = handler.validate_evse_exists(DEFAULT_EVSE_ID);
    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

/*
 * 0 - For K01.FR.52, we return DuplicateTxDefaultProfileFound if an existing profile is on an EVSE,
 *  and a new profile with the same stack level and different profile ID will be associated with EVSE ID = 0.
 * 1 - For K01.FR.53, we return DuplicateTxDefaultProfileFound if an existing profile is on an EVSE ID = 0,
 *  and a new profile with the same stack level and different profile ID will be associated with an EVSE.
 * 2-7 - We return Valid for any other case, such as using the same EVSE or using the same profile ID.
 */
class ChargepointTestFixtureV201_FR52
    : public ChargepointTestFixtureV201,
      public ::testing::WithParamInterface<std::tuple<int, int, ProfileValidationResultEnum>> {};

INSTANTIATE_TEST_SUITE_P(TxDefaultProfileValidationV201_Param_Test_Instantiate_FR52, ChargepointTestFixtureV201_FR52,
                         testing::Values(std::make_tuple(DEFAULT_PROFILE_ID + 1, DEFAULT_STACK_LEVEL,
                                                         ProfileValidationResultEnum::DuplicateTxDefaultProfileFound),
                                         std::make_tuple(DEFAULT_PROFILE_ID, DEFAULT_STACK_LEVEL,
                                                         ProfileValidationResultEnum::Valid),
                                         std::make_tuple(DEFAULT_PROFILE_ID + 1, DEFAULT_STACK_LEVEL + 1,
                                                         ProfileValidationResultEnum::Valid)));

TEST_P(ChargepointTestFixtureV201_FR52, K01FR52_TxDefaultProfileValidationV201Tests) {
    auto [added_profile_id, added_stack_level, expected] = GetParam();
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID);

    auto profile = create_charging_profile(added_profile_id, ChargingProfilePurposeEnum::TxDefaultProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), {},
                                           ChargingProfileKindEnum::Absolute, added_stack_level);
    auto sut = handler.validate_tx_default_profile(profile, STATION_WIDE_ID);

    EXPECT_THAT(sut, testing::Eq(expected));
}

class ChargepointTestFixtureV201_FR53
    : public ChargepointTestFixtureV201,
      public ::testing::WithParamInterface<std::tuple<int, int, ProfileValidationResultEnum>> {};

INSTANTIATE_TEST_SUITE_P(TxDefaultProfileValidationV201_Param_Test_Instantiate_FR53, ChargepointTestFixtureV201_FR53,
                         testing::Values(std::make_tuple(DEFAULT_PROFILE_ID + 1, DEFAULT_STACK_LEVEL,
                                                         ProfileValidationResultEnum::DuplicateTxDefaultProfileFound),
                                         std::make_tuple(DEFAULT_PROFILE_ID, DEFAULT_STACK_LEVEL,
                                                         ProfileValidationResultEnum::Valid),
                                         std::make_tuple(DEFAULT_PROFILE_ID + 1, DEFAULT_STACK_LEVEL + 1,
                                                         ProfileValidationResultEnum::Valid)));

TEST_P(ChargepointTestFixtureV201_FR53, K01FR53_TxDefaultProfileValidationV201Tests) {
    auto [added_profile_id, added_stack_level, expected] = GetParam();
    install_profile_on_evse(STATION_WIDE_ID, DEFAULT_PROFILE_ID);
    create_evse_with_id(DEFAULT_EVSE_ID);

    auto profile = create_charging_profile(added_profile_id, ChargingProfilePurposeEnum::TxDefaultProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), {},
                                           ChargingProfileKindEnum::Absolute, added_stack_level);
    auto sut = handler.validate_tx_default_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(expected));
}

TEST_F(ChargepointTestFixtureV201, K01FR52_TxDefaultProfileValidIfAppliedToWholeSystemAgain) {
    install_profile_on_evse(STATION_WIDE_ID, DEFAULT_PROFILE_ID);

    auto profile = create_charging_profile(DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), {},
                                           ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL);
    auto sut = handler.validate_tx_default_profile(profile, STATION_WIDE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201, K01FR53_TxDefaultProfileValidIfAppliedToExistingEvseAgain) {
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID);

    auto profile = create_charging_profile(DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), {},
                                           ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL);
    auto sut = handler.validate_tx_default_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201, K01FR53_TxDefaultProfileValidIfAppliedToDifferentEvse) {
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID);
    create_evse_with_id(DEFAULT_EVSE_ID + 1);

    auto profile = create_charging_profile(DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), {},
                                           ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL);
    auto sut = handler.validate_tx_default_profile(profile, DEFAULT_EVSE_ID + 1);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201, K01FR44_IfNumberPhasesProvidedForDCEVSE_ThenProfileIsInvalid) {
    auto mock_evse = testing::NiceMock<EvseMock>();
    ON_CALL(mock_evse, get_current_phase_type).WillByDefault(testing::Return(CurrentPhaseType::DC));

    auto periods = create_charging_schedule_periods(0, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile, &mock_evse);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodExtraneousPhaseValues));
}

TEST_F(ChargepointTestFixtureV201, K01FR44_IfPhaseToUseProvidedForDCEVSE_ThenProfileIsInvalid) {
    auto mock_evse = testing::NiceMock<EvseMock>();
    ON_CALL(mock_evse, get_current_phase_type).WillByDefault(testing::Return(CurrentPhaseType::DC));

    auto periods = create_charging_schedule_periods(0, 1, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile, &mock_evse);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodExtraneousPhaseValues));
}

TEST_F(ChargepointTestFixtureV201, K01FR44_IfNumberPhasesProvidedForDCChargingStation_ThenProfileIsInvalid) {
    device_model->set_value(ControllerComponentVariables::ChargingStationSupplyPhases.component,
                            ControllerComponentVariables::ChargingStationSupplyPhases.variable.value(),
                            AttributeEnum::Actual, std::to_string(0), true);

    auto periods = create_charging_schedule_periods(0, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile, std::nullopt);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodExtraneousPhaseValues));
}

TEST_F(ChargepointTestFixtureV201, K01FR44_IfPhaseToUseProvidedForDCChargingStation_ThenProfileIsInvalid) {
    device_model->set_value(ControllerComponentVariables::ChargingStationSupplyPhases.component,
                            ControllerComponentVariables::ChargingStationSupplyPhases.variable.value(),
                            AttributeEnum::Actual, std::to_string(0), true);

    auto periods = create_charging_schedule_periods(0, 1, 1);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile, std::nullopt);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodExtraneousPhaseValues));
}

TEST_F(ChargepointTestFixtureV201, K01FR45_IfNumberPhasesGreaterThanMaxNumberPhasesForACEVSE_ThenProfileIsInvalid) {
    auto mock_evse = testing::NiceMock<EvseMock>();
    ON_CALL(mock_evse, get_current_phase_type).WillByDefault(testing::Return(CurrentPhaseType::AC));

    auto periods = create_charging_schedule_periods(0, 4);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile, &mock_evse);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodUnsupportedNumberPhases));
}

TEST_F(ChargepointTestFixtureV201,
       K01FR45_IfNumberPhasesGreaterThanMaxNumberPhasesForACChargingStation_ThenProfileIsInvalid) {
    device_model->set_value(ControllerComponentVariables::ChargingStationSupplyPhases.component,
                            ControllerComponentVariables::ChargingStationSupplyPhases.variable.value(),
                            AttributeEnum::Actual, std::to_string(1), true);

    auto periods = create_charging_schedule_periods(0, 4);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile, std::nullopt);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingSchedulePeriodUnsupportedNumberPhases));
}

TEST_F(ChargepointTestFixtureV201, K01FR49_IfNumberPhasesMissingForACEVSE_ThenSetNumberPhasesToThree) {
    auto mock_evse = testing::NiceMock<EvseMock>();
    ON_CALL(mock_evse, get_current_phase_type).WillByDefault(testing::Return(CurrentPhaseType::AC));

    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile, &mock_evse);

    auto numberPhases = profile.chargingSchedule[0].chargingSchedulePeriod[0].numberPhases;

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
    EXPECT_THAT(numberPhases, testing::Eq(3));
}

TEST_F(ChargepointTestFixtureV201, K01FR49_IfNumberPhasesMissingForACChargingStation_ThenSetNumberPhasesToThree) {
    device_model->set_value(ControllerComponentVariables::ChargingStationSupplyPhases.component,
                            ControllerComponentVariables::ChargingStationSupplyPhases.variable.value(),
                            AttributeEnum::Actual, std::to_string(3), true);

    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);

    auto sut = handler.validate_profile_schedules(profile, std::nullopt);

    auto numberPhases = profile.chargingSchedule[0].chargingSchedulePeriod[0].numberPhases;

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
    EXPECT_THAT(numberPhases, testing::Eq(3));
}

TEST_F(ChargepointTestFixtureV201, K01FR06_ExistingProfileLastsForever_RejectIncoming) {
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID, ocpp::DateTime(date::utc_clock::time_point::min()),
                            ocpp::DateTime(date::utc_clock::time_point::max()));

    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), {},
        ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL, ocpp::DateTime("2024-01-02T13:00:00"),
        ocpp::DateTime("2024-03-01T13:00:00"));

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::DuplicateProfileValidityPeriod));
}

TEST_F(ChargepointTestFixtureV201, K01FR06_ExisitingProfileHasValidFromIncomingValidToOverlaps_RejectIncoming) {
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID, ocpp::DateTime("2024-01-01T13:00:00"),
                            ocpp::DateTime(date::utc_clock::time_point::max()));

    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), {},
        ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL, {}, ocpp::DateTime("2024-01-01T13:00:00"));

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::DuplicateProfileValidityPeriod));
}

TEST_F(ChargepointTestFixtureV201, K01FR06_ExisitingProfileHasValidToIncomingValidFromOverlaps_RejectIncoming) {
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID, ocpp::DateTime("2024-02-01T13:00:00"),
                            ocpp::DateTime(date::utc_clock::time_point::max()));

    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), {},
        ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL, ocpp::DateTime("2024-01-31T13:00:00"), {});

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::DuplicateProfileValidityPeriod));
}

TEST_F(ChargepointTestFixtureV201, K01FR06_ExisitingProfileHasValidPeriodIncomingIsNowToMax_RejectIncoming) {
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID,
                            ocpp::DateTime(date::utc_clock::now() - std::chrono::hours(5 * 24)),
                            ocpp::DateTime(date::utc_clock::now() + std::chrono::hours(5 * 24)));

    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), {},
        ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL, {}, {});

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::DuplicateProfileValidityPeriod));
}

TEST_F(ChargepointTestFixtureV201, K01FR06_ExisitingProfileHasValidPeriodIncomingOverlaps_RejectIncoming) {
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID, ocpp::DateTime("2024-01-01T13:00:00"),
                            ocpp::DateTime("2024-02-01T13:00:00"));

    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), {},
        ChargingProfileKindEnum::Absolute, DEFAULT_STACK_LEVEL, ocpp::DateTime("2024-01-15T13:00:00"),
        ocpp::DateTime("2024-02-01T13:00:00"));

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::DuplicateProfileValidityPeriod));
}

TEST_F(ChargepointTestFixtureV201, K01_ValidateProfile_IfEvseDoesNotExist_ThenProfileIsInvalid) {
    auto profile = create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A), DEFAULT_TX_ID);

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID + 1);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::EvseDoesNotExist));
}

TEST_F(ChargepointTestFixtureV201, K01_ValidateProfile_IfScheduleIsInvalid_ThenProfileIsInvalid) {
    create_evse_with_id(DEFAULT_EVSE_ID);

    auto extraneous_start_schedule = ocpp::DateTime("2024-01-17T17:00:00");
    auto periods = create_charging_schedule_periods(0);
    auto profile =
        create_charging_profile(DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
                                create_charge_schedule(ChargingRateUnitEnum::A, periods, extraneous_start_schedule),
                                DEFAULT_TX_ID, ChargingProfileKindEnum::Relative, 1);

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingProfileExtraneousStartSchedule));
}

TEST_F(ChargepointTestFixtureV201, K01_ValidateProfile_IfChargeStationMaxProfileIsInvalid_ThenProfileIsInvalid) {
    create_evse_with_id(DEFAULT_EVSE_ID);
    auto periods = create_charging_schedule_periods({0, 1, 2});
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::ChargingStationMaxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")));

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::ChargingStationMaxProfileEvseIdGreaterThanZero));
}

TEST_F(ChargepointTestFixtureV201,
       K01_ValidateProfile_IfDuplicateTxDefaultProfileFoundOnEVSE_IsInvalid_ThenProfileIsInvalid) {
    install_profile_on_evse(DEFAULT_EVSE_ID, DEFAULT_PROFILE_ID);

    auto periods = create_charging_schedule_periods(0);

    auto profile = create_charging_profile(DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A, periods), {},
                                           ChargingProfileKindEnum::Relative, DEFAULT_STACK_LEVEL);

    auto sut = handler.validate_profile(profile, STATION_WIDE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::DuplicateTxDefaultProfileFound));
}

TEST_F(ChargepointTestFixtureV201,
       K01_ValidateProfile_IfDuplicateTxDefaultProfileFoundOnChargingStation_IsInvalid_ThenProfileIsInvalid) {
    install_profile_on_evse(STATION_WIDE_ID, DEFAULT_PROFILE_ID);
    create_evse_with_id(DEFAULT_EVSE_ID);

    auto periods = create_charging_schedule_periods(0);

    auto profile = create_charging_profile(DEFAULT_PROFILE_ID + 1, ChargingProfilePurposeEnum::TxDefaultProfile,
                                           create_charge_schedule(ChargingRateUnitEnum::A, periods), {},
                                           ChargingProfileKindEnum::Relative, DEFAULT_STACK_LEVEL);

    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::DuplicateTxDefaultProfileFound));
}

TEST_F(ChargepointTestFixtureV201, K01_ValidateProfile_IfTxProfileIsInvalid_ThenProfileIsInvalid) {
    create_evse_with_id(DEFAULT_EVSE_ID);
    auto periods = create_charging_schedule_periods(0);
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")));
    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::TxProfileMissingTransactionId));
}

TEST_F(ChargepointTestFixtureV201, K01_ValidateProfile_IfTxProfileIsValid_ThenProfileIsValid) {
    auto periods = create_charging_schedule_periods({0, 1, 2});

    create_evse_with_id(DEFAULT_EVSE_ID);
    open_evse_transaction(DEFAULT_EVSE_ID, DEFAULT_TX_ID);

    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")), DEFAULT_TX_ID);
    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201, K01_ValidateProfile_IfTxDefaultProfileIsValid_ThenProfileIsValid) {
    auto periods = create_charging_schedule_periods({0, 1, 2});

    create_evse_with_id(DEFAULT_EVSE_ID);

    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::TxDefaultProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")));
    auto sut = handler.validate_profile(profile, DEFAULT_EVSE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

TEST_F(ChargepointTestFixtureV201, K01_ValidateProfile_IfChargeStationMaxProfileIsValid_ThenProfileIsValid) {
    auto periods = create_charging_schedule_periods({0, 1, 2});
    auto profile = create_charging_profile(
        DEFAULT_PROFILE_ID, ChargingProfilePurposeEnum::ChargingStationMaxProfile,
        create_charge_schedule(ChargingRateUnitEnum::A, periods, ocpp::DateTime("2024-01-17T17:00:00")));

    auto sut = handler.validate_profile(profile, STATION_WIDE_ID);

    EXPECT_THAT(sut, testing::Eq(ProfileValidationResultEnum::Valid));
}

} // namespace ocpp::v201
