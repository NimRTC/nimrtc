# examples/demo-agent-gateway/CMakeLists.txt
# ----------------------------------------------------------------------------
# demo-agent-gateway — PCM tap → streaming ASR → LLM mock flow demo.
# ----------------------------------------------------------------------------

if(NOT NIMRTC_BUILD_EXAMPLES)
    return()
endif()

add_executable(demo-agent-gateway demo-agent-gateway.cpp)
nimrtc_apply_options(demo-agent-gateway)
# See examples/CMakeLists.txt for the PAL Slice 2 datachannel link rationale.
target_link_libraries(demo-agent-gateway PRIVATE
    nimrtc::assembly
    nimrtc::engine
    nimrtc::datachannel)

# Stage Chrome-friendly DTLS cert (same as loopback-p2p)
add_custom_command(
    TARGET demo-agent-gateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:demo-agent-gateway>/certs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/third_party/wolfssl/src/certs/server-ecc.pem"
        "$<TARGET_FILE_DIR:demo-agent-gateway>/certs/server-ecc.pem"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/third_party/wolfssl/src/certs/ecc-key.pem"
        "$<TARGET_FILE_DIR:demo-agent-gateway>/certs/ecc-key.pem"
    COMMENT "Staging Chrome-friendly DTLS cert for demo-agent-gateway"
)
