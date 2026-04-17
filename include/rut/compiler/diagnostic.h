#pragma once

#include "core/expected.h"
#include "rut/common/types.h"

namespace rut {

enum class FrontendError : u8 {
    UnexpectedChar,
    UnterminatedString,
    InvalidInteger,
    UnexpectedToken,
    UnexpectedEof,
    TooManyTokens,
    TooManyItems,
    InvalidStatusCode,
    DuplicateUpstream,
    UnknownUpstream,
    OutOfMemory,
    UnsupportedSyntax,
};

struct Span {
    u32 start = 0;
    u32 end = 0;
    u32 line = 1;
    u32 col = 1;
};

struct Diagnostic {
    FrontendError code = FrontendError::UnexpectedToken;
    Span span{};
    Str detail{};
};

template <typename T>
using FrontendResult = core::Expected<T, Diagnostic>;

inline auto frontend_error(FrontendError code, Span span = {}, Str detail = {}) {
    return core::make_unexpected(Diagnostic{code, span, detail});
}

}  // namespace rut
