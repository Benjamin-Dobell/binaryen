/*
 * Copyright 2026 WebAssembly Community Group participants
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

// A typed-IR implementation of the opt-in Breaka lazy-table ABI. Run after
// normal whole-module optimization. Table identities must not be merged after
// this pass emits their canonical-address metadata.

#include "ir/names.h"
#include "pass.h"
#include "support/json.h"
#include "wasm-builder.h"
#include "wasm-traversal.h"

#include <sstream>
#include <unordered_map>

namespace wasm {

namespace {

constexpr Index BitmapBits = 1 << 17;
constexpr Index MetadataVersion = 2;
constexpr const char* LazyMarker = "breaka.lazy-table";
constexpr const char* GuardedMarker = "breaka.guarded-table";
constexpr const char* RegisterImport = "__breaka_lazy_register";
constexpr const char* EnsureImport = "__breaka_lazy_ensure";
constexpr const char* FillImport = "__breaka_lazy_fill";

void require(bool condition, const std::string& message) {
  if (!condition) {
    Fatal() << "breaka-lazy-table: " << message;
  }
}

json::Value::Ref jsonNumber(Index value) {
  return json::Value::make(double(value));
}

struct OriginalCodeValidator
  : public PostWalker<OriginalCodeValidator,
                      UnifiedExpressionVisitor<OriginalCodeValidator>> {
  Name table;
  Index indirectSites = 0;

  explicit OriginalCodeValidator(Name table) : table(table) {}

  void visitExpression(Expression* curr) {
    require(!curr->is<TableGet>() && !curr->is<TableSet>() &&
              !curr->is<TableSize>() && !curr->is<TableGrow>() &&
              !curr->is<TableFill>() && !curr->is<TableCopy>() &&
              !curr->is<TableInit>() && !curr->is<ElemDrop>(),
            "original table operations are unsupported");
  }

  void visitCallIndirect(CallIndirect* curr) {
    require(curr->table == table, "indirect call uses a different table");
    require(curr->target->type == Type::i32 ||
              curr->target->type == Type::unreachable,
            "indirect call target must be i32");
    indirectSites++;
  }
};

struct InstrumentCalls : public PostWalker<InstrumentCalls> {
  Builder& builder;
  Name ensure;
  Index sites = 0;

  InstrumentCalls(Builder& builder, Name ensure)
    : builder(builder), ensure(ensure) {}

  void visitCallIndirect(CallIndirect* curr) {
    // Wasm evaluates call arguments before the target. Wrapping only the target
    // evaluates it once, after those arguments, without copying/reordering any
    // argument expression. The original isReturn flag remains untouched.
    curr->target = builder.makeCall(ensure, {curr->target}, Type::i32);
    curr->finalize();
    sites++;
  }
};

struct BreakaLazyTable : public Pass {
  bool addsEffects() override { return true; }

  Name
  importFunction(Module& wasm, const char* externalName, Signature signature) {
    for (auto& function : wasm.functions) {
      if (function->imported() && function->module == "env" &&
          function->base == externalName) {
        require(function->getSig() == signature,
                std::string("wrong import signature: ") + externalName);
        return function->name;
      }
    }
    auto function = std::make_unique<Function>();
    function->name = Names::getValidFunctionName(wasm, externalName);
    function->module = "env";
    function->base = externalName;
    function->type = Type(signature, NonNullable);
    return wasm.addFunction(std::move(function))->name;
  }

  Name mainEnsure(Module& wasm) {
    auto* exported = wasm.getExportOrNull(EnsureImport);
    require(exported && exported->kind == ExternalKind::Function,
            "main module requires the compiled __breaka_lazy_ensure export");
    auto* function = wasm.getFunction(*exported->getInternalName());
    require(!function->imported() &&
              function->getSig() ==
                Signature(Type({Type::i32, Type::i32}), Type::i32),
            "compiled ensure helper has the wrong signature or is imported");
    OriginalCodeValidator validator(wasm.tables[0]->name);
    validator.walkFunction(function);
    require(validator.indirectSites == 0,
            "compiled ensure helper must not make recursive indirect calls");
    function->noFullInline = true;
    function->noPartialInline = true;
    return function->name;
  }

  void run(Module* module) override {
    auto& wasm = *module;
    Builder builder(wasm);
    const auto role = getArgument("breaka-lazy-table-role",
                                  "expected role main, side, or guarded-side");
    const bool main = role == "main";
    const bool guarded = role == "guarded-side";
    require(main || guarded || role == "side", "unknown role: " + role);
    require(wasm.tables.size() == 1 && wasm.memories.size() == 1,
            "exactly one table and one memory are required");
    auto* table = wasm.tables[0].get();
    require(table->addressType == Type::i32 &&
              table->type == Type(HeapType::func, Nullable) &&
              !wasm.memories[0]->is64(),
            "requires wasm32 and a funcref table");
    require(wasm.dylinkSection && !wasm.dylinkSection->isLegacy,
            "requires dylink.0 metadata");
    require(wasm.dylinkSection->hasMemoryInfo, "requires dylink.0 memory info");
    for (auto& section : wasm.customSections) {
      require(section.name != LazyMarker && section.name != GuardedMarker,
              "module is already transformed");
      require(section.name != "linking" &&
                section.name.rfind("reloc.", 0) != 0 &&
                section.name.rfind(".debug_", 0) != 0 &&
                section.name != "sourceMappingURL",
              "relocatable objects and instruction-offset debug data are "
              "unsupported");
    }
    require(wasm.debugInfoFileNames.empty() &&
              wasm.debugInfoSymbolNames.empty(),
            "source-location debug data requires relocation support");
    require(wasm.elementSegments.size() == 1,
            "exactly one original active element segment is required");
    auto* elements = wasm.elementSegments[0].get();
    require(
      elements->table == table->name && elements->offset &&
        elements->type == Type(HeapType::func, Nullable),
      "expected one active function element segment on the original table");
    require(elements->data.size() <= UINT32_MAX,
            "element count exceeds the metadata ABI");
    const Index count = Index(elements->data.size());
    require(wasm.dylinkSection->tableSize == count,
            "dylink table allocation differs from original element count");
    std::vector<Name> identities;
    identities.reserve(count);
    for (auto* expression : elements->data) {
      auto* reference = expression->dynCast<RefFunc>();
      require(reference && wasm.getFunctionOrNull(reference->func),
              "element entries must be existing nonnull functions");
      identities.push_back(reference->func);
    }
    Name tableBase;
    Index fillSlot;
    if (main) {
      auto* offset = elements->offset->dynCast<Const>();
      require(!table->imported() && offset && offset->type == Type::i32 &&
                offset->value.geti32() == 1 && !table->hasMax() &&
                table->initial.addr == uint64_t(count) + 1 &&
                table->initial.addr < BitmapBits,
              "main requires eager entries at 1 ending at its growable table "
              "minimum");
      fillSlot = Index(table->initial.addr);
    } else {
      auto* offset = elements->offset->dynCast<GlobalGet>();
      require(offset, "side element offset must be env.__table_base");
      auto* global = wasm.getGlobalOrNull(offset->name);
      require(global && global->imported() && global->module == "env" &&
                global->base == "__table_base" && global->type == Type::i32 &&
                !global->mutable_,
              "side requires an immutable i32 env.__table_base offset");
      tableBase = global->name;
      fillSlot = count;
      require(guarded || count < BitmapBits,
              "lazy segment exceeds bitmap capacity");
    }

    // Validate the complete original code before changing any of its IR. The
    // regular Binaryen reader/validator owns instruction/type/index decoding.
    OriginalCodeValidator original(table->name);
    std::vector<Function*> callers;
    for (auto& function : wasm.functions) {
      if (!function->imported()) {
        original.walkFunction(function.get());
        callers.push_back(function.get());
      }
    }

    json::Value metadata;
    metadata.setObject();
    metadata["version"] = jsonNumber(MetadataVersion);
    metadata["role"] = json::Value::make(main ? "main" : "side");
    metadata["count"] = jsonNumber(main ? 0 : count);
    metadata["guardedSites"] = jsonNumber(original.indirectSites);
    Name bitmap;
    Name ensure;
    Name compiledEnsure;
    if (!guarded) {
      bitmap = Names::getValidGlobalName(wasm, "__breaka_lazy_bitmap");
      wasm.addGlobal(builder.makeGlobal(
        bitmap, Type::i32, builder.makeConst(int32_t(0)), Builder::Mutable));
      compiledEnsure =
        main
          ? mainEnsure(wasm)
          : importFunction(wasm,
                           EnsureImport,
                           Signature(Type({Type::i32, Type::i32}), Type::i32));
      const auto registerFunction = importFunction(
        wasm,
        RegisterImport,
        Signature(Type({Type::i32, Type::i32, Type::i32}), Type::i32));
      const auto thunkName =
        Names::getValidFunctionName(wasm, "__breaka_lazy_table_thunk");
      const auto thunkType =
        HeapType(Signature(Type({Type::i32, Type::i32}), Type::none));
      std::vector<Expression*> thunkBody;
      thunkBody.push_back(builder.makeIf(
        builder.makeBinary(EqInt32,
                           builder.makeLocalGet(0, Type::i32),
                           builder.makeConst(int32_t(-1))),
        builder.makeBlock(
          {builder.makeGlobalSet(bitmap, builder.makeLocalGet(1, Type::i32)),
           builder.makeReturn()})));
      if (!main) {
        thunkBody.push_back(builder.makeTableInit(
          elements->name,
          builder.makeLocalGet(0, Type::i32),
          builder.makeBinary(SubInt32,
                             builder.makeLocalGet(0, Type::i32),
                             builder.makeGlobalGet(tableBase, Type::i32)),
          builder.makeConst(int32_t(1)),
          table->name));
      }
      auto thunk =
        builder.makeFunction(thunkName,
                             {{"index", Type::i32}, {"bitmap", Type::i32}},
                             thunkType,
                             {},
                             builder.makeBlock(thunkBody));
      thunk->noFullInline = true;
      thunk->noPartialInline = true;
      wasm.addFunction(std::move(thunk));
      elements->data.push_back(builder.makeRefFunc(thunkName));

      auto exports = json::Value::Ref(new json::Value());
      exports->setObject();
      auto aliases = json::Value::Ref(new json::Value());
      aliases->setArray();
      if (!main) {
        std::unordered_map<Name, Index> last;
        for (Index position = 0; position < count; position++) {
          last[identities[position]] = position;
        }
        for (Index position = 0; position < count; position++) {
          if (last[identities[position]] != position) {
            aliases->push_back(jsonNumber(position));
            aliases->push_back(jsonNumber(last[identities[position]]));
          }
        }
        for (auto& exported : wasm.exports) {
          if (exported->kind == ExternalKind::Function) {
            auto found = last.find(*exported->getInternalName());
            if (found != last.end()) {
              (*exports)[exported->name] = jsonNumber(found->second);
            }
          }
        }
        // A passive segment has neither a target table nor an offset. Its
        // contents retain all original positions and the single added thunk.
        elements->table = Name();
        elements->offset = nullptr;
        wasm.features.setBulkMemory();
        wasm.features.setBulkMemoryOpt();
      } else {
        table->initial = table->initial.addr + 1;
      }
      wasm.dylinkSection->tableSize++;
      metadata["bitmapBits"] = jsonNumber(BitmapBits);
      metadata["fillSlot"] = jsonNumber(fillSlot);
      metadata["exports"] = exports;
      metadata["aliases"] = aliases;

      const auto base = [&]() -> Expression* {
        return main ? static_cast<Expression*>(builder.makeConst(int32_t(0)))
                    : builder.makeGlobalGet(tableBase, Type::i32);
      };
      const auto slot = [&]() -> Expression* {
        return main ? static_cast<Expression*>(
                        builder.makeConst(int32_t(fillSlot)))
                    : builder.makeBinary(
                        AddInt32, base(), builder.makeConst(int32_t(fillSlot)));
      };
      std::vector<Expression*> startBody;
      if (!main) {
        startBody.push_back(
          builder.makeTableInit(elements->name,
                                slot(),
                                builder.makeConst(int32_t(fillSlot)),
                                builder.makeConst(int32_t(1)),
                                table->name));
      }
      startBody.push_back(builder.makeGlobalSet(
        bitmap,
        builder.makeCall(
          registerFunction,
          {base(), builder.makeConst(int32_t(main ? 0 : count)), slot()},
          Type::i32)));
      if (wasm.start.is()) {
        startBody.push_back(builder.makeCall(wasm.start, {}, Type::none));
      }
      const auto startName =
        Names::getValidFunctionName(wasm, "__breaka_lazy_table_start");
      wasm.addFunction(builder.makeFunction(startName,
                                            {},
                                            Signature(Type::none, Type::none),
                                            {},
                                            builder.makeBlock(startBody)));
      wasm.start = startName;
    }

    // A compiled C++ function may spill parameters through __stack_pointer even
    // before its first branch. Module starts precede pthread stack/TLS setup,
    // so the bitmap-zero path must remain entirely typed IR and JavaScript.
    // There is one wrapper per module, not an inline copy at each call site.
    const auto fill =
      importFunction(wasm, FillImport, Signature(Type::i32, Type::none));
    ensure = Names::getValidFunctionName(wasm, "__breaka_lazy_ensure_target");
    std::vector<Expression*> ensureBody;
    if (!guarded) {
      ensureBody.push_back(
        builder.makeIf(builder.makeGlobalGet(bitmap, Type::i32),
                       builder.makeReturn(builder.makeCall(
                         compiledEnsure,
                         {builder.makeLocalGet(0, Type::i32),
                          builder.makeGlobalGet(bitmap, Type::i32)},
                         Type::i32))));
    }
    ensureBody.push_back(builder.makeIf(
      builder.makeRefIsNull(builder.makeTableGet(
        table->name, builder.makeLocalGet(0, Type::i32), table->type)),
      builder.makeCall(
        fill, {builder.makeLocalGet(0, Type::i32)}, Type::none)));
    ensureBody.push_back(builder.makeLocalGet(0, Type::i32));
    auto ensureFunction = builder.makeFunction(ensure,
                                               {{"index", Type::i32}},
                                               Signature(Type::i32, Type::i32),
                                               {},
                                               builder.makeBlock(ensureBody));
    ensureFunction->noFullInline = true;
    ensureFunction->noPartialInline = true;
    wasm.addFunction(std::move(ensureFunction));
    wasm.features.setReferenceTypes();

    InstrumentCalls instrument(builder, ensure);
    for (auto* function : callers) {
      // The compiled helper was checked above to contain no indirect calls.
      instrument.walkFunction(function);
    }
    require(instrument.sites == original.indirectSites,
            "indirect call coverage changed while transforming");
    std::ostringstream encoded;
    metadata.stringify(encoded);
    const auto text = encoded.str();
    wasm.customSections.push_back(
      {guarded ? GuardedMarker : LazyMarker, {text.begin(), text.end()}});
  }
};

} // anonymous namespace

Pass* createBreakaLazyTablePass() { return new BreakaLazyTable(); }

} // namespace wasm
