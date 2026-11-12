// Minimal reproduction of the GCC-only failure that broke PR #51's
// Linux legs.
//
// The Phase 1 change made TypedRegistry own its keys (`std::string`
// instead of `std::string_view`) so a plugin author could register
// with a temporary `std::string` without leaving a dangling view.
//
// But registry.hpp included only <string_view>.  <string_view> does not
// make `std::string` a complete type, and libstdc++'s std::pair
// instantiates its members eagerly -- so
//
//     std::vector<std::pair<std::string, const T*>> entries_;
//
// fails with
//
//     error: 'std::pair<_T1, _T2>::first' has incomplete type
//
// MSVC and libc++ happened to paper over it (both pull <string> in
// transitively), which is why only the Linux legs went red.
//
// This TU reproduces the failure using the real header.  Compile it
// standalone; it must produce no diagnostics.
//
//   g++ -std=c++20 -fsyntax-only -I src/core/include \
//       -I src/plugins/include \
//       tools/repro_registry_string.cpp

#include <nimrtc/core/registry.hpp>

// Force the instantiation the way pal_default_registrars.cpp does.
namespace nimrtc::core {
namespace {

void instantiate() {
    TypedRegistry<plugins::ITransportFactory> reg;
    reg.register_one("x", nullptr);
    (void)reg.get("x");
}

} // namespace
} // namespace nimrtc::core

int main() {
    nimrtc::core::instantiate();
    return 0;
}
