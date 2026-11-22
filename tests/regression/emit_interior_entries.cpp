// Synthetic executable PPC bytes pass through the real config loader, writer,
// decoder, and instruction builders. The companion test runs their C++ output.
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/codegen/test_support.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace rex::codegen;
namespace fs = std::filesystem;
constexpr uint32_t kBase = 0x82000000;
constexpr uint32_t kEntry = kBase + 0x18;
constexpr uint32_t kExtraOwner = kBase + 0x40;
constexpr uint32_t kExtraEntry = kBase + 0x48;
constexpr uint32_t kSwitch = kBase + 0x90;
constexpr uint32_t kSwitchBctr = kBase + 0xAC;

// How the jump table of sub_82000090 reaches codegen: not at all, as one of the
// function's analysed jump tables, or as a config switch table.
enum class SwitchTable { None, Analysis, Config };

// The table at +0xB8: case 0 tail-calls the body= entry, case 1 a plain
// function, case 2 a local label.
JumpTable EntrySwitch(uint32_t case0 = kEntry) {
  return JumpTable{kSwitchBctr, kBase + 0xB8, 3, {case0, kBase + 0x88, kBase + 0xB0}};
}

void Check(bool result, const char* message) {
  if (!result) throw std::runtime_error(message);
}

std::vector<uint8_t> Bytes(bool externalHole = false, bool switchTable = false) {
  std::vector<uint8_t> bytes(switchTable ? 0xC4 : 0x90);
  auto put = [&](unsigned offset, uint32_t instruction) {
    for (unsigned i = 0; i < 4; ++i)
      bytes[offset + i] = static_cast<uint8_t>(instruction >> (24 - i * 8));
  };
  // A real frame, conditional backward branch, live CTR loop, and owner-only
  // call metadata. The interior entry skips all four prologue instructions.
  put(0x00, 0x3821FFF0);  // addi r1,r1,-16
  put(0x04, 0x38600000);  // li r3,0
  put(0x08, 0x38800003);  // li r4,3
  put(0x0C, 0x7C8903A6);  // mtctr r4
  put(0x10, 0x38630001);  // addi r3,r3,1
  put(0x14, 0x4800006D);  // bl helper at +0x80
  put(0x18, 0x2C030001);  // cmpwi r3,1
  put(0x1C, 0x4182FFF4);  // beq +0x10
  put(0x20, 0x4200FFF0);  // bdnz +0x10
  put(0x24, 0x38210010);  // addi r1,r1,16
  put(0x28, 0x4E800020);  // blr
  put(0x30, 0x4E800020);  // standalone hook target for registration tests
  if (externalHole) {
    put(0x30, 0x38210010);  // external tail callee restores existing frame
    put(0x34, 0x38E70001);  // addi r7,r7,1
    put(0x38, 0x4E800020);  // blr
  }
  // Normal execution skips the interior-only path. Discovery from the
  // interior entry found a second block and call absent from the owner CFG.
  put(0x40, 0x3821FFF0);  // addi r1,r1,-16
  put(0x44, 0x4800000C);  // b +0x50
  put(0x48, 0x60000000);  // nop (actual entry instruction in owner block)
  put(0x4C, 0x48000014);  // b +0x60 (entry-only discovered instruction)
  put(0x50, 0x38210010);  // addi r1,r1,16
  put(0x54, 0x4E800020);  // blr
  put(0x60, 0x48000029);  // bl helper at +0x88
  put(0x64, 0x48000000 | (static_cast<uint32_t>(-0x14) & 0x03FFFFFC)); // b +0x50
  put(0x80, 0x38A50001);  // addi r5,r5,1
  put(0x84, 0x4E800020);
  put(0x88, 0x38C60001);  // addi r6,r6,1
  put(0x8C, 0x4E800020);
  if (switchTable) {
    // A bounded absolute switch on r3 through the table at +0xB8 (EntrySwitch).
    put(0x90, 0x3D608200);  // lis r11,0x8200
    put(0x94, 0x396B00B8);  // addi r11,r11,0xB8 (table below)
    put(0x98, 0x28030002);  // cmplwi r3,2
    put(0x9C, 0x41810014);  // bgt +0xB0 (default)
    put(0xA0, 0x546C103A);  // slwi r12,r3,2
    put(0xA4, 0x7C0B602E);  // lwzx r0,r11,r12
    put(0xA8, 0x7C0903A6);  // mtctr r0
    put(0xAC, 0x4E800420);  // bctr
    put(0xB0, 0x4E800020);  // blr
    put(0xB8, kEntry);      // case 0: interior (body=) entry
    put(0xBC, kBase + 0x88);  // case 1: plain function
    put(0xC0, kBase + 0xB0);  // case 2: local label
  }
  return bytes;
}

