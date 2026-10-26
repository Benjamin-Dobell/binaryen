/*
 * Copyright 2026 WebAssembly Community Group participants
 * Licensed under the Apache License, Version 2.0.
 */

#include "pass.h"
#include "support/json.h"
#include "wasm-binary.h"
#include "wasm-builder.h"
#include "wasm-validator.h"
#include "gtest/gtest.h"

using namespace wasm;

namespace {

void makeFixture(Module& module, bool main = false, bool exportHelper = true) {
  Builder builder(module);
  auto memory = builder.makeMemory("memory", 1);
  if (!main) {
    memory->module = "env";
    memory->base = "memory";
  }
  module.addMemory(std::move(memory));
  auto table =
    builder.makeTable("table", Type(HeapType::func, Nullable), main ? 4 : 0);
  if (!main) {
    table->module = "env";
    table->base = "__indirect_function_table";
    auto base =
      builder.makeGlobal("table_base", Type::i32, nullptr, Builder::Immutable);
    base->module = "env";
    base->base = "__table_base";
    module.addGlobal(std::move(base));
  }
  module.addTable(std::move(table));
  const auto type = HeapType(Signature(Type::i32, Type::i32));
  module.addFunction(builder.makeFunction("target",
                                          {{"value", Type::i32}},
                                          type,
                                          {},
                                          builder.makeLocalGet(0, Type::i32)));
  module.addFunction(builder.makeFunction("other",
                                          {{"value", Type::i32}},
                                          type,
                                          {},
                                          builder.makeLocalGet(0, Type::i32)));
  auto segment = builder.makeElementSegment(
    "entries",
    "table",
    main ? static_cast<Expression*>(builder.makeConst(1))
         : builder.makeGlobalGet("table_base", Type::i32));
  for (Name name : {Name("target"), Name("other"), Name("target")}) {
    segment->data.push_back(builder.makeRefFunc(name));
  }
  module.addElementSegment(std::move(segment));
  module.addExport(builder.makeExport(
    "visible_target", Name("target"), ExternalKind::Function));
  for (bool tail : {false, true}) {
    auto call =
      builder.makeCallIndirect("table",
                               builder.makeConst(1),
                               std::vector<Expression*>{builder.makeConst(7)},
                               type,
                               tail);
    module.addFunction(builder.makeFunction(tail ? "tail_caller" : "caller",
                                            {},
                                            Signature(Type::none, Type::i32),
                                            {},
                                            call));
  }
  module.features.setTailCall();
  module.addFunction(builder.makeFunction(
    "old_start", {}, Signature(Type::none, Type::none), {}, builder.makeNop()));
  module.start = "old_start";
  if (main) {
    module.addFunction(
      builder.makeFunction("compiled_ensure",
                           {{"index", Type::i32}, {"bitmap", Type::i32}},
                           Signature(Type({Type::i32, Type::i32}), Type::i32),
                           {},
                           builder.makeLocalGet(0, Type::i32)));
    if (exportHelper) {
      module.addExport(builder.makeExport("__breaka_lazy_ensure",
                                          Name("compiled_ensure"),
                                          ExternalKind::Function));
    }
  }
  module.dylinkSection = std::make_unique<DylinkSection>();
  module.dylinkSection->memorySize = 16;
  module.dylinkSection->memoryAlignment = 4;
  module.dylinkSection->tableSize = 3;
  module.dylinkSection->tableAlignment = 0;
}

void transform(Module& module, const char* role) {
  PassRunner runner(&module);
  runner.options.arguments["breaka-lazy-table-role"] = role;
  runner.add("breaka-lazy-table");
  runner.run();
}

std::string markerText(Module& module, const char* name) {
  for (auto& custom : module.customSections) {
    if (custom.name == name) {
      return {custom.data.begin(), custom.data.end()};
    }
  }
  return "";
}

void checkValid(Module& module) {
  EXPECT_TRUE(WasmValidator{}.validate(
    module, WasmValidator::Globally | WasmValidator::Quiet));
}

} // anonymous namespace

class BreakaLazyTableTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Binaryen initializes a worker pool. Re-exec death-test children instead
    // of inheriting its locked synchronization state without worker threads.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  }
};

