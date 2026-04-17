#pragma once

#include "rut/compiler/ast.h"
#include "rut/compiler/hir.h"

namespace rut {

FrontendResult<HirModule*> analyze_file(const AstFile& file);
FrontendResult<HirModule*> analyze_file(const AstFile& file, Str source_path);
void reset_import_analysis_counter();
u32 get_import_analysis_counter();

}  // namespace rut
