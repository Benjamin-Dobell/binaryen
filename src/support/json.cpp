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

void Value::stringify(std::ostream& os, bool pretty, int indent) {
  auto doIndent = [&]() {
    for (int i = 0; i < indent; i++) {
      os << ' ';
    }
  };

  auto maybeNewline = [&]() {
    if (pretty) {
      os << '\n';
      doIndent();
    }
  };

  switch (type) {
    case String: {
      std::stringstream wtf16;
      [[maybe_unused]] bool valid =
        wasm::String::convertWTF8ToWTF16(wtf16, getIString().view());
      assert(valid);
      // TODO: Use wtf16.view() once we have C++20.
      wasm::String::printEscapedJSON(os, wtf16.str());
      return;
    }
    case Array: {
      os << '[';
      indent++;
      auto first = true;
      for (auto& item : getArray()) {
        if (first) {
          first = false;
        } else {
          os << ',';
        }
        maybeNewline();
        item->stringify(os, pretty, indent);
      }
      indent--;
      maybeNewline();
      os << ']';
      return;
    }
    case Object: {
      os << '{';
      indent++;
      auto first = true;
      for (auto& [key, value] : getObject()) {
        if (first) {
          first = false;
        } else {
          os << ',';
        }
        maybeNewline();
        Value name;
        name.setString(key);
        name.stringify(os, pretty, indent);
        os << ':';
        if (pretty) {
          os << ' ';
        }
        value->stringify(os, pretty, indent);
      }
      indent--;
      maybeNewline();
      os << '}';
      return;
    }
    case Number: {
      // Compiler metadata needs exact numbers, independent of caller stream flags.
      // JSON has no nonfinite numeric literals; serialize those as null.
      const auto value = getNumber();
      if (!std::isfinite(value)) {
        os << "null";
      } else {
        std::ostringstream number;
        number.imbue(std::locale::classic());
        number << std::setprecision(std::numeric_limits<double>::max_digits10)
               << value;
        os << number.str();
      }
      return;
    }
    case Null:
      os << "null";
      return;
    case Bool:
      os << (getBool() ? "true" : "false");
      return;
  };
}

} // namespace json