TEST_F(BreakaLazyTableTest, SideCanonicalMetadataAndOneSharedWrapper) {
  Module module;
  makeFixture(module);
  auto* caller = module.getFunction("caller")->body->cast<CallIndirect>();
  auto* originalOperand = caller->operands[0];
  auto* originalTarget = caller->target;
  auto originalExport = *module.getExport("visible_target")->getInternalName();
  transform(module, "side");
  checkValid(module);
  EXPECT_EQ(*module.getExport("visible_target")->getInternalName(),
            originalExport);
  EXPECT_EQ(module.dylinkSection->tableSize, 4u);
  auto* segment = module.elementSegments[0].get();
  EXPECT_FALSE(segment->offset);
  EXPECT_FALSE(segment->table.is());
  ASSERT_EQ(segment->data.size(), 4u);
  EXPECT_EQ(segment->data[0]->cast<RefFunc>()->func, Name("target"));
  EXPECT_EQ(segment->data[2]->cast<RefFunc>()->func, Name("target"));
  auto* normal = caller->target->cast<Call>();
  auto* tail = module.getFunction("tail_caller")->body->cast<CallIndirect>();
  EXPECT_EQ(normal->target, tail->target->cast<Call>()->target);
  EXPECT_TRUE(tail->isReturn);
  EXPECT_EQ(caller->operands[0], originalOperand);
  EXPECT_EQ(normal->operands[0], originalTarget);
  EXPECT_EQ(normal->operands.size(), 1u);
  auto* shared = module.getFunction(normal->target);
  EXPECT_TRUE(shared->noFullInline);
  EXPECT_TRUE(shared->noPartialInline);
  auto* body = shared->body->cast<Block>();
  ASSERT_EQ(body->list.size(), 3u);
  EXPECT_TRUE(body->list[0]->is<If>());
  // The zero-bitmap bootstrap branch is table.get + JavaScript, not C++.
  EXPECT_TRUE(body->list[1]->cast<If>()->condition->is<RefIsNull>());
  std::string text = markerText(module, "breaka.lazy-table");
  json::Value value;
  value.parse(text.data(), json::Value::ASCII);
  EXPECT_EQ(value["version"]->getInteger(), 2);
  EXPECT_EQ(value["count"]->getInteger(), 3);
  EXPECT_EQ(value["fillSlot"]->getInteger(), 3);
  EXPECT_EQ(value["guardedSites"]->getInteger(), 2);
  EXPECT_EQ(value["aliases"]->size(), 2u);
  EXPECT_EQ(value["aliases"][0]->getInteger(), 0);
  EXPECT_EQ(value["aliases"][1]->getInteger(), 2);
  EXPECT_EQ(value["exports"]["visible_target"]->getInteger(), 2);
  auto* start = module.getFunction(module.start)->body->cast<Block>();
  EXPECT_TRUE(start->list[0]->is<TableInit>());
  EXPECT_EQ(start->list.back()->cast<Call>()->target, Name("old_start"));
}

TEST_F(BreakaLazyTableTest, MainKeepsEagerElementsAndUsesCompiledHelper) {
  Module module;
  makeFixture(module, true);
  transform(module, "main");
  checkValid(module);
  EXPECT_EQ(module.tables[0]->initial.addr, 5u);
  EXPECT_EQ(module.dylinkSection->tableSize, 4u);
  EXPECT_EQ(module.elementSegments[0]->offset->cast<Const>()->value.geti32(),
            1);
  auto* caller = module.getFunction("caller")->body->cast<CallIndirect>();
  auto* shared = module.getFunction(caller->target->cast<Call>()->target);
  auto* active = shared->body->cast<Block>()->list[0]->cast<If>();
  auto* compiled = active->ifTrue->cast<Return>()->value->cast<Call>();
  EXPECT_EQ(compiled->target, Name("compiled_ensure"));
  EXPECT_EQ(compiled->operands.size(), 2u);
  auto text = markerText(module, "breaka.lazy-table");
  json::Value value;
  value.parse(text.data(), json::Value::ASCII);
  EXPECT_EQ(value["count"]->getInteger(), 0);
  EXPECT_EQ(value["fillSlot"]->getInteger(), 4);
  EXPECT_EQ(value["aliases"]->size(), 0u);
}

TEST_F(BreakaLazyTableTest, GuardedSideRetainsAllocationAndStart) {
  Module module;
  makeFixture(module);
  transform(module, "guarded-side");
  checkValid(module);
  EXPECT_EQ(module.start, Name("old_start"));
  EXPECT_EQ(module.dylinkSection->tableSize, 3u);
  EXPECT_TRUE(module.elementSegments[0]->offset);
  EXPECT_EQ(module.elementSegments[0]->data.size(), 3u);
  EXPECT_EQ(module.globals.size(), 1u);
  auto* caller = module.getFunction("caller")->body->cast<CallIndirect>();
  auto* shared = module.getFunction(caller->target->cast<Call>()->target);
  EXPECT_EQ(shared->body->cast<Block>()->list.size(), 2u);
  EXPECT_FALSE(markerText(module, "breaka.guarded-table").empty());
}

