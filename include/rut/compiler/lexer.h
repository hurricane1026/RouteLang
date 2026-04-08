#pragma once

#include "rut/common/types.h"

namespace rut {

// Token types for the .rut language
enum class TokenType : u8 {
    // Literals
    Ident,
    StringLit,
    IntLit,
    FloatLit,

    // Keywords
    KwFunc,
    KwLet,
    KwVar,
    KwConst,
    KwGuard,
    KwStruct,
    KwRoute,
    KwMatch,
    KwIf,
    KwElse,
    KwFor,
    KwIn,
    KwReturn,
    KwUpstream,
    KwListen,
    KwTls,
    KwDefaults,
    KwForward,
    KwWebsocket,
    KwImport,
    KwUsing,
    KwAs,
    KwFire,
    KwNotify,
    KwDefer,
    KwSubmit,
    KwWait,
    KwTimer,
    KwInit,
    KwShutdown,
    KwFirewall,
    KwThrottle,
    KwPer,
    KwAnd,
    KwOr,
    KwNot,
    KwNil,
    KwTrue,
    KwFalse,

    // HTTP methods
    KwGet,
    KwPost,
    KwPut,
    KwDelete,
    KwPatch,
    KwHead,
    KwOptions,

    // Regex literal
    RegexLit,   // re"pattern"

    // Symbols
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Colon,
    Comma,
    Dot,
    Arrow,      // => (single expression, implicit return)
    ThinArrow,  // ->
    Eq,
    EqEq,
    BangEq,
    Lt,
    Gt,
    LtEq,
    GtEq,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Bang,
    Question,
    DoubleQuestion,  // ?? (null coalescing)
    At,
    DoubleStar,  // **
    Underscore,  // _

    // Special
    Eof,
    Error,
};

struct Token {
    TokenType type;
    Str text;
    u32 line;
    u32 col;
};

}  // namespace rut
