#pragma once

#include "rut/compiler/ast.h"
#include "rut/compiler/lexer.h"

namespace rut {

FrontendResult<AstFile*> parse_file(const LexedTokens& tokens);

}
