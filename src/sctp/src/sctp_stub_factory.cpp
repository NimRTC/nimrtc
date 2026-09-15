/**
 * @file src/sctp/src/sctp_stub_factory.cpp
 * @brief SctpStubFactory — registration id "stub" (NOT "usrsctp").
 *
 * All behaviour lives in the header; this .cpp exists so the static lib
 * carries the symbol for the static-link workaround (same pattern as
 * `nimrtc::ice::IceTransportFactory` and `nimrtc::audio3a::NullPluginFactory`).
 */

#include "sctp_stub_factory.hpp"
