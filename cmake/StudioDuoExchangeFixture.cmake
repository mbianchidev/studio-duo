cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED TEST_BINARY OR NOT IS_ABSOLUTE "${TEST_BINARY}" OR NOT EXISTS "${TEST_BINARY}")
    message(FATAL_ERROR "TEST_BINARY must name the built native StudioDuoTests executable.")
endif()
if(NOT DEFINED OUTPUT_DIRECTORY OR NOT IS_ABSOLUTE "${OUTPUT_DIRECTORY}")
    message(FATAL_ERROR "OUTPUT_DIRECTORY must be an absolute native-exchange artifact directory.")
endif()

get_filename_component(STUDIO_DUO_VERIFIER_NAME "${TEST_BINARY}" NAME)
if(NOT STUDIO_DUO_VERIFIER_NAME MATCHES "^StudioDuoTests(\\.exe)?$")
    message(FATAL_ERROR "The native exchange fixture requires StudioDuoTests, not the application.")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(COPY "${TEST_BINARY}" DESTINATION "${OUTPUT_DIRECTORY}")
execute_process(
    COMMAND "${TEST_BINARY}" "--write-exchange-fixture=${OUTPUT_DIRECTORY}/project.studioduo"
    RESULT_VARIABLE STUDIO_DUO_EXCHANGE_RESULT
    OUTPUT_VARIABLE STUDIO_DUO_EXCHANGE_OUTPUT
    ERROR_VARIABLE STUDIO_DUO_EXCHANGE_ERROR
    TIMEOUT 30
)
if(NOT STUDIO_DUO_EXCHANGE_RESULT STREQUAL "0")
    message(FATAL_ERROR
        "Native exchange fixture failed (${STUDIO_DUO_EXCHANGE_RESULT}):\n"
        "${STUDIO_DUO_EXCHANGE_OUTPUT}${STUDIO_DUO_EXCHANGE_ERROR}"
    )
endif()
message(STATUS "${STUDIO_DUO_EXCHANGE_OUTPUT}")