TEST_F(BreakaLazyTableTest, EmptySideKeepsItsSingleBootstrapEntry) {
  Module module;
  makeFixture(module);
  module.elementSegments[0]->data.clear();
  module.dylinkSection->tableSize = 0;
  transform(module, "side");
  checkValid(module);
  EXPECT_EQ(module.elementSegments[0]->data.size(), 1u);
  EXPECT_EQ(module.dylinkSection->tableSize, 1u);
  auto text = markerText(module, "breaka.lazy-table");
  json::Value value;
  value.parse(text.data(), json::Value::ASCII);
  EXPECT_EQ(value["count"]->getInteger(), 0);
  EXPECT_EQ(value["fillSlot"]->getInteger(), 0);
  EXPECT_EQ(value["aliases"]->size(), 0u);
}

TEST_F(BreakaLazyTableTest, MainRequiresTheNormalCompiledHelper) {
  EXPECT_DEATH(
    {
      Module module;
      makeFixture(module, true, false);
      transform(module, "main");
    },
    "compiled __breaka_lazy_ensure export");
}

TEST_F(BreakaLazyTableTest, RejectsSecondTransform) {
  EXPECT_DEATH(
    {
      Module module;
      makeFixture(module);
      transform(module, "side");
      transform(module, "side");
    },
    "already transformed");
}

TEST_F(BreakaLazyTableTest, RejectsOriginalTableMutation) {
  EXPECT_DEATH(
    {
      Module module;
      makeFixture(module);
      Builder builder(module);
      module.getFunction("old_start")->body =
        builder.makeDrop(builder.makeTableGet(
          "table", builder.makeConst(0), Type(HeapType::func, Nullable)));
      transform(module, "side");
    },
    "original table operations");
}

TEST_F(BreakaLazyTableTest, RejectsDuplicateDylinkInNormalReader) {
  Module module;
  makeFixture(module);
  module.customSections.push_back({"dylink.0", {}});
  BufferWithRandomAccess buffer;
  WasmBinaryWriter writer(&module, buffer, PassOptions{});
  writer.write();
  Module parsed;
  std::vector<char> bytes(buffer.begin(), buffer.end());
  WasmBinaryReader reader(parsed, FeatureSet::All, bytes);
  EXPECT_ANY_THROW(reader.read());
}

TEST_F(BreakaLazyTableTest, RejectsDuplicateDylinkMemoryInfo) {
  for (bool unknownBeforeDuplicate : {false, true}) {
    Module module;
    makeFixture(module);
    BufferWithRandomAccess tail;
    if (unknownBeforeDuplicate) {
      constexpr uint8_t UnknownDylinkSubsection = 128;
      tail << int8_t(UnknownDylinkSubsection) << U32LEB(0);
    }
    BufferWithRandomAccess info;
    info << U32LEB(16) << U32LEB(4) << U32LEB(3) << U32LEB(0);
    tail << int8_t(BinaryConsts::CustomSections::Subsection::DylinkMemInfo)
         << U32LEB(info.size());
    tail.insert(tail.end(), info.begin(), info.end());
    module.dylinkSection->tail = {tail.begin(), tail.end()};
    BufferWithRandomAccess buffer;
    WasmBinaryWriter writer(&module, buffer, PassOptions{});
    writer.write();
    Module parsed;
    std::vector<char> bytes(buffer.begin(), buffer.end());
    WasmBinaryReader reader(parsed, FeatureSet::All, bytes);
    EXPECT_ANY_THROW(reader.read());
  }
}

TEST_F(BreakaLazyTableTest, PreservesUnknownDylinkSubsections) {
  Module module;
  makeFixture(module);
  BufferWithRandomAccess tail;
  constexpr uint8_t UnknownDylinkSubsection = 128;
  tail << int8_t(UnknownDylinkSubsection) << U32LEB(1) << int8_t(42);
  module.dylinkSection->tail = {tail.begin(), tail.end()};
  BufferWithRandomAccess buffer;
  WasmBinaryWriter writer(&module, buffer, PassOptions{});
  writer.write();
  Module parsed;
  std::vector<char> bytes(buffer.begin(), buffer.end());
  WasmBinaryReader reader(parsed, FeatureSet::All, bytes);
  reader.read();
  ASSERT_TRUE(parsed.dylinkSection);
  EXPECT_TRUE(parsed.dylinkSection->hasMemoryInfo);
  EXPECT_EQ(parsed.dylinkSection->tail, module.dylinkSection->tail);
}

TEST_F(BreakaLazyTableTest, RejectsMissingDylinkMemoryInfo) {
  EXPECT_DEATH(
    {
      Module module;
      makeFixture(module);
      module.dylinkSection = nullptr;
      module.customSections.push_back({"dylink.0", {}});
      BufferWithRandomAccess buffer;
      WasmBinaryWriter writer(&module, buffer, PassOptions{});
      writer.write();
      Module parsed;
      std::vector<char> bytes(buffer.begin(), buffer.end());
      WasmBinaryReader reader(parsed, FeatureSet::All, bytes);
      reader.read();
      transform(parsed, "side");
    },
    "requires dylink.0 memory info");
}
