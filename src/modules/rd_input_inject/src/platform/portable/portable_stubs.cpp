/**
 * @file src/modules/rd_input_inject/src/platform/portable/portable_stubs.cpp
 * @brief The stub backend — the "this platform has no injector" answer.
 *
 * See `src/stub_injector.hpp` for why the stub exists.  This file is
 * the *portable* translation unit that carries it on the platforms that
 * have no native backend.
 */

#include "../../stub_injector.hpp"

namespace nimrtc::rd_input_inject {

// The stub's definition lives in the shared header, so the platform
// dispatch can construct it on every platform.  This translation unit
// exists so that a platform with no native backend still compiles one
// of the module's sources (and so the stub is not an unreferenced
// header on those platforms).

} // namespace nimrtc::rd_input_inject
