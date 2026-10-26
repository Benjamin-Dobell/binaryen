/*
 * Copyright 2024 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "support/json.h"
#include "support/string.h"

#include <locale>
#include <sstream>

namespace json {

void Value::stringify(std::ostream& os, bool pretty) {
  if (isString()) {
    std::stringstream wtf16;
    [[maybe_unused]] bool valid =
      wasm::String::convertWTF8ToWTF16(wtf16, getIString().str);
    assert(valid);
    // TODO: Use wtf16.view() once we have C++20.
    wasm::String::printEscapedJSON(os, wtf16.str());
  } else if (isArray()) {
    os << '[';
    auto first = true;
    for (auto& item : getArray()) {
      if (first) {
        first = false;
      } else {
        // TODO pretty whitespace
        os << ',';
      }
      item->stringify(os, pretty);
    }
    os << ']';
  } else if (isObject()) {
    // ObjectStorage is unordered. Keep serialized compiler metadata stable
    // across runs and platforms by sorting the keys by their string contents.
    std::vector<IString> keys;
    keys.reserve(obj->size());
    for (auto& item : *obj) {
      keys.push_back(item.first);
    }
    std::sort(keys.begin(), keys.end());
    os << '{';
    auto first = true;
    for (auto key : keys) {
      if (first) {
        first = false;
      } else {
        os << ',';
      }
      Value name;
      name.setString(key);
      name.stringify(os, pretty);
      os << ':';
      (*obj)[key]->stringify(os, pretty);
    }
    os << '}';
  } else if (isNumber()) {
    // JSON has no nonfinite numeric literals. Match JSON.stringify by emitting
    // null for those values. A separate classic-locale stream also prevents a
    // caller's locale, precision, or float flags from changing valid JSON.
    if (!std::isfinite(num)) {
      os << "null";
    } else {
      std::ostringstream number;
      number.imbue(std::locale::classic());
      number << std::setprecision(std::numeric_limits<double>::max_digits10)
             << num;
      os << number.str();
    }
  } else if (isBool()) {
    os << (boo ? "true" : "false");
  } else if (isNull()) {
    os << "null";
  } else {
    WASM_UNREACHABLE("invalid JSON value type");
  }
}

} // namespace json