CodegenContext Context(const fs::path& root, RecompilerConfig config,
                       bool missingEntry = false, uint32_t pendingTarget = 0,
                       bool externalHole = false, bool localizationProbe = false,
                       SwitchTable switchTable = SwitchTable::None) {
  auto bytes = Bytes(externalHole, switchTable != SwitchTable::None);
  if (switchTable == SwitchTable::Config)
    config.switchTables.try_emplace(kSwitchBctr, EntrySwitch());
  if (localizationProbe) {
    // lwarx r0,0,r31 replaces the nop shared by the extra owner and entry.
    constexpr uint32_t probe = 0x7C00F828;
    for (unsigned i = 0; i < 4; ++i)
      bytes[0x48 + i] = static_cast<uint8_t>(probe >> (24 - i * 8));
  }
  if (pendingTarget) {
    uint32_t instruction = 0x41820000 | ((pendingTarget - (kBase + 0x1C)) & 0xFFFC);
    for (unsigned i = 0; i < 4; ++i)
      bytes[0x1C + i] = static_cast<uint8_t>(instruction >> (24 - i * 8));
  }
  TestModule module;
  module.Load(kBase, bytes.data(), bytes.size());
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  ctx.setConfigDir(root);
  ctx.analysisState().format = "xex";
  ctx.analysisState().loadAddress = kBase;
  ctx.analysisState().entryPoint = kBase + 0x80;
  ctx.analysisState().imageSize = static_cast<uint32_t>(bytes.size());
  auto add = [&](uint32_t address, uint32_t size, std::vector<Block> blocks) {
    auto* node = ctx.graph.addFunction(address, size, FunctionAuthority::CONFIG, true);
    Check(node != nullptr, "addFunction failed");
    node->discover(std::move(blocks), {}, {});
    return node;
  };
  auto* helper = add(kBase + 0x80, 8, {{kBase + 0x80, 8}});
  ctx.graph.setFunctionName(kBase + 0x80, "xstart");
  auto* extraHelper = add(kBase + 0x88, 8, {{kBase + 0x88, 8}});
  auto* owner = add(kBase, externalHole ? 0x40 : 0x2C, missingEntry
      ? std::vector<Block>{{kBase, 0x18}, {kBase + 0x1C, 0x10}}
      : std::vector<Block>{{kBase, 0x2C}});
  auto* entry = add(kEntry, 0x14, {{kEntry, 0x14}});
  auto* extraOwner = add(kExtraOwner, 0x18,
      {{kExtraOwner, 0xC}, {kBase + 0x50, 8}});
  auto* extraEntry = add(kExtraEntry, 0x20,
      {{kExtraEntry, 8}, {kBase + 0x50, 8}, {kBase + 0x60, 8}});
  ctx.graph.addCallToFunction(kBase, kBase + 0x14, CallTarget::function(helper));
  ctx.graph.addLabelToFunction(kBase, kBase + 0x10);
  ctx.graph.addCallToFunction(kEntry, kBase + 0x14, CallTarget::function(extraHelper));
  ctx.graph.addCallToFunction(kExtraEntry, kBase + 0x60, CallTarget::function(extraHelper));
  // A conflicting classification from isolated-entry analysis must not
  // override the actual owner's backward-branch interpretation.
  ctx.graph.addTailCallToFunction(kEntry, kBase + 0x1C, CallTarget::function(owner));
  if (pendingTarget)
    ctx.graph.addUnresolvedJumpToFunction(kEntry, kBase + 0x1C, pendingTarget, false, true);
  for (auto* node : {helper, extraHelper, owner, entry, extraOwner, extraEntry}) {
    if (node != entry || !pendingTarget) node->seal();
  }
  if (switchTable != SwitchTable::None) {
    auto* dispatcher = add(kSwitch, 0x24, {{kSwitch, 0x24}});
    ctx.graph.addLabelToFunction(kSwitch, kBase + 0xB0);
    if (switchTable == SwitchTable::Analysis)
      ctx.graph.addJumpTableToFunction(kSwitch, EntrySwitch());
    dispatcher->seal();
  }
  if (pendingTarget)
    Check(entry->isDiscovered() && !entry->isSealed() && entry->unresolvedJumps().size() == 1,
          "fixture must retain a genuinely unsealed deferred alias branch");
  return ctx;
}

