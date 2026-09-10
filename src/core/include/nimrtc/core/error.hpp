#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace nimrtc::core {

// -----------------------------------------------------------------------------
// Error codes
// -----------------------------------------------------------------------------
// Keep this enum stable. Module-specific codes may be appended in their own
// headers, but core codes are part of the cross-module ABI.
enum class ErrorCode : std::uint8_t {
    Ok                  = 0,
    InvalidArgument     = 1,   // argument shape / value rejected
    InvalidState        = 2,   // API called in wrong state machine state
    BufferTooSmall      = 3,   // output buffer too small for requested data
    OutOfMemory         = 4,   // allocation failed
    Timeout             = 5,   // operation timed out
    NetworkError        = 6,   // socket / IO / wire error
    ProtocolError       = 7,   // malformed or unexpected protocol payload
    CryptoError         = 8,   // DTLS / SRTP keying / cipher failure
    NotImplemented      = 9,   // capability not yet implemented
    NotFound            = 10,  // stream / session / config not found
    Cancelled           = 11,  // operation cancelled by caller
    InternalError       = 99,  // bug — should not happen
};

const char* to_string(ErrorCode code) noexcept;

// -----------------------------------------------------------------------------
// Error
// -----------------------------------------------------------------------------
class Error {
public:
    Error() noexcept = default;
    Error(ErrorCode code, std::string message) noexcept
        : code_(code), message_(std::move(message)) {}

    ErrorCode           code()    const noexcept { return code_; }
    const std::string&  message() const noexcept { return message_; }
    std::string_view    message_view() const noexcept { return message_; }

    bool                ok()      const noexcept { return code_ == ErrorCode::Ok; }
    explicit operator   bool()    const noexcept { return ok(); }

private:
    ErrorCode  code_    = ErrorCode::Ok;
    std::string message_;
};

// -----------------------------------------------------------------------------
// Result<T>
// -----------------------------------------------------------------------------
// A variant-free Result<T> using tag dispatch. T must be move-constructible.
template <typename T>
class Result {
public:
    static Result ok(T value) {
        return Result(std::move(value), ok_tag{});
    }
    static Result fail(ErrorCode code, std::string message) {
        return Result(Error{code, std::move(message)}, err_tag{});
    }
    static Result fail(const Error& e) {
        return Result(e, err_tag{});
    }

    bool ok() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    T&       value()       &  { return value_; }
    const T& value() const &  { return value_; }
    T&&      value()       && { return std::move(value_); }

    Error&       error()       &  { return error_; }
    const Error& error() const &  { return error_; }

private:
    struct ok_tag {};
    struct err_tag {};

    Result(T v, ok_tag) noexcept(std::is_nothrow_move_constructible_v<T>)
        : has_value_(true), value_(std::move(v)) {}
    Result(Error e, err_tag) noexcept
        : has_value_(false), error_(std::move(e)) {}

    bool  has_value_ = false;
    T     value_{};
    Error error_{};
};

// -----------------------------------------------------------------------------
// Result<void>
// -----------------------------------------------------------------------------
template <>
class Result<void> {
public:
    // Named `make_ok()` to avoid signature collision with `bool ok() const noexcept`
    // on MSVC (C2686) in template specializations.
    static Result make_ok() { return Result(ok_tag{}); }
    static Result fail(ErrorCode code, std::string message) {
        return Result(Error{code, std::move(message)}, err_tag{});
    }
    static Result fail(const Error& e) {
        return Result(e, err_tag{});
    }

    bool ok() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    Error&       error()       &  { return error_; }
    const Error& error() const &  { return error_; }

private:
    // Tag-dispatch constructors — mirror Result<T>'s pattern so the ok and
    // err paths are symmetric.  The default constructor is private and only
    // callable through the explicit make_ok() factory.
    struct ok_tag {};
    struct err_tag {};

    Result(ok_tag) noexcept : has_value_(true) {}
    Result(Error e, err_tag) noexcept
        : has_value_(false), error_(std::move(e)) {}

    bool  has_value_ = true;
    Error error_{};
};

} // namespace nimrtc::core