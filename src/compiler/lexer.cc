#include "rut/compiler/lexer.h"

namespace rut {

namespace {

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_continue(char c) {
    return is_ident_start(c) || is_digit(c);
}

static TokenType keyword_type(Str text) {
    if (text.eq({"func", 4})) return TokenType::KwFunc;
    if (text.eq({"let", 3})) return TokenType::KwLet;
    if (text.eq({"const", 5})) return TokenType::KwConst;
    if (text.eq({"guard", 5})) return TokenType::KwGuard;
    if (text.eq({"case", 4})) return TokenType::KwCase;
    if (text.eq({"error", 5})) return TokenType::KwError;
    if (text.eq({"protocol", 8})) return TokenType::KwProtocol;
    if (text.eq({"impl", 4})) return TokenType::KwImpl;
    if (text.eq({"import", 6})) return TokenType::KwImport;
    if (text.eq({"package", 7})) return TokenType::KwPackage;
    if (text.eq({"using", 5})) return TokenType::KwUsing;
    if (text.eq({"as", 2})) return TokenType::KwAs;
    if (text.eq({"variant", 7})) return TokenType::KwVariant;
    if (text.eq({"struct", 6})) return TokenType::KwStruct;
    if (text.eq({"match", 5})) return TokenType::KwMatch;
    if (text.eq({"if", 2})) return TokenType::KwIf;
    if (text.eq({"else", 4})) return TokenType::KwElse;
    if (text.eq({"for", 3})) return TokenType::KwFor;
    if (text.eq({"or", 2})) return TokenType::KwOr;
    if (text.eq({"nil", 3})) return TokenType::KwNil;
    if (text.eq({"upstream", 8})) return TokenType::KwUpstream;
    if (text.eq({"route", 5})) return TokenType::KwRoute;
    if (text.eq({"return", 6})) return TokenType::KwReturn;
    if (text.eq({"forward", 7})) return TokenType::KwForward;
    if (text.eq({"true", 4})) return TokenType::KwTrue;
    if (text.eq({"false", 5})) return TokenType::KwFalse;
    if (text.eq({"GET", 3}) || text.eq({"get", 3})) return TokenType::KwGet;
    if (text.eq({"POST", 4}) || text.eq({"post", 4})) return TokenType::KwPost;
    if (text.eq({"PUT", 3}) || text.eq({"put", 3})) return TokenType::KwPut;
    if (text.eq({"DELETE", 6}) || text.eq({"delete", 6})) return TokenType::KwDelete;
    if (text.eq({"PATCH", 5}) || text.eq({"patch", 5})) return TokenType::KwPatch;
    if (text.eq({"HEAD", 4}) || text.eq({"head", 4})) return TokenType::KwHead;
    if (text.eq({"OPTIONS", 7}) || text.eq({"options", 7})) return TokenType::KwOptions;
    return TokenType::Ident;
}

static Span token_span(const Token& tok) {
    return Span{tok.start, tok.end, tok.line, tok.col};
}

}  // namespace

LexResult lex(Str source) {
    LexedTokens out{};
    u32 pos = 0;
    u32 line = 1;
    u32 col = 1;

    while (pos < source.len) {
        const char c = source.ptr[pos];

        if (is_space(c)) {
            if (c == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
            pos++;
            continue;
        }

        if (c == '/' && pos + 1 < source.len && source.ptr[pos + 1] == '/') {
            pos += 2;
            col += 2;
            while (pos < source.len && source.ptr[pos] != '\n') {
                pos++;
                col++;
            }
            continue;
        }

        Token tok{};
        tok.start = pos;
        tok.line = line;
        tok.col = col;

        if (is_ident_start(c)) {
            pos++;
            col++;
            while (pos < source.len && is_ident_continue(source.ptr[pos])) {
                pos++;
                col++;
            }
            tok.end = pos;
            tok.text = source.slice(tok.start, tok.end);
            tok.type = tok.text.len == 1 && tok.text.ptr[0] == '_' ? TokenType::Underscore
                                                                   : keyword_type(tok.text);
            if (!out.tokens.push(tok)) return frontend_error(FrontendError::TooManyTokens, token_span(tok));
            continue;
        }

        if (is_digit(c)) {
            pos++;
            col++;
            while (pos < source.len && is_digit(source.ptr[pos])) {
                pos++;
                col++;
            }
            tok.end = pos;
            tok.text = source.slice(tok.start, tok.end);
            tok.type = TokenType::IntLit;
            if (!out.tokens.push(tok)) return frontend_error(FrontendError::TooManyTokens, token_span(tok));
            continue;
        }

        if (c == '"') {
            const u32 quote_start = pos;
            pos++;
            col++;
            while (pos < source.len) {
                const char cur = source.ptr[pos];
                if (cur == '"') break;
                if (cur == '\\') {
                    if (pos + 1 >= source.len) {
                        return frontend_error(
                            FrontendError::UnterminatedString, Span{quote_start, pos, tok.line, tok.col});
                    }
                    pos += 2;
                    col += 2;
                    continue;
                }
                if (cur == '\n') {
                    return frontend_error(
                        FrontendError::UnterminatedString, Span{quote_start, pos, tok.line, tok.col});
                }
                pos++;
                col++;
            }
            if (pos >= source.len || source.ptr[pos] != '"') {
                return frontend_error(
                    FrontendError::UnterminatedString, Span{quote_start, pos, tok.line, tok.col});
            }
            tok.type = TokenType::StringLit;
            tok.start = quote_start + 1;
            tok.end = pos;
            tok.text = source.slice(tok.start, tok.end);
            pos++;
            col++;
            if (!out.tokens.push(tok)) return frontend_error(FrontendError::TooManyTokens, token_span(tok));
            continue;
        }

        pos++;
        col++;
        tok.end = pos;
        tok.text = source.slice(tok.start, tok.end);
        if (c == '=' && pos < source.len && source.ptr[pos] == '=') {
            pos++;
            col++;
            tok.end = pos;
            tok.text = source.slice(tok.start, tok.end);
            tok.type = TokenType::EqEq;
            if (!out.tokens.push(tok)) return frontend_error(FrontendError::TooManyTokens, token_span(tok));
            continue;
        }
        if (c == '=' && pos < source.len && source.ptr[pos] == '>') {
            pos++;
            col++;
            tok.end = pos;
            tok.text = source.slice(tok.start, tok.end);
            tok.type = TokenType::Arrow;
            if (!out.tokens.push(tok)) return frontend_error(FrontendError::TooManyTokens, token_span(tok));
            continue;
        }
        if (c == '-' && pos < source.len && source.ptr[pos] == '>') {
            pos++;
            col++;
            tok.end = pos;
            tok.text = source.slice(tok.start, tok.end);
            tok.type = TokenType::ThinArrow;
            if (!out.tokens.push(tok)) return frontend_error(FrontendError::TooManyTokens, token_span(tok));
            continue;
        }
        switch (c) {
            case '{':
                tok.type = TokenType::LBrace;
                break;
            case '}':
                tok.type = TokenType::RBrace;
                break;
            case '(':
                tok.type = TokenType::LParen;
                break;
            case ')':
                tok.type = TokenType::RParen;
                break;
            case ',':
                tok.type = TokenType::Comma;
                break;
            case ':':
                tok.type = TokenType::Colon;
                break;
            case '.':
                tok.type = TokenType::Dot;
                break;
            case '<':
                tok.type = TokenType::Lt;
                break;
            case '>':
                tok.type = TokenType::Gt;
                break;
            case '*':
                tok.type = TokenType::Star;
                break;
            case '|':
                tok.type = TokenType::Pipe;
                break;
            case '=':
                tok.type = TokenType::Eq;
                break;
            case '@':
                tok.type = TokenType::At;
                break;
            default:
                return frontend_error(FrontendError::UnexpectedChar, token_span(tok), tok.text);
        }
        if (!out.tokens.push(tok)) return frontend_error(FrontendError::TooManyTokens, token_span(tok));
    }

    Token eof{};
    eof.type = TokenType::Eof;
    eof.start = source.len;
    eof.end = source.len;
    eof.line = line;
    eof.col = col;
    eof.text = {source.ptr + source.len, 0};
    if (!out.tokens.push(eof)) return frontend_error(FrontendError::TooManyTokens, token_span(eof));
    return out;
}

}  // namespace rut
