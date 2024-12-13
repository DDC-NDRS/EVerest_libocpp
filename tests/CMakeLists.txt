set(CONFIG_DIR_V16 "${CMAKE_CURRENT_BINARY_DIR}/resources/v16/")
set(MIGRATION_FILES_LOCATION_V16 "${CMAKE_CURRENT_BINARY_DIR}/resources/v16/migration_files")
set(MIGRATION_FILES_LOCATION_V201 "${CMAKE_CURRENT_BINARY_DIR}/resources/v201/migration_files")
set(CONFIG_FILE_LOCATION_V16 ${CMAKE_CURRENT_SOURCE_DIR}/config/v16/resources/config.json)
set(USER_CONFIG_FILE_LOCATION_V16 ${CMAKE_CURRENT_SOURCE_DIR}/config/v16/resources/user_config.json)
set(CONFIG_FILE_RESOURCES_LOCATION_V16 "${CMAKE_CURRENT_BINARY_DIR}/resources/config/v16/config.json")
set(USER_CONFIG_FILE_RESOURCES_LOCATION_V16 "${CMAKE_CURRENT_BINARY_DIR}/resources/config/v16/user_config.json")

set(MIGRATION_FILES_DEVICE_MODEL_LOCATION_V201 "${CMAKE_CURRENT_BINARY_DIR}/resources/v201/device_model_migration_files")
set(DEVICE_MODEL_DB_LOCATION_V201 "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/everest/modules/OCPP201/device_model_storage.db")
set(DEVICE_MODEL_RESOURCES_LOCATION_V201 "${CMAKE_CURRENT_BINARY_DIR}/resources/config/v201")
set(DEVICE_MODEL_RESOURCES_CHANGED_LOCATION_V201 "${CMAKE_CURRENT_BINARY_DIR}/resources/config/v201/changed")
set(DEVICE_MODEL_RESOURCES_WRONG_LOCATION_V201 "${CMAKE_CURRENT_BINARY_DIR}/resources/config/v201/wrong")
set(DEVICE_MODEL_CURRENT_RESOURCES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/v201/resources)
set(DEVICE_MODEL_CURRENT_CHANGED_RESOURCES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/v201/resources_changed)
set(DEVICE_MODEL_CURRENT_WRONG_RESOURCES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/v201/resources_wrong)
set(DEVICE_MODEL_EXAMPLE_CONFIG_LOCATION_V201 "${CMAKE_CURRENT_BINARY_DIR}/resources/example_config/v201/component_config")
set(DEVICE_MODEL_CURRENT_EXAMPLE_CONFIG_LOCATION_V201 "${PROJECT_SOURCE_DIR}/config/v201/component_config")
set(TEST_PROFILES_LOCATION_V16 "${CMAKE_CURRENT_BINARY_DIR}/resources/profiles/v16")
set(TEST_PROFILES_LOCATION_V201 "${CMAKE_CURRENT_BINARY_DIR}/resources/profiles/v201")


add_executable(libocpp_unit_tests
    config/v201/resources_wrong/component_config_required_no_value/standardized/UnitTestCtrlr.json)

target_compile_definitions(libocpp_unit_tests
    PRIVATE
    CONFIG_FILE_LOCATION_V16="${CONFIG_FILE_RESOURCES_LOCATION_V16}"
    USER_CONFIG_FILE_LOCATION_V16="${USER_CONFIG_FILE_RESOURCES_LOCATION_V16}"
    CONFIG_DIR_V16="${CONFIG_DIR_V16}"
    MIGRATION_FILES_LOCATION_V16="${MIGRATION_FILES_LOCATION_V16}"
    MIGRATION_FILES_LOCATION_V201="${MIGRATION_FILES_LOCATION_V201}"
    MIGRATION_FILE_VERSION_V16=${MIGRATION_FILE_VERSION_V16}
    MIGRATION_FILE_VERSION_V201=${MIGRATION_FILE_VERSION_V201}
    DEVICE_MODEL_DB_LOCATION_V201="${DEVICE_MODEL_DB_LOCATION_V201}"
    TEST_PROFILES_LOCATION_V16="${TEST_PROFILES_LOCATION_V16}"
    TEST_PROFILES_LOCATION_V201="${TEST_PROFILES_LOCATION_V201}"
)

