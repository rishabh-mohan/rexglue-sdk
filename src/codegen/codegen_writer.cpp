/**
 * @file        codegen/codegen_writer.cpp
 * @brief       Consolidated codegen output writer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/codegen_writer.h>
#include "codegen_flags.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <inja/inja.hpp>

#include <rex/codegen/function_graph.h>
#include <rex/codegen/output_partition.h>
#include <rex/codegen/template_registry.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>

#include "codegen_logging.h"
#include "file_io.h"
#include "template_registry_internal.h"

namespace {

bool ValidateFunctionBodies(const rex::codegen::RecompilerConfig& config,
                            const rex::codegen::FunctionGraph& graph,
                            const rex::codegen::BinaryView& binary) {
  for (const auto& [address, function] : config.functions) {
    if (!function.bodyOwner)
      continue;
    auto reject = [&](std::string_view reason) {
      REXCODEGEN_ERROR("Function 0x{:08X} body 0x{:08X}: {}", address,
                       function.bodyOwner, reason);
      return false;
    };
    if ((address & 3) || (function.bodyOwner & 3) || function.parent ||
        address == function.bodyOwner)
      return reject("body requires a distinct aligned owner and cannot be a chunk");
    auto ownerConfig = config.functions.find(function.bodyOwner);
    if (ownerConfig != config.functions.end() && ownerConfig->second.bodyOwner)
      return reject("nested or cyclic body owners are unsupported");
    const auto* entry = graph.getFunction(address);
    const auto* owner = graph.getFunction(function.bodyOwner);
    if (!entry || !owner || entry->isRegistered() || owner->isRegistered() ||
        entry->isImport() || owner->isImport() || owner->blocks().empty() ||
        entry->blocks().empty()) {
      REXCODEGEN_ERROR("Body node states: entry present={} state={} import={} unresolved={}; owner present={} state={} import={} unresolved={}",
          entry != nullptr, entry ? int(entry->state()) : -1, entry && entry->isImport(),
          entry ? entry->unresolvedJumps().size() : 0, owner != nullptr,
          owner ? int(owner->state()) : -1, owner && owner->isImport(),
          owner ? owner->unresolvedJumps().size() : 0);
      return reject("entry and owner must have discovered non-import instruction blocks");
    }
    if (std::any_of(config.rexcrtFunctions.begin(), config.rexcrtFunctions.end(),
                    [&](const auto& crt) {
                      return crt.second == address || crt.second == function.bodyOwner;
                    }))
      return reject("body entries cannot replace a rexcrt mapping");
    bool contained = std::any_of(owner->blocks().begin(), owner->blocks().end(),
                                  [&](const rex::codegen::Block& block) {
      return address >= block.base && uint64_t(address) + 4 <= uint64_t(block.base) + block.size;
    });
    if (!contained || !binary.translate(address))
      return reject("entry instruction is missing from the owner's actual blocks");
    // The emission union must never wrap an address or import unmapped bytes.
    for (const auto* node : {owner, entry}) {
      for (const auto& block : node->blocks()) {
        const auto* section = binary.findSection(block.base);
        if (!block.size || (block.base & 3) || (block.size & 3) ||
            block.base < owner->base() || !section || !section->executable ||
            uint64_t(block.base) + block.size > UINT32_MAX ||
            uint64_t(block.base) + block.size > uint64_t(section->baseAddress) + section->size)
          return reject("body has an invalid or unmapped instruction block");
      }
    }
    auto inBody = [&](uint32_t instruction) {
      if (instruction & 3) return false;
      for (const auto* node : {owner, entry}) {
        for (const auto& block : node->blocks()) {
          if (instruction >= block.base &&
              uint64_t(instruction) + 4 <= uint64_t(block.base) + block.size)
            return true;
        }
      }
      return false;
    };
    auto resolvedAt = [&](uint32_t site) -> const rex::codegen::CallTarget* {
      // Match the emission union's owner-first precedence exactly.
      for (const auto* node : {owner, entry}) {
        for (const auto* edges : {&node->calls(), &node->tailCalls()}) {
          for (const auto& edge : *edges)
            if (edge.site == site) return &edge.target;
        }
      }
      return nullptr;
    };
    for (const auto* node : {owner, entry}) {
      for (const auto& jump : node->unresolvedJumps()) {
        if (!inBody(jump.site)) return reject("pending branch site is outside the actual body");
        const auto* bytes = binary.translate(jump.site);
        uint32_t instruction = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
                               (uint32_t(bytes[2]) << 8) | bytes[3];
        uint32_t opcode = instruction >> 26;
        if ((opcode != 16 && opcode != 18) || bool(instruction & 1) != jump.isCall ||
            (opcode == 16) != jump.isConditional)
          return reject("pending branch metadata does not match its instruction");
        unsigned width = opcode == 16 ? 16 : 26;
        uint32_t displacementBits = instruction & ((uint32_t(1) << width) - 4);
        int64_t displacement = displacementBits;
        if (displacementBits & (uint32_t(1) << (width - 1)))
          displacement -= int64_t(1) << width;
        uint32_t target = static_cast<uint32_t>(displacement +
            ((instruction & 2) ? 0 : int64_t(jump.site)));
        if (target != jump.target) return reject("pending branch target does not match its instruction");
        const auto* call = resolvedAt(jump.site);
        bool mapped = call && ((call->isFunction() && call->asFunction() &&
                               call->asFunction()->base() == target) ||
            (call->isImport() &&
             std::get<rex::codegen::CallTarget::ToImport>(call->value).address == target));
        if (mapped) continue;
        if (jump.isCall || !inBody(target))
          return reject("pending branch has no actual local instruction or resolved call target");
        // Conditional local branches use owner labels. An unconditional jump
        // to a graph entry still requires call metadata with the current builder.
        auto kind = graph.classifyTarget(target, jump.site, false);
        if (!jump.isConditional && (kind == rex::codegen::TargetKind::Function ||
                                   kind == rex::codegen::TargetKind::Import))
          return reject("pending unconditional branch conflicts with another graph entry");
      }
    }
  }
  return true;
}

// Register localization assumes every transfer into a function is a real call
// whose callee reads only argument registers from ctx. An interior entry breaks
// that: the code before it can leave any register live. So while a localization
// option is on, interior entries (body= entries and entries inside another
// function's blocks), body owners and every function with a direct edge or jump
// table into an interior entry keep all registers in ctx. Those make real calls
// to the save/restore helpers, so the helpers keep their registers in ctx too.
struct RegisterFallback {
  std::unordered_set<uint32_t> interior;
  std::unordered_set<uint32_t> inCtx;
};

RegisterFallback BuildRegisterFallback(const rex::codegen::RecompilerConfig& config,
                                       const rex::codegen::FunctionGraph& graph) {
  RegisterFallback result;
  if (!config.ctrAsLocalVariable && !config.xerAsLocalVariable &&
      !config.reservedRegisterAsLocalVariable && !config.crRegistersAsLocalVariables &&
      !config.nonArgumentRegistersAsLocalVariables && !config.nonVolatileRegistersAsLocalVariables)
    return result;

  auto isHelper = [](const rex::codegen::FunctionNode& node) {
    return node.authority() == rex::codegen::FunctionAuthority::HELPER ||
           node.name().starts_with("__save") || node.name().starts_with("__rest");
  };
  size_t owners = 0;
  for (const auto& [address, function] : config.functions) {
    if (function.bodyOwner && result.interior.insert(address).second)
      owners += result.inCtx.insert(function.bodyOwner).second;
  }
  const size_t bodyEntries = result.interior.size();

  // Helpers overlap each other by design and are not interior entries.
  std::set<uint32_t> entries;
  for (const auto& [address, node] : graph.functions()) {
    if (!node->isImport() && !isHelper(*node))
      entries.insert(address);
  }
  for (const auto& [address, node] : graph.functions()) {
    if (node->isImport() || isHelper(*node))
      continue;
    for (const auto& block : node->blocks()) {
      for (auto it = entries.lower_bound(block.base); it != entries.end() && *it < block.end();
           ++it) {
        if (*it != address)
          result.interior.insert(*it);
      }
    }
  }
  if (result.interior.empty())
    return result;

  auto hitsInterior = [&](const std::vector<uint32_t>& targets) {
    return std::any_of(targets.begin(), targets.end(),
                       [&](uint32_t target) { return result.interior.contains(target); });
  };
  auto intoInterior = [&](const rex::codegen::CallEdge& edge) {
    const auto* target = edge.target.asFunction();
    return target && result.interior.contains(static_cast<uint32_t>(target->base()));
  };
  size_t callers = 0, helpers = 0;
  for (const auto& [address, node] : graph.functions()) {
    bool reaches = std::any_of(node->calls().begin(), node->calls().end(), intoInterior) ||
                   std::any_of(node->tailCalls().begin(), node->tailCalls().end(), intoInterior) ||
                   std::any_of(node->jumpTables().begin(), node->jumpTables().end(),
                               [&](const auto& table) { return hitsInterior(table.targets); });
    if (reaches && !result.interior.contains(address))
      callers += result.inCtx.insert(address).second;
    else if (isHelper(*node))
      helpers += result.inCtx.insert(address).second;
  }
  for (const auto& [bctr, table] : config.switchTables) {
    if (!hitsInterior(table.targets))
      continue;
    for (const auto& [address, node] : graph.functions()) {
      if (std::any_of(node->blocks().begin(), node->blocks().end(),
                      [&](const auto& block) { return block.contains(bctr); }))
        callers += result.inCtx.insert(address).second;
    }
  }
  result.inCtx.insert(result.interior.begin(), result.interior.end());
  REXCODEGEN_INFO(
      "Register localization fallback: {} functions keep registers in ctx ({} interior entries, "
      "{} of them body=; {} more body owners, {} callers, {} save/restore helpers)",
      result.inCtx.size(), result.interior.size(), bodyEntries, owners, callers, helpers);
  return result;
}

nlohmann::json buildTemplateData(const rex::codegen::CodegenContext& ctx,
                                 const std::vector<const rex::codegen::FunctionNode*>& functions,
                                 const std::unordered_map<uint32_t, std::string>& rexcrtByAddr) {
  const auto& cfg = ctx.Config();

  // Compute code_base and code_size from binary sections
  size_t codeMin = ~size_t(0);
  size_t codeMax = 0;
  for (const auto& section : ctx.binary().sections()) {
    if (section.executable) {
      if (section.baseAddress < codeMin)
        codeMin = section.baseAddress;
      if ((section.baseAddress + section.size) > codeMax)
        codeMax = section.baseAddress + section.size;
    }
  }

  // Build functions JSON array
  nlohmann::json functionsJson = nlohmann::json::array();
  nlohmann::json registrationsJson = nlohmann::json::array();
  for (const auto* fn : functions) {
    std::string funcName;
    bool isRexcrt = false;

    auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
    if (crtIt != rexcrtByAddr.end()) {
      funcName = crtIt->second;
      isRexcrt = true;
    } else if (fn->base() == ctx.analysisState().entryPoint) {
      funcName = "xstart";
    } else if (!fn->name().empty()) {
      funcName = fn->name();
    } else {
      funcName = fmt::format("sub_{:08X}", fn->base());
    }

    functionsJson.push_back({
        {"address", fmt::format("0x{:X}", fn->base())},
        {"name", funcName},
        {"is_rexcrt", isRexcrt},
        {"below_code_base", (fn->base() < codeMin)},
        {"is_import", fn->authority() == rex::codegen::FunctionAuthority::IMPORT},
    });
    // Preserve the registration order and filtering used by the mapping
    // table, including import thunks below the executable code range.
    if (fn->base() >= codeMin || fn->authority() == rex::codegen::FunctionAuthority::IMPORT)
      registrationsJson.push_back({{"address", fmt::format("0x{:X}", fn->base())},
                                   {"name", funcName}});
  }

  // Build config flags
  nlohmann::json configFlags = {
      {"skip_lr", cfg.skipLr},
      {"ctr_as_local", cfg.ctrAsLocalVariable},
      {"xer_as_local", cfg.xerAsLocalVariable},
      {"reserved_as_local", cfg.reservedRegisterAsLocalVariable},
      {"skip_msr", cfg.skipMsr},
      {"cr_as_local", cfg.crRegistersAsLocalVariables},
      {"non_argument_as_local", cfg.nonArgumentRegistersAsLocalVariables},
      {"non_volatile_as_local", cfg.nonVolatileRegistersAsLocalVariables},
  };

  return {
      {"project", cfg.projectName},
      {"image_base", fmt::format("0x{:X}", ctx.binary().baseAddress())},
      {"image_size", fmt::format("0x{:X}", ctx.binary().imageSize())},
      {"code_base", fmt::format("0x{:X}", codeMin)},
      {"code_size", fmt::format("0x{:X}", codeMax - codeMin)},
      {"rexcrt_heap", cfg.rexcrtFunctions.contains("RtlAllocateHeap") ? 1 : 0},
      {"thunk_reserve_size", fmt::format("0x{:X}", 0x10000u)},
      {"has_dll_modules", ctx.hasDllModules()},
      {"is_dll", ctx.isDllModule()},
      {"config_flags", configFlags},
      {"functions", functionsJson},
      {"registrations", registrationsJson},
      {"recomp_files", nlohmann::json::array()},
  };
}

}  // namespace

namespace rex::codegen {

// cr_compare_so_zero. Generated code writes XER.SO only in mtxer
// (build_mtspr) and through mid-asm hooks that take xer: OE forms do not model
// SO. A local xer is zero-initialized. ctx.xer.so starts at 0 (ThreadState
// value-initializes its PPCContext, and so do GuestToHostFunction, the
// auto-isolating ImportFunction call and the hook call contexts), and the
// runtime only copies it (XThread::SaveContext/LoadContext). The one exception
// is rex::CallFrame (hook.h): it leaves every register except r1, r13 and fpscr
// uninitialized, so a guest call through it reads an indeterminate XER.SO with
// or without this option (no LEGO code uses CallFrame). So SO is 0 at every
// compare of a body without a writer whose XER is local, and of every body
// without a writer when no body that keeps XER in ctx has one.
void PrepareEmitAnalyses(EmitContext& emitCtx, const std::vector<const FunctionNode*>& functions,
                         EmitAnalyses& storage) {
  const auto& config = emitCtx.config;
  auto fallback = BuildRegisterFallback(config, emitCtx.graph);
  storage.interiorEntries = std::move(fallback.interior);
  storage.registersInCtx = std::move(fallback.inCtx);
  if (!storage.registersInCtx.empty()) {
    emitCtx.interiorEntries = &storage.interiorEntries;
    emitCtx.registersInCtx = &storage.registersInCtx;
  }
  // The blocks an entry's text is emitted from: its body= owner's, or its own.
  auto bodyOf = [&](const FunctionNode* fn) {
    const auto configured = config.functions.find(fn->base());
    const auto* body = configured != config.functions.end() && configured->second.bodyOwner
                           ? emitCtx.graph.getFunction(configured->second.bodyOwner)
                           : fn;
    return body ? body : fn;
  };

  // skip_lr drops every LR write (bl, bctrl, blrl, mtlr) and read (mflr). That
  // is safe where LR only carries the return address: blr is `return;` either
  // way, and mflr/mtlr just move it through the stack. A body that uses LR as
  // data would read a stale ctx.lr: `mtlr rX; blrl` calls through it, and a bl
  // to one of the body's own labels (`bl $+4; mflr rY`) takes an address from
  // it. Such a body (a bclrl, i.e. blrl or a conditional form, or a bl that
  // build_bl emits as a local goto) keeps LR exactly as without skip_lr.
  if (config.skipLr) {
    constexpr uint32_t kBclrlMask = 0xFC0007FF;  // opcode 19, XO 16, LK 1, any BO/BI
    constexpr uint32_t kBclrl = 0x4C000021;
    constexpr uint32_t kBlMask = 0xFC000003;  // opcode 18, AA 0, LK 1
    constexpr uint32_t kBl = 0x48000001;
    for (const auto* fn : functions) {
      bool usesLr = false;
      for (const auto& block : bodyOf(fn)->blocks()) {
        const auto* data = reinterpret_cast<const uint8_t*>(emitCtx.binary.translate(block.base));
        for (size_t offset = 0; data && !usesLr && block.base + offset < block.end(); offset += 4) {
          const auto pc = static_cast<uint32_t>(block.base + offset);
          const uint32_t word = rex::memory::load_and_swap<uint32_t>(data + offset);
          if ((word & kBclrlMask) == kBclrl) {
            usesLr = true;
          } else if ((word & kBlMask) == kBl) {
            // The classification build_bl uses: InternalLabel becomes a goto.
            const auto disp = (static_cast<int32_t>(word << 6) >> 6) & ~3;
            const uint32_t target = pc + static_cast<uint32_t>(disp);
            usesLr = emitCtx.graph.classifyTarget(target, pc, true) ==
                     rex::codegen::TargetKind::InternalLabel;
          }
        }
        if (usesLr)
          break;
      }
      if (usesLr)
        storage.lrUsers.insert(static_cast<uint32_t>(fn->base()));
    }
    emitCtx.lrUsers = &storage.lrUsers;
    REXCODEGEN_INFO("skip_lr: {} functions use LR as data (blrl, or bl to a local label) and keep LR",
                    storage.lrUsers.size());
  }

  if (!config.crCompareSoZero)
    return;

  std::vector<uint32_t> hookWriters;
  for (const auto& [address, hook] : config.midAsmHooks) {
    if (std::any_of(hook.registers.begin(), hook.registers.end(),
                    [](const std::string& reg) { return !reg.empty() && reg[0] == 'x'; }))
      hookWriters.push_back(address);
  }
  constexpr uint32_t kMtxerMask = 0xFC1FFFFF;  // mtspr XER, rS with rS masked out
  constexpr uint32_t kMtxer = 0x7C0103A6;
  size_t inCtxWriters = 0;
  for (const auto* fn : functions) {
    const auto* body = bodyOf(fn);
    bool writes = false;
    for (const auto& block : body->blocks()) {
      const auto* data = reinterpret_cast<const uint8_t*>(emitCtx.binary.translate(block.base));
      for (size_t offset = 0; data && !writes && block.base + offset < block.end(); offset += 4)
        writes = (rex::memory::load_and_swap<uint32_t>(data + offset) & kMtxerMask) == kMtxer;
      writes = writes || std::any_of(hookWriters.begin(), hookWriters.end(),
                                     [&](uint32_t hook) { return block.contains(hook); });
      if (writes)
        break;
    }
    if (!writes)
      continue;
    const auto address = static_cast<uint32_t>(fn->base());
    storage.xerWriters.insert(address);
    if (!config.xerAsLocalVariable || storage.registersInCtx.contains(address))
      ++inCtxWriters;
  }
  emitCtx.xerWriters = &storage.xerWriters;
  emitCtx.ctxXerWritten = inCtxWriters != 0;
  REXCODEGEN_INFO(
      "cr_compare_so_zero: {} functions write XER.SO ({} keep XER in ctx); the others compare "
      "with SO = 0{}",
      storage.xerWriters.size(), inCtxWriters,
      inCtxWriters ? " where their XER is local" : "");
}

bool IsGeneratedOutputName(std::string_view filename, std::string_view projectName) {
  auto dot = filename.rfind('.');
  if (dot == std::string_view::npos)
    return false;
  auto ext = filename.substr(dot);
  if (ext != ".cpp" && ext != ".h" && ext != ".cmake")
    return false;

  if (filename == "sources.cmake")
    return true;

  constexpr std::array<std::string_view, 4> kPrefixes{"ppc_recomp", "ppc_func_mapping",
                                                      "function_table_init", "ppc_config"};
  for (auto prefix : kPrefixes) {
    if (filename.starts_with(prefix))
      return true;
  }

  std::string projectPrefix(projectName);
  projectPrefix += '_';
  return filename.starts_with(projectPrefix);
}

constexpr size_t kOutputBufferReserveSize = 32 * 1024 * 1024;  // 32 MB

CodegenWriter::CodegenWriter(CodegenContext& ctx, Runtime* runtime)
    : ctx_(ctx), runtime_(runtime) {}

// Convenience accessors
FunctionGraph& CodegenWriter::graph() {
  return ctx_.graph;
}
const FunctionGraph& CodegenWriter::graph() const {
  return ctx_.graph;
}
const BinaryView& CodegenWriter::binary() const {
  return ctx_.binary();
}
RecompilerConfig& CodegenWriter::config() {
  return ctx_.Config();
}
const RecompilerConfig& CodegenWriter::config() const {
  return ctx_.Config();
}
AnalysisState& CodegenWriter::analysisState() {
  return ctx_.analysisState();
}
const AnalysisState& CodegenWriter::analysisState() const {
  return ctx_.analysisState();
}

bool CodegenWriter::write(bool force) {
  deletedFiles_.clear();
  writtenFiles_.clear();
  unchangedFiles_.clear();

  // --- Validation gate (from recompile.cpp) ---
  if (ctx_.errors.HasErrors() && !force) {
    REXCODEGEN_ERROR("Code generation blocked: {} validation errors. Use --force to override.",
                     ctx_.errors.Count());
    return false;
  }
  if (!ValidateFunctionBodies(config(), graph(), binary()))
    return false;
  // The FPR/VMX save/restore helpers take r12/r11 implicitly from ctx. Their calls
  // are elided only with non_volatile_as_local, so a local r11/r12 would hand a
  // real helper call a stale ctx value (function_scanner.cpp helper patterns).
  if (config().nonArgumentRegistersAsLocalVariables &&
      !config().nonVolatileRegistersAsLocalVariables) {
    REXCODEGEN_ERROR(
        "non_argument_as_local requires non_volatile_as_local: the save/restore helpers read "
        "r11/r12 from ctx");
    return false;
  }

  // --- Output directory setup (from recompile.cpp) ---
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;
  REXCODEGEN_TRACE("Output path: {}", outputPath.string());
  std::filesystem::create_directories(outputPath);

  // --- Everything below from recompiler.cpp recompile() ---
  REXCODEGEN_TRACE("Recompile: starting");
  out.reserve(kOutputBufferReserveSize);

  // Build sorted function list from graph
  std::vector<const FunctionNode*> functions;
  functions.reserve(graph().functionCount());
  for (const auto& [addr, node] : graph().functions()) {
    functions.push_back(node.get());
  }
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->base() < b->base(); });

  // Build rexcrt reverse map and rename graph nodes
  std::unordered_map<uint32_t, std::string> rexcrtByAddr;
  for (const auto& [name, addr] : config().rexcrtFunctions) {
    auto crtName = fmt::format("rexcrt_{}", name);
    rexcrtByAddr[addr] = crtName;
    if (auto* node = graph().getFunction(addr)) {
      node->setName(std::move(crtName));
    }
  }

  const std::string& projectName = config().projectName;

  TemplateRegistry registry;
  if (!config().templateDir.empty())
    registry.loadOverrides(config().templateDir);

  auto tmplData = buildTemplateData(ctx_, functions, rexcrtByAddr);

  // Generate {project}_pch.h (config + macros, stable enough to precompile)
  REXCODEGEN_TRACE("Recompile: generating {}_pch.h", projectName);
  out = renderWithJson(registry, "codegen/pch_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_pch.h", projectName));

  // Generate {project}_funcs.h (every guest function declaration)
  REXCODEGEN_TRACE("Recompile: generating {}_funcs.h", projectName);
  out = renderWithJson(registry, "codegen/funcs_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_funcs.h", projectName));

  // Generate {project}_init.h (the full surface, for init.cpp and consumers)
  REXCODEGEN_TRACE("Recompile: generating {}_init.h", projectName);
  out = renderWithJson(registry, "codegen/init_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.h", projectName));

  // Generate {project}_init.cpp (PPCImageConfig + PPCFuncMappings)
  REXCODEGEN_TRACE("Recompile: generating {}_init.cpp", projectName);
  out = renderWithJson(registry, "codegen/init_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.cpp", projectName));

  // Generate {project}_register.cpp (registration function for hash-based dispatch)
  REXCODEGEN_TRACE("Recompile: generating {}_register.cpp", projectName);
  tmplData["is_dll"] = ctx_.isDllModule();
  out = renderWithJson(registry, "codegen/register_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_register.cpp", projectName));

  // Filter out imports and rexcrt functions before recompilation
  std::erase_if(functions, [](const FunctionNode* fn) {
    return fn->authority() == FunctionAuthority::IMPORT;
  });
  std::erase_if(functions, [&rexcrtByAddr](const FunctionNode* fn) {
    return rexcrtByAddr.contains(static_cast<uint32_t>(fn->base()));
  });

  // Build EmitContext -- resolver is now properly connected
  EmitContext emitCtx{binary(), config(), graph(),
                      static_cast<uint32_t>(analysisState().entryPoint), nullptr};
  if (runtime_)
    emitCtx.resolver = runtime_->export_resolver();

  // Interior entries and everything that can reach them keep registers in ctx;
  // cr_compare_so_zero needs the XER.SO writers.
  EmitAnalyses analyses;
  PrepareEmitAnalyses(emitCtx, functions, analyses);

  REXCODEGEN_TRACE("Recompiling {} functions...", functions.size());

  std::vector<std::string> bodies;
  std::vector<FunctionSize> sizes;
  std::vector<std::unordered_set<std::string>> references;
  bodies.reserve(functions.size());
  sizes.reserve(functions.size());
  references.reserve(functions.size());

  const size_t maxFileBytes = REXCVAR_GET(max_file_size_bytes);
  std::unordered_set<std::string> referenced;
  emitCtx.referenced = &referenced;
  size_t handoffErrors = 0;
  emitCtx.handoffErrors = &handoffErrors;
  for (const auto* fn : functions) {
    referenced.clear();
    const auto configured = config().functions.find(fn->base());
    const auto* body = configured != config().functions.end() && configured->second.bodyOwner
        ? graph().getFunction(configured->second.bodyOwner) : fn;
    std::string code = body->emitCpp(emitCtx, body == fn ? nullptr : fn);
    if (code.size() > maxFileBytes) {
      REXCODEGEN_WARN("Function 0x{:08X} is {} bytes, exceeds max_file_size_bytes ({})", fn->base(),
                      code.size(), maxFileBytes);
    }
    sizes.push_back({static_cast<uint32_t>(fn->base()), code.size()});
    bodies.push_back(std::move(code));
    references.push_back(referenced);
  }
  emitCtx.referenced = nullptr;
  emitCtx.handoffErrors = nullptr;
  if (handoffErrors) {
    // Nothing has been flushed yet: drop the buffered headers too.
    REXCODEGEN_ERROR(
        "Code generation blocked: {} control transfers reach an interior entry with localized "
        "registers",
        handoffErrors);
    pendingWrites.clear();
    out.clear();
    return false;
  }

  auto partition = OutputPartition::Load(outputPath / kPartitionFileName);
  auto buckets = partition.Assign(sizes, maxFileBytes);

  for (size_t index = 0; index < buckets.size(); ++index) {
    // Buckets are address-ordered, so a call can precede its definition.
    std::unordered_set<std::string> needed;
    for (size_t entry : buckets[index]) {
      needed.insert(references[entry].begin(), references[entry].end());
    }

    std::vector<std::string> ordered(needed.begin(), needed.end());
    std::sort(ordered.begin(), ordered.end());
    println("#pragma once\n");
    println("#include \"{}_pch.h\"\n", projectName);
    for (const auto& name : ordered) {
      println("DECLARE_REX_FUNC({});", name);
    }
    SaveCurrentOutData(fmt::format("{}_funcs.{}.h", projectName, index));

    println("#include \"{}_funcs.{}.h\"\n", projectName, index);
    for (size_t entry : buckets[index]) {
      out += bodies[entry];
    }
    SaveCurrentOutData(fmt::format("{}_recomp.{}.cpp", projectName, index));
  }

  out = partition.Serialize();
  SaveCurrentOutData(kPartitionFileName);

  REXCODEGEN_TRACE("Recompilation complete.");

  // Generate sources.cmake
  REXCODEGEN_TRACE("Recompile: generating sources.cmake");
  {
    auto& recompFiles = tmplData["recomp_files"];
    recompFiles = nlohmann::json::array();
    for (size_t i = 0; i < buckets.size(); ++i) {
      recompFiles.push_back(fmt::format("{}_recomp.{}.cpp", projectName, i));
    }
    out = renderWithJson(registry, "codegen/sources_cmake", tmplData);
    SaveCurrentOutData("sources.cmake");
  }

  // Write all buffered files to disk
  return FlushPendingWrites();
}

void CodegenWriter::SaveCurrentOutData(const std::string_view name) {
  if (!out.empty()) {
    pendingWrites.emplace_back(std::string(name), std::move(out));
    out.clear();
  }
}

bool CodegenWriter::FlushPendingWrites() {
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;

  std::unordered_set<std::string> emitted;
  emitted.reserve(pendingWrites.size());

  // A swallowed failure would be stamped as success and skipped on the next run.
  bool ok = true;

  for (const auto& [filename, content] : pendingWrites) {
    emitted.insert(filename);
    auto filePath = outputPath / filename;

    switch (WriteIfChanged(filePath, content)) {
      case WriteOutcome::Failed:
        REXCODEGEN_ERROR("Failed to write {}", filePath.string());
        ok = false;
        break;
      case WriteOutcome::Unchanged:
        REXCODEGEN_TRACE("Unchanged, skipping write: {}", filePath.string());
        unchangedFiles_.push_back(filename);
        break;
      case WriteOutcome::Written:
        REXCODEGEN_TRACE("Wrote {} bytes to {}", content.size(), filePath.string());
        writtenFiles_.push_back(filename);
        break;
    }
  }

  for (const auto& entry : std::filesystem::directory_iterator(outputPath)) {
    if (!entry.is_regular_file())
      continue;
    auto filename = entry.path().filename().string();
    if (emitted.contains(filename))
      continue;
    if (!IsGeneratedOutputName(filename, config().projectName))
      continue;

    std::error_code ec;
    std::filesystem::remove(entry.path(), ec);
    if (ec) {
      REXCODEGEN_ERROR("Failed to delete stale output {}: {}", entry.path().string(), ec.message());
      continue;
    }
    REXCODEGEN_TRACE("Deleted stale output: {}", entry.path().string());
    deletedFiles_.push_back(filename);
  }

  pendingWrites.clear();
  return ok;
}

}  // namespace rex::codegen
