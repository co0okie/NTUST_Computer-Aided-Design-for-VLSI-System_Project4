#pragma once

#include <string>
#include <utility>
#include <variant>

// Rust-style Result: functions that can fail return Result<T> instead of throwing.
struct Error {
    std::string message;
};

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}

    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    T &value() & { return std::get<T>(data_); }
    T const &value() const & { return std::get<T>(data_); }
    T &&value() && { return std::get<T>(std::move(data_)); }
    Error const &error() const { return std::get<Error>(data_); }

private:
    std::variant<T, Error> data_;
};

struct Unit {};

inline Error err(std::string message) { return Error{std::move(message)}; }

// Early return on failure: TRY(auto x, f()); or TRY_VOID(g());
#define MLRCS_CONCAT_(a, b) a##b
#define MLRCS_CONCAT(a, b) MLRCS_CONCAT_(a, b)
#define TRY(decl, expr) TRY_IMPL_(decl, expr, MLRCS_CONCAT(try_result_, __LINE__))
#define TRY_IMPL_(decl, expr, tmp)                                                                 \
    auto tmp = (expr);                                                                             \
    if (!tmp.ok()) return tmp.error();                                                             \
    decl = std::move(tmp).value()
#define TRY_VOID(expr)                                                                             \
    do {                                                                                           \
        auto try_result_void_ = (expr);                                                            \
        if (!try_result_void_.ok()) return try_result_void_.error();                               \
    } while (0)