target_compile_options(libocpp_unit_tests
    PRIVATE
        -pedantic-errors
)

target_compile_features(libocpp_unit_tests PUBLIC cxx_std_17)

add_custom_command(TARGET libocpp_unit_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/resources/unittest_device_model.db ${CMAKE_CURRENT_BINARY_DIR}/resources/unittest_device_model.db
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/resources/unittest_device_model_missing_required.db ${CMAKE_CURRENT_BINARY_DIR}/resources/unittest_device_model_missing_required.db
        COMMAND ${CMAKE_COMMAND} -E copy ${CONFIG_FILE_LOCATION_V16} ${CONFIG_FILE_RESOURCES_LOCATION_V16}
        COMMAND ${CMAKE_COMMAND} -E copy ${USER_CONFIG_FILE_LOCATION_V16} ${USER_CONFIG_FILE_RESOURCES_LOCATION_V16}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CONFIG_DIR_V16}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${OCPP1_6_CONFIG_DIR} ${CONFIG_DIR_V16}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${MIGRATION_FILES_LOCATION_V16}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${MIGRATION_FILES_SOURCE_DIR_V16} ${MIGRATION_FILES_LOCATION_V16}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${MIGRATION_FILES_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${MIGRATION_FILES_SOURCE_DIR_V201} ${MIGRATION_FILES_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${MIGRATION_FILES_DEVICE_MODEL_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${MIGRATION_FILES_DEVICE_MODEL_SOURCE_DIR_V201} ${MIGRATION_FILES_DEVICE_MODEL_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${DEVICE_MODEL_RESOURCES_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${DEVICE_MODEL_CURRENT_RESOURCES_DIR} ${DEVICE_MODEL_RESOURCES_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${DEVICE_MODEL_RESOURCES_CHANGED_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${DEVICE_MODEL_CURRENT_CHANGED_RESOURCES_DIR} ${DEVICE_MODEL_RESOURCES_CHANGED_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${DEVICE_MODEL_RESOURCES_WRONG_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${DEVICE_MODEL_CURRENT_WRONG_RESOURCES_DIR} ${DEVICE_MODEL_RESOURCES_WRONG_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${DEVICE_MODEL_EXAMPLE_CONFIG_LOCATION_V201}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${DEVICE_MODEL_CURRENT_EXAMPLE_CONFIG_LOCATION_V201} ${DEVICE_MODEL_EXAMPLE_CONFIG_LOCATION_V201}
)

add_test(libocpp_unit_tests libocpp_unit_tests)

target_include_directories(libocpp_unit_tests PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/ocpp/common
)
target_link_libraries(libocpp_unit_tests PRIVATE
        ocpp
        GTest::gmock_main
        GTest::gtest_main
)

add_subdirectory(lib/ocpp/common)

if(LIBOCPP_ENABLE_V16)
    add_subdirectory(lib/ocpp/v16)
endif()

if(LIBOCPP_ENABLE_V201)
    add_subdirectory(lib/ocpp/v201)
endif()

setup_target_for_coverage_gcovr_html(
    NAME ${PROJECT_NAME}_gcovr_coverage
    EXECUTABLE ctest
    DEPENDENCIES libocpp_unit_tests
    EXCLUDE "src/*" "tests/*"
)

setup_target_for_coverage_gcovr_xml(
    NAME ${PROJECT_NAME}_gcovr_coverage_xml
    EXECUTABLE ctest
    DEPENDENCIES libocpp_unit_tests
    EXCLUDE "src/*" "tests/*"
)
