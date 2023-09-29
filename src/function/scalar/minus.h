//
// Created by jinhai on 23-1-10.
//

#pragma once

#include "function/scalar_function.h"
#include "function/function_set.h"
#include "storage/meta/catalog.h"

namespace infinity {

void
RegisterMinusFunction(const UniquePtr<NewCatalog>& catalog_ptr);

}

