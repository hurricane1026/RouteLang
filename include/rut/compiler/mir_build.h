#pragma once

#include "rut/compiler/hir.h"
#include "rut/compiler/mir.h"

namespace rut {

FrontendResult<MirModule*> build_mir(const HirModule& module);

}