// Concatenated text of a project's emitted recompiled translation units.
std::string ReadSources(const fs::path& dir, const std::string& project) {
  std::string all;
  for (const auto& file : fs::recursive_directory_iterator(dir)) {
    auto name = file.path().filename().string();
    if (!file.is_regular_file() || name.rfind(project + "_recomp.", 0) != 0 ||
        file.path().extension() != ".cpp")
      continue;
    std::ifstream in(file.path(), std::ios::binary);
    all.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    all += '\n';
  }
  Check(!all.empty(), "no recompiled translation units emitted");
  return all;
}

// One emitted function, from its definition to the next one.
std::string FunctionText(const std::string& sources, const std::string& name) {
  auto start = sources.find("DEFINE_REX_FUNC(" + name + ") {");
  Check(start != std::string::npos, "expected function missing from the output");
  auto end = sources.find("DEFINE_REX_FUNC(", start + 1);
  return sources.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Names of the register locals a function declares (ctr, xer, reserved, crN,
// rN, fN, vN); scratch locals such as temp, vTemp, env and ea are ignored.
std::set<std::string> RegisterLocals(const std::string& text) {
  std::set<std::string> names;
  size_t pos = 0;
  while (pos < text.size()) {
    auto eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("\tPPC", 0) != 0 || line.size() < 4 ||
        line.compare(line.size() - 3, 3, "{};") != 0)
      continue;
    auto space = line.rfind(' ');
    if (space == std::string::npos) continue;
    auto name = line.substr(space + 1, line.size() - space - 4);
    if (name != "temp" && name != "vTemp" && name != "env") names.insert(name);
  }
  return names;
}

// A plain function without body= entries that touches every localizable
// register class: cmpwi r3,1 (cr0, xer.so); mtctr r4; addic r5,r5,1 (xer.ca);
// lwarx r6,0,r4 (reserved); li r0,1; li r31,1; blr.
CodegenContext ControlContext(const fs::path& root, const std::vector<bool RecompilerConfig::*>& flags) {
  constexpr std::array<uint32_t, 7> code{0x2C030001, 0x7C8903A6, 0x30A50001, 0x7CC02028,
                                         0x38000001, 0x3BE00001, 0x4E800020};
  std::vector<uint8_t> bytes;
  for (auto instruction : code)
    for (unsigned i = 0; i < 4; ++i)
      bytes.push_back(static_cast<uint8_t>(instruction >> (24 - i * 8)));
  TestModule module;
  module.Load(kBase, bytes.data(), bytes.size());
  RecompilerConfig config;
  config.projectName = "control";
  config.outDirectoryPath = "generated";
  for (auto flag : flags) config.*flag = true;
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  ctx.setConfigDir(root);
  ctx.analysisState().format = "xex";
  ctx.analysisState().loadAddress = kBase;
  ctx.analysisState().entryPoint = kBase;
  ctx.analysisState().imageSize = static_cast<uint32_t>(bytes.size());
  auto* node = ctx.graph.addFunction(kBase, static_cast<uint32_t>(bytes.size()),
                                     FunctionAuthority::CONFIG, true);
  Check(node != nullptr, "control node rejected");
  node->discover({{kBase, static_cast<uint32_t>(bytes.size())}}, {}, {});
  node->seal();
  return ctx;
}

// skip_lr keeps LR in the bodies that use it as data and drops it elsewhere.
// sub_82000000: mtlr r11; blrl; blr (calls through LR). sub_82000010: bl $+4;
// mflr r3; blr (PIC: reads its own address from LR). xstart (+0x20): mflr r12;
// bl sub_82000000; mtlr r12; blr, which only saves and restores the return
// address around a call.
CodegenContext LrContext(const fs::path& root, bool skipLr) {
  constexpr std::array<uint32_t, 12> code{
      0x7D6803A6, 0x4E800021, 0x4E800020, 0x60000000,   // +0x00
      0x48000005, 0x7C6802A6, 0x4E800020, 0x60000000,   // +0x10
      0x7D8802A6, 0x4BFFFFDD, 0x7D8803A6, 0x4E800020};  // +0x20
  std::vector<uint8_t> bytes;
  for (auto instruction : code)
    for (unsigned i = 0; i < 4; ++i)
      bytes.push_back(static_cast<uint8_t>(instruction >> (24 - i * 8)));
  TestModule module;
  module.Load(kBase, bytes.data(), bytes.size());
  RecompilerConfig config;
  config.projectName = "lr";
  config.outDirectoryPath = "generated";
  config.skipLr = skipLr;
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  ctx.setConfigDir(root);
  ctx.analysisState().format = "xex";
  ctx.analysisState().loadAddress = kBase;
  ctx.analysisState().entryPoint = kBase + 0x20;
  ctx.analysisState().imageSize = static_cast<uint32_t>(bytes.size());
  auto add = [&](uint32_t address, uint32_t size) {
    auto* node = ctx.graph.addFunction(address, size, FunctionAuthority::CONFIG, true);
    Check(node != nullptr, "skip_lr fixture node rejected");
    node->discover({{address, size}}, {}, {});
    return node;
  };
  auto* blrl = add(kBase, 0xC);
  auto* pic = add(kBase + 0x10, 0xC);
  auto* saver = add(kBase + 0x20, 0x10);
  ctx.graph.addLabelToFunction(kBase + 0x10, kBase + 0x14);
  ctx.graph.addCallToFunction(kBase + 0x20, kBase + 0x24, CallTarget::function(blrl));
  for (auto* node : {blrl, pic, saver}) node->seal();
  return ctx;
}

RecompilerConfig Config(const fs::path& root) {
  fs::create_directories(root);
  auto path = root / "fixture.toml";
  std::ofstream(path) << "project_name = 'interior'\nfile_path = 'synthetic.bin'\n"
      "out_directory_path = 'generated'\n"
      "[functions]\n0x82000018 = { body = 0x82000000 }\n"
      "0x82000048 = { body = 0x82000040 }\n";
  RecompilerConfig config;
  Check(config.Load(path.string()), "valid body config rejected");
  Check(config.functions.at(kEntry).bodyOwner == kBase, "body owner lost by parser");
  Check(config.outDirectoryPath == "generated", "output path lost by parser");
  return config;
}

int main(int argc, char** argv) try {
  Check(argc == 2, "expected output directory");
  fs::path root = fs::absolute(argv[1]);
  auto config = Config(root);
  auto ctx = Context(root, config);
  CodegenWriter writer(ctx);
  Check(writer.write(false), "valid body emission failed");

  auto deferred = Context(root / "deferred", config, false, kBase + 0x10);
  CodegenWriter deferredWriter(deferred);
  Check(deferredWriter.write(false), "mapped unsealed interior branch rejected");

  // Exercise registration through the same actual writer as interior bodies.
  // The below-code non-import is a rexcrt mapping so no out-of-image body is
  // emitted. Its declaration must not become a registration.
  auto registration = Context(root / "registration", config);
  Check(registration.graph.addImportFunction(kBase - 8, "__imp__registration_import") != nullptr,
        "registration import rejected");
  Check(registration.graph.addFunction(kBase - 4, 4, FunctionAuthority::CONFIG, true) != nullptr,
        "below-code fixture node rejected");
  registration.Config().rexcrtFunctions["registration_suppressed"] = kBase - 4;
  auto* hooked = registration.graph.addFunction(kBase + 0x30, 4,
      FunctionAuthority::CONFIG, "registration_hooked", true);
  Check(hooked != nullptr, "hook fixture node rejected");
  hooked->discover({{kBase + 0x30, 4}}, {}, {});
  hooked->seal();
  CodegenWriter registrationWriter(registration);
  Check(registrationWriter.write(false), "registration fixture emission failed");

  for (const char* project : {"empty", "single", "dll"}) {
    const std::array<uint8_t, 4> bytes{0x4E, 0x80, 0x00, 0x20};
    TestModule module;
    module.Load(kBase, bytes.data(), bytes.size());
    RecompilerConfig smallConfig;
    smallConfig.projectName = project;
    smallConfig.outDirectoryPath = "generated";
    auto small = CodegenContext::Create(BinaryView::fromModule(module), std::move(smallConfig));
    small.setConfigDir(root / project);
    small.analysisState().format = "xex";
    small.analysisState().loadAddress = kBase;
    small.analysisState().entryPoint = kBase;
    small.analysisState().imageSize = bytes.size();
    small.setDllModule(std::string_view(project) == "dll");
    if (std::string_view(project) != "empty") {
      auto* node = small.graph.addFunction(kBase, 4, FunctionAuthority::CONFIG, true);
      Check(node != nullptr, "single registration node rejected");
      node->discover({{kBase, 4}}, {}, {});
      node->seal();
    }
    CodegenWriter smallWriter(small);
    Check(smallWriter.write(false), "small registration fixture emission failed");
  }

  // Capture a pending node before graph notification resolves the external
  // target. Test this stale snapshot directly with the production emitter:
  // normal discovery usually resolves it immediately, but the wrapper must
  // still honor owner metadata instead of inventing a label in a CONFIG hole.
  auto external = Context(root / "external", config, false, kBase + 0x30, true);
  FunctionNode snapshot = *external.graph.getFunction(kEntry);
  snapshot.setName("pending_external_snapshot");
  auto* externalTarget = external.graph.addFunction(kBase + 0x30, 12,
      FunctionAuthority::CONFIG, true);
  externalTarget->discover({{kBase + 0x30, 12}}, {}, {});
  externalTarget->seal();
  external.graph.addCallToFunction(kBase, kBase + 0x1C,
      CallTarget::function(externalTarget));
  const auto* externalOwner = external.graph.getFunction(kBase);
  Check(externalOwner->containsAddress(kBase + 0x30), "fixture must exercise CONFIG hole fallback");
  Check(snapshot.unresolvedJumps().size() == 1, "snapshot lost its pending branch");
  EmitContext emission{external.binary(), external.Config(), external.graph, kBase + 0x80};
  auto code = externalOwner->emitCpp(emission, &snapshot);
  Check(code.find("goto loc_82000030") == std::string::npos,
        "resolved external call was promoted to an un-emitted local label");
  std::ofstream(root / "external_snapshot.cpp")
      << "#include \"interior_pch.h\"\n"
         "DECLARE_REX_FUNC(xstart);\nDECLARE_REX_FUNC(sub_82000030);\n"
      << externalTarget->emitCpp(emission) << code;

  auto reject = [&](RecompilerConfig invalid, const char* name, bool missingEntry = false,
                    uint32_t pendingTarget = 0) {
    auto out = root / name;
    auto bad = Context(out, std::move(invalid), missingEntry, pendingTarget);
    CodegenWriter rejected(bad);
    Check(!rejected.write(true), "invalid body accepted even with force");
    Check(!fs::exists(out / "generated"), "invalid body created output");
  };
  auto invalid = config;
  invalid.functions.at(kEntry).bodyOwner = kBase + 0x30;
  reject(invalid, "missing-owner");
  reject(config, "missing-entry-in-owner", true);
  reject(config, "pending-target-gap", false, kBase + 0x30);
  reject(config, "pending-target-outside-image", false, kBase + 0x94);
  invalid = config;
  invalid.functions.at(kEntry).bodyOwner = kEntry;
  reject(invalid, "self-owner");
  invalid = config;
  invalid.functions[kBase].bodyOwner = kEntry;
  reject(invalid, "cyclic-owner");
  invalid = config;
  invalid.functions.at(kEntry).parent = kBase;
  reject(invalid, "chunk-owner");
  invalid = config;
  invalid.functions.at(kEntry).bodyOwner = kBase + 1;
  reject(invalid, "unaligned-owner");
  invalid = config;
  invalid.rexcrtFunctions["test"] = kEntry;
  reject(invalid, "rexcrt-entry");
  // Register localization no longer rejects body= entries. Interior entries,
  // their owners and partners keep every register in ctx (per-function
  // fallback). Each case must emit, those four functions must declare no
  // register local, and a control module without body= entries must still get
  // the locals, which proves the flag itself took effect. The probe fixture
  // puts lwarx r0,0,r31 in the extra owner/entry block so reserved, r0 and r31
  // would be localized there without the fallback.
  using Flag = bool RecompilerConfig::*;
  const Flag ctrFlag = &RecompilerConfig::ctrAsLocalVariable;
  const Flag xerFlag = &RecompilerConfig::xerAsLocalVariable;
  const Flag reservedFlag = &RecompilerConfig::reservedRegisterAsLocalVariable;
  const Flag crFlag = &RecompilerConfig::crRegistersAsLocalVariables;
  const Flag nonArgumentFlag = &RecompilerConfig::nonArgumentRegistersAsLocalVariables;
  const Flag nonVolatileFlag = &RecompilerConfig::nonVolatileRegistersAsLocalVariables;
  struct Localization {
    const char* name;
    std::vector<Flag> flags;
    std::set<std::string> controlLocals;
  };
  const std::vector<Localization> localizations{
      {"ctr", {ctrFlag}, {"ctr"}},
      {"xer", {xerFlag}, {"xer"}},
      {"reserved", {reservedFlag}, {"reserved"}},
      {"cr", {crFlag}, {"cr0"}},
      {"non-volatile", {nonVolatileFlag}, {"r31"}},
      {"non-argument-and-non-volatile", {nonArgumentFlag, nonVolatileFlag}, {"r0", "r31"}},
      {"all", {ctrFlag, xerFlag, reservedFlag, crFlag, nonArgumentFlag, nonVolatileFlag},
       {"ctr", "xer", "reserved", "cr0", "r0", "r31"}}};
  for (const auto& localization : localizations) {
    auto localized = config;
    for (auto flag : localization.flags) localized.*flag = true;
    auto out = root / (std::string("localization-") + localization.name);
    auto probe = Context(out, localized, false, 0, false, true);
    CodegenWriter probeWriter(probe);
    Check(probeWriter.write(false), "register localization rejected a body= fixture");
    auto sources = ReadSources(out / "generated", "interior");
    for (const char* function : {"sub_82000000", "sub_82000018", "sub_82000040", "sub_82000048"}) {
      Check(RegisterLocals(FunctionText(sources, function)).empty(),
            "interior entry, owner or partner kept a localized register");
    }
    auto controlOut = root / (std::string("localization-control-") + localization.name);
    auto control = ControlContext(controlOut, localization.flags);
    CodegenWriter controlWriter(control);
    Check(controlWriter.write(false), "localization control module emission failed");
    auto controlLocals = RegisterLocals(FunctionText(ReadSources(controlOut / "generated", "control"),
                                                     "xstart"));
    for (const auto& expected : localization.controlLocals) {
      Check(controlLocals.count(expected) == 1, "localization flag had no effect on the control module");
    }
  }
  // Localized r11/r12 with r14-r31 kept in ctx would split one live value
  // between ctx and a local across the save/restore helpers, so the writer
  // refuses non_argument without non_volatile.
  invalid = config;
  invalid.nonArgumentRegistersAsLocalVariables = true;
  reject(invalid, "localization-non-argument-only");
  // sub_82000090 reaches the body= entry only through case 0 of a jump table.
  // Whether the table is one of the function's analysed jump tables or a config
  // switch table, the fallback must keep the dispatcher's registers in ctx (its
  // cmplwi would otherwise write a local cr0) while it still emits the switch
  // and the tail call. Without localization the same switch is emitted. A table
  // that reaches no interior entry leaves the dispatcher localized.
  auto switchText = [&](const char* name, RecompilerConfig switchConfig, SwitchTable table) {
    auto out = root / name;
    auto module = Context(out, std::move(switchConfig), false, 0, false, false, table);
    CodegenWriter switchWriter(module);
    Check(switchWriter.write(false), "switch fixture emission failed");
    return FunctionText(ReadSources(out / "generated", "interior"), "sub_82000090");
  };
  auto dispatchesToEntry = [](const std::string& text) {
    return text.find("switch (") != std::string::npos &&
           text.find("sub_82000018(ctx, base);") != std::string::npos;
  };
  auto switchLocalized = config;
  switchLocalized.crRegistersAsLocalVariables = true;
  for (auto table : {SwitchTable::Analysis, SwitchTable::Config}) {
    const bool analysed = table == SwitchTable::Analysis;
    auto control = switchText(analysed ? "switch-analysis-control" : "switch-config-control",
                              config, table);
    Check(dispatchesToEntry(control), "switch control did not tail-call the body= entry");
    auto localized = switchText(analysed ? "switch-analysis-localized" : "switch-config-localized",
                                switchLocalized, table);
    Check(dispatchesToEntry(localized) && RegisterLocals(localized).empty(),
          "a switch into a body= entry kept a localized register");
  }
  auto precise = switchLocalized;
  precise.switchTables.emplace(kSwitchBctr, EntrySwitch(kBase + 0x80));
  Check(RegisterLocals(switchText("switch-precision", precise, SwitchTable::Config)).count("cr0") == 1,
        "a switch that reaches no interior entry lost its localized registers");
  // The fallback covers every graph edge and config switch table into an
  // interior entry, so BuilderContext::check_register_handoff has no known
  // trigger through write(). It guards future gaps, such as jump tables found
  // only during emission (emitCpp's late detection, whose mtctr pattern
  // currently never matches). Drive the production emitter with an incomplete
  // fallback: the switch case into the body= entry must be counted (write()
  // then fails before flushing any output), and must not be once the
  // dispatcher keeps its registers in ctx.
  {
    auto net = Context(root / "switch-safety-net", switchLocalized, false, 0, false, false,
                       SwitchTable::Config);
    const std::unordered_set<uint32_t> interior{kEntry, kExtraEntry};
    const std::unordered_set<uint32_t> none;
    const std::unordered_set<uint32_t> dispatcher{kSwitch};
    size_t errors = 0;
    EmitContext netEmission{net.binary(), net.Config(), net.graph, kBase + 0x80};
    netEmission.interiorEntries = &interior;
    netEmission.registersInCtx = &none;
    netEmission.handoffErrors = &errors;
    const auto* node = net.graph.getFunction(kSwitch);
    Check(node != nullptr, "switch dispatcher missing from the graph");
    Check(dispatchesToEntry(node->emitCpp(netEmission)) && errors == 1,
          "handoff check missed a localized switch case into a body= entry");
    errors = 0;
    netEmission.registersInCtx = &dispatcher;
    Check(dispatchesToEntry(node->emitCpp(netEmission)) && errors == 0,
          "handoff check fired for a dispatcher that keeps its registers in ctx");
  }
  // skip_lr: the blrl and PIC bodies keep LR, with the same text as without
  // skip_lr, and the body that only saves and restores the return address
  // around a call drops every LR access.
  {
    std::string controlBlrl, controlPic;
    for (bool skipLr : {false, true}) {
      auto out = root / (skipLr ? "skip-lr" : "skip-lr-control");
      auto lrModule = LrContext(out, skipLr);
      CodegenWriter lrWriter(lrModule);
      Check(lrWriter.write(false), "skip_lr fixture emission failed");
      auto sources = ReadSources(out / "generated", "lr");
      auto blrl = FunctionText(sources, "sub_82000000");
      auto pic = FunctionText(sources, "sub_82000010");
      auto saver = FunctionText(sources, "xstart");
      Check(blrl.find("ctx.lr = ctx.r11.u64;") != std::string::npos &&
                blrl.find("auto old_lr = ctx.lr;") != std::string::npos,
            "blrl lost the LR it calls through");
      Check(pic.find("ctx.lr = 0x82000014;") != std::string::npos &&
                pic.find("ctx.r3.u64 = ctx.lr;") != std::string::npos,
            "bl $+4; mflr lost the address it reads from LR");
      Check(saver.find("sub_82000000(ctx, base);") != std::string::npos,
            "skip_lr fixture call missing");
      Check((saver.find("ctx.lr") == std::string::npos) == skipLr,
            skipLr ? "skip_lr kept LR in a body that only saves it"
                   : "LR accesses missing without skip_lr");
      if (!skipLr) {
        controlBlrl = blrl;
        controlPic = pic;
      } else {
        Check(blrl == controlBlrl && pic == controlPic,
              "a body that uses LR as data differs from its output without skip_lr");
      }
    }
  }
  // Invalid body values must fail config loading, rather than silently discard
  // the override and emit the original lossy isolated function.
  for (const char* value : {"'bad'", "0", "0x82000018", "0x82000001", "-1",
                           "0x100000000"}) {
    auto path = root / "invalid.toml";
    std::ofstream(path) << "file_path = 'synthetic.bin'\n"
        "[functions]\n0x82000018 = { body = " << value << " }\n";
    RecompilerConfig bad;
    Check(!bad.Load(path.string()), "invalid TOML body accepted");
  }
  std::cout << "Real sealed/deferred codegen body emission, 7 register localization fallback "
               "cases, 5 switch-table fallback cases, the emitter handoff safety net, 2 skip_lr "
               "LR-use cases and 16 rejection cases passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
