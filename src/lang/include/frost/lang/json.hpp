// SPDX-License-Identifier: Apache-2.0
//
// frost::lang::Json: the JSON value the LSP client speaks in. It is frost::rpc::Json (frost/rpc/json.hpp),
// which moved to frost::rpc so the DAP client (frost::debug) shares it (ADR 0050); this header keeps
// the frost::lang spelling for the LSP code and its users.
#pragma once

#include "frost/rpc/json.hpp"

namespace frost::lang {

using Json = rpc::Json;
using rpc::parseJson;

} // namespace frost::lang
