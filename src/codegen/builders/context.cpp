/**
 * @file        rexcodegen/builders/context.cpp
 * @brief       PPC context instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <algorithm>

#include <rex/codegen/binary_view.h>
#include <rex/logging.h>

#include "../codegen_logging.h"
#include <rex/system/export_resolver.h>

namespace rex::codegen {

/// eieio instruction encoding (big-endian). Used for MMIO detection:
/// if the next instruction after a load/store is eieio, the access is MMIO.
static constexpr uint32_t kEieioEncoding = 0xAC06007C;

//=============================================================================
// Convenience Accessors
//=============================================================================

const RecompilerConfig& BuilderContext::config() const {
  return emitCtx.config;
}

const FunctionGraph& BuilderContext::graph() const {
  return emitCtx.graph;
}

bool BuilderContext::trackLr() const {
  return !config().skipLr || keepLr;
}

//=============================================================================
// Register Accessors
//=============================================================================

// Localizing a non-volatile assumes the function owns it across its whole body.
// An SEH funclet does not: it inherits its owner's live registers through ctx.
bool BuilderContext::localizeNonVolatiles() const {
  return config().nonVolatileRegistersAsLocalVariables && !fn.sharesRegisters() &&
         !registersInCtx;
}

std::string BuilderContext::r(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables && !registersInCtx &&
       (index == 0 || index == 2 || index == 11 || index == 12)) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.r[index] = true;
    return fmt::format("r{}", index);
  }
  return fmt::format("ctx.r{}", index);
}

std::string BuilderContext::f(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables && !registersInCtx && index == 0) ||
      (localizeNonVolatiles() && index >= 14)) {
    locals.f[index] = true;
    return fmt::format("f{}", index);
  }
  return fmt::format("ctx.f{}", index);
}

std::string BuilderContext::v(size_t index) {
  const auto& cfg = config();
  if ((cfg.nonArgumentRegistersAsLocalVariables && !registersInCtx &&
       (index >= 32 && index <= 63)) ||
      (localizeNonVolatiles() && ((index >= 14 && index <= 31) || (index >= 64 && index <= 127)))) {
    locals.v[index] = true;
    return fmt::format("v{}", index);
  }
  return fmt::format("ctx.v{}", index);
}

std::string BuilderContext::cr(size_t index) {
  if (config().crRegistersAsLocalVariables && !registersInCtx) {
    locals.cr[index] = true;
    return fmt::format("cr{}", index);
  }
  return fmt::format("ctx.cr{}", index);
}

const char* BuilderContext::ctr() {
  if (config().ctrAsLocalVariable && !registersInCtx) {
    locals.ctr = true;
    return "ctr";
  }
  return "ctx.ctr";
}

const char* BuilderContext::xer() {
  if (config().xerAsLocalVariable && !registersInCtx) {
    locals.xer = true;
    return "xer";
  }
  return "ctx.xer";
}

const char* BuilderContext::reserved() {
  if (config().reservedRegisterAsLocalVariable && !registersInCtx) {
    locals.reserved = true;
    return "reserved";
  }
  return "ctx.reserved";
}

const char* BuilderContext::temp() {
  locals.temp = true;
  return "temp";
}

const char* BuilderContext::v_temp() {
  locals.v_temp = true;
  return "vTemp";
}

const char* BuilderContext::env() {
  locals.env = true;
  return "env";
}

const char* BuilderContext::ea() {
  locals.ea = true;
  return "ea";
}

//=============================================================================
// Output Helpers
//=============================================================================

// Template implementations in header, but we need explicit instantiations
// for common format strings to avoid link errors in some cases

//=============================================================================
// Code Generation Helpers
//=============================================================================

bool BuilderContext::mmio_check_d_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[2]);
}

bool BuilderContext::mmio_check_x_form() {
  if (base + 4 < fn.end() && *(data + 1) == kEieioEncoding)
    return true;
  return locals.is_mmio_base(insn.operands[1]) || locals.is_mmio_base(insn.operands[2]);
}

const CallTarget* BuilderContext::findCallTarget(uint32_t site) const {
  // Search in calls (bl instructions)
  for (const auto& edge : fn.calls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  // Search in tail calls (b instructions to other functions)
  for (const auto& edge : fn.tailCalls()) {
    if (edge.site == site) {
      return &edge.target;
    }
  }

  return nullptr;
}

void BuilderContext::check_register_handoff(uint32_t target) const {
  if (!registersInCtx && emitCtx.interiorEntries && emitCtx.interiorEntries->contains(target)) {
    REXCODEGEN_ERROR("0x{:08X} in 0x{:08X} reaches interior entry 0x{:08X} with localized registers",
                     base, fn.base(), target);
    if (emitCtx.handoffErrors)
      ++*emitCtx.handoffErrors;
  }
}

//=============================================================================
// Guest Tail Calls (RecompilerConfig::musttailCalls, TOML musttail_calls)
//=============================================================================
//
// With musttail_calls on, a guest tail call is emitted as
//   [[clang::musttail]] return NAME(ctx, base);
// instead of `NAME(ctx, base);` + `return;`. clang then has to lower it to a
// jmp at every optimization level, and the host stack stops growing along guest
// tail-call chains. That is valid because every guest function and import is
// declared through REX_FUNC with the one signature
// void(PPCContext& __restrict, uint8_t*), the call passes the caller's own
// parameters through, and no generated local has a non-trivial destructor
// (bench/agent9/musttail/README.md). The old text stays wherever that is not
// provable:
//  - SEH-wrapped bodies (generateExceptionHandlers with SEH scopes): a musttail
//    cannot leave the __try / SehGuard try block (compile error), and the
//    handler would no longer cover the callee.
//  - Functions that call setjmp (setJmpAddress): the saved context refers to
//    this frame, which a musttail releases before the callee could longjmp.
//  - Anything but the plain one-line call: longjmp, setjmp, elided
//    __save/__rest helpers, the SEH-funclet register handoff, unresolved targets.
// REX_FUNC_PROLOGUE declares a Tracy zone (non-trivial destructor) when
// REXGLUE_PROFILE_GUEST_FUNCTIONS and REXGLUE_ENABLE_PROFILING are both defined,
// and clang rejects every musttail in that build: keep the option off there.
// Mid-asm hooks get references to locals and must not keep them past the hook
// call: the frame is gone before the tail-called function runs.

namespace {

constexpr std::string_view kMustTailReturn = "[[clang::musttail]] return ";

// Whether a direct branch in this function targets setJmpAddress, so that
// emit_function_call() puts ppc_setjmp() in this frame. Both readings of the
// displacement (relative, and absolute for AA=1) count, which is conservative.
bool calls_setjmp(const BuilderContext& ctx) {
  const uint32_t target = ctx.config().setJmpAddress;
  if (target == 0)
    return false;
  for (const auto& block : ctx.fn.blocks()) {
    const uint8_t* p = ctx.emitCtx.binary.translate(block.base);
    if (!p)
      continue;
    for (uint32_t addr = block.base; addr < block.end(); addr += 4, p += 4) {
      const uint32_t w = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      int32_t disp;
      if ((w >> 26) == 18)  // b, ba, bl, bla: LI
        disp = (int32_t(w << 6) >> 6) & ~3;
      else if ((w >> 26) == 16)  // bc, bca, bcl, bcla: BD
        disp = int32_t(int16_t(w & 0xFFFC));
      else
        continue;
      if (addr + uint32_t(disp) == target || uint32_t(disp) == target)
        return true;
    }
  }
  return false;
}

bool musttail_allowed(const BuilderContext& ctx) {
  const auto& cfg = ctx.config();
  if (!cfg.musttailCalls)
    return false;
  // The generateSeh test of FunctionNode::recompile(): the body is in SEH_TRY.
  if (cfg.generateExceptionHandlers && ctx.fn.hasExceptionInfo()) {
    const auto* seh = ctx.fn.exceptionInfo()->asSeh();
    if (seh && !seh->scopes.empty())
      return false;
  }
  return !calls_setjmp(ctx);
}

bool is_identifier(std::string_view s) {
  if (s.empty() || (s[0] >= '0' && s[0] <= '9'))
    return false;
  for (char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
  }
  return true;
}

}  // namespace

// `<indent>NAME(ctx, base);` and `<indent>return;`, or the single line
// `<indent>[[clang::musttail]] return NAME(ctx, base);` where musttail is
// allowed. Used by the conditional tail calls below and by bctr jump tables.
void emit_tail_call_line(BuilderContext& ctx, std::string_view indent, std::string_view name) {
  if (musttail_allowed(ctx)) {
    ctx.println("{}{}{}(ctx, base);", indent, kMustTailReturn, name);
    return;
  }
  ctx.println("{}{}(ctx, base);", indent, name);
  ctx.println("{}return;", indent);
}

// A `b` tail call: emit_function_call() followed by `return;`. Where musttail
// is allowed and the call came out as the plain line `\tNAME(ctx, base);`, that
// line becomes the musttail return; every other form keeps its text and the
// `return;`.
void emit_tail_call(BuilderContext& ctx, uint32_t address) {
  const size_t mark = ctx.out.size();
  ctx.emit_function_call(address);
  if (musttail_allowed(ctx)) {
    constexpr std::string_view kCallEnd = "(ctx, base);\n";
    const std::string_view call = std::string_view(ctx.out).substr(mark);
    if (call.size() > 1 + kCallEnd.size() && call.front() == '\t' && call.ends_with(kCallEnd)) {
      const std::string_view name = call.substr(1, call.size() - 1 - kCallEnd.size());
      if (is_identifier(name)) {
        std::string line = fmt::format("\t{}{}(ctx, base);\n", kMustTailReturn, name);
        ctx.out.resize(mark);
        ctx.out += line;
        return;
      }
    }
  }
  ctx.println("\treturn;");
}

void BuilderContext::emit_function_call(uint32_t address) {
  const auto& cfg = config();

  if (address == cfg.longJmpAddress) {
    // Use custom ppc_longjmp that uses guest address as key (not for storage)
    println("\tppc_longjmp({}.u32, {}.s32);", r(3), r(4));
    return;
  }

  if (address == cfg.setJmpAddress) {
    // Save PPCContext for restoration after longjmp
    println("\t{} = ctx;", env());
    // Use custom ppc_setjmp that uses guest address as key
    println("\t{}.s64 = ppc_setjmp({}.u32);", temp(), r(3));
    // Restore PPCContext if returning from longjmp
    println("\tif ({}.s64 != 0) ctx = {};", temp(), env());
    println("\t{} = {};", r(3), temp());
    return;
  }

  // Try to use pre-resolved call target from FunctionGraph
  if (const auto* target = findCallTarget(base)) {
    if (target->isFunction()) {
      auto* targetFn = target->asFunction();
      const auto& name = targetFn->name();
      check_register_handoff(static_cast<uint32_t>(targetFn->base()));

      // Handle save/restore helpers. Gated on the global setting, not this
      // function's: the helper bodies are emitted under it too, so once they
      // spill to locals they are no-ops that scribble zeros on the caller's
      // frame. A share_registers function still has to elide them. A
      // registersInCtx function keeps its non-volatiles in ctx and needs the
      // real save/restore; the writer emits the helpers in ctx for it.
      if (cfg.nonVolatileRegistersAsLocalVariables && !registersInCtx &&
          (name.find("__rest") == 0 || name.find("__save") == 0)) {
        // print nothing - these are handled by local variable tracking
        return;
      }

      // An SEH funclet runs on its owner's frame and reads whatever non-volatiles
      // the owner left live, so hand it the localized copies through ctx and take
      // them back afterwards. Only registers already localized here can be live at
      // this point, so that set is the whole live-in the funclet can see.
      if (targetFn->sharesRegisters() && localizeNonVolatiles()) {
        for (size_t i = 14; i < 32; ++i) {
          if (locals.r[i])
            println("\tctx.r{} = r{};", i, i);
        }
        emitCtx.reference(name);
        println("\t{}(ctx, base);", name);
        for (size_t i = 14; i < 32; ++i) {
          if (locals.r[i])
            println("\tr{} = ctx.r{};", i, i);
        }
        return;
      }

      emitCtx.reference(name);
      println("\t{}(ctx, base);", name);
      return;
    }

    if (target->isImport()) {
      const auto& importTarget = std::get<CallTarget::ToImport>(target->value);
      std::string func_name;

      // Try to resolve ordinal to actual function name
      auto at_pos = importTarget.name.find('@');
      if (at_pos != std::string::npos && emitCtx.resolver) {
        auto lib_name = importTarget.name.substr(0, at_pos);
        auto ordinal_str = importTarget.name.substr(at_pos + 1);
        uint16_t ordinal = static_cast<uint16_t>(std::stoul(ordinal_str));

        auto* exp = emitCtx.resolver->GetExportByOrdinal(lib_name + ".xex", ordinal);
        if (!exp)
          exp = emitCtx.resolver->GetExportByOrdinal(lib_name, ordinal);

        if (exp) {
          func_name = "__imp__" + std::string(exp->name);
        }
      }

      if (func_name.empty()) {
        func_name = "__imp__" + importTarget.name;
        std::replace(func_name.begin(), func_name.end(), '@', '_');
        std::replace(func_name.begin(), func_name.end(), '.', '_');
      }

      emitCtx.reference(func_name);
      println("\t{}(ctx, base);", func_name);
      return;
    }

    // Unresolved target from graph
    REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X}", address, base);
    println("\t// FATAL: unresolved function 0x{:08X}", address);
    println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
    return;
  }

  // No pre-resolved target found - this is an error
  REXCODEGEN_ERROR("Unresolved function 0x{:08X} from 0x{:08X} (no CallTarget in FunctionNode)",
                   address, base);
  println("\t// FATAL: unresolved function 0x{:08X} (no CallTarget in FunctionNode)", address);
  println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", base, address);
}

void BuilderContext::emit_conditional_branch(bool not_, std::string_view cond) {
  uint32_t target = insn.operands[1];

  // A conditional branch to the entry of the function being emitted is a loop
  // back to the top, never a tail call. classifyTarget() has to answer that
  // from getFunctionContaining(base), which resolves to the nearest preceding
  // entry point; when a second legitimate entry (an address the game only ever
  // reaches by indirect call, so it is declared separately) sits inside this
  // function's range, that lookup returns the wrong node, the branch is read as
  // a call into another function, no CallTarget exists for it, and the emitted
  // REX_FATAL kills the game at runtime. Here the function knows its own base,
  // so answer directly. LEGO Dimensions 2026-09-02: sub_83988648 spans
  // 0x83988648-0x8398A8C4 and also contains the declared entry 0x83988EB0; the
  // loop at 0x8398A800 crashed on arrival in the Sonic world.
  // Deliberately ONLY the entry, not the whole body. Extending this to any
  // target inside the function looks right and would make code_pointer_scan
  // usable (that scan finds ~340 more functions here, but declares some inside
  // existing functions, and every branch across such a split becomes a runtime
  // fatal). Both attempts - FunctionNode::containsAddress() and a scan of
  // fn.blocks() - emitted a goto to an address the label pass never labels, and
  // the generated code stopped compiling ("use of undeclared label", e.g.
  // sub_8318C5F8 -> 0x8318C618). Doing it properly means teaching the label
  // collection in FunctionNode::recompile() the same rule; until then the entry
  // case alone is provably safe, because a function's first block always starts
  // there.
  if (target == fn.base()) {
    println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond, target);
    return;
  }

  // The label already exists in this function's body, so the goto is valid by
  // construction - emit it without asking classifyTarget.
  //
  // classifyTarget only trusts an address that lands inside a recorded block,
  // while the label collection walks the function linearly. Where the compiler
  // left out-of-line code in a gap between blocks, a branch into it classified
  // as Unknown and became a REX_FATAL even though the target label sits a few
  // lines above in the same body. That is what killed the game on the ropes:
  // four branches to loc_839AAE20 inside sub_839AACC0.
  if (fn.isLabel(target)) {
    println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond, target);
    return;
  }

  // Use classifyTarget for consistent branch classification
  // false = branch instruction (not a call), so own-base means loop back
  auto kind = graph().classifyTarget(target, base, false);

  switch (kind) {
    case TargetKind::InternalLabel:
      // Target is within this function - local goto
      println("\tif ({}{}.{}) goto loc_{:08X};", not_ ? "!" : "", cr(insn.operands[0]), cond,
              target);
      break;

    case TargetKind::Function:
    case TargetKind::Import:
      // Conditional tail call to another function - check pre-resolved call target
      if (const auto* callTarget = findCallTarget(base)) {
        if (callTarget->isFunction()) {
          auto* targetFn = callTarget->asFunction();
          check_register_handoff(static_cast<uint32_t>(targetFn->base()));
          emitCtx.reference(targetFn->name());
          println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
          emit_tail_call_line(*this, "\t\t", targetFn->name());
          println("\t}}");
        } else if (callTarget->isImport()) {
          const auto& importTarget = std::get<CallTarget::ToImport>(callTarget->value);
          std::string func_name = "__imp__" + importTarget.name;
          std::replace(func_name.begin(), func_name.end(), '@', '_');
          std::replace(func_name.begin(), func_name.end(), '.', '_');
          emitCtx.reference(func_name);
          println("\tif ({}{}.{}) {{", not_ ? "!" : "", cr(insn.operands[0]), cond);
          emit_tail_call_line(*this, "\t\t", func_name);
          println("\t}}");
        }
      } else {
        REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X} (no CallTarget)",
                         target, base);
        println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X} in sub_{:08X}\");",
                not_ ? "!" : "", cr(insn.operands[0]), cond, base, target, fn.base());
      }
      break;

    case TargetKind::Unknown:
      REXCODEGEN_ERROR("Unresolved conditional branch to 0x{:08X} from 0x{:08X}", target, base);
      println("\t// ERROR: conditional branch to unknown address 0x{:08X}", target);
      println("\tif ({}{}.{}) REX_FATAL(\"Unresolved branch from 0x{:08X} to 0x{:08X} in sub_{:08X}\");",
              not_ ? "!" : "", cr(insn.operands[0]), cond, base, target, fn.base());
      break;
  }
}

void BuilderContext::emit_set_flush_mode(bool enable) {
  auto newState = enable ? CSRState::VMX : CSRState::FPU;
  if (csrState != newState) {
    auto prefix = enable ? "enable" : "disable";
    auto suffix = csrState != CSRState::Unknown ? "Unconditional" : "";
    println("\tctx.fpscr.{}FlushMode{}();", prefix, suffix);

    csrState = newState;
  }
}

bool BuilderContext::has_mid_asm_hook() const {
  return config().midAsmHooks.find(base) != config().midAsmHooks.end();
}

void BuilderContext::emit_mid_asm_hook() {
  auto midAsmHook = config().midAsmHooks.find(base);
  if (midAsmHook == config().midAsmHooks.end()) {
    return;
  }

  bool returnsBool = midAsmHook->second.returnOnFalse || midAsmHook->second.returnOnTrue ||
                     midAsmHook->second.jumpAddressOnFalse != 0 ||
                     midAsmHook->second.jumpAddressOnTrue != 0;

  print("\t");
  if (returnsBool)
    print("if (");

  // Build call -- no ctx/base prefix, just register arguments resolved through accessors
  print("{}(", midAsmHook->second.name);
  for (auto& reg : midAsmHook->second.registers) {
    if (out.back() != '(')
      out += ", ";

    switch (reg[0]) {
      case 'c':
        if (reg == "ctr")
          out += ctr();
        else
          out += cr(std::atoi(reg.c_str() + 2));
        break;
      case 'x':
        out += xer();
        break;
      case 'r':
        if (reg == "reserved")
          out += reserved();
        else
          out += r(std::atoi(reg.c_str() + 1));
        break;
      case 'f':
        if (reg == "fpscr")
          out += "ctx.fpscr";
        else
          out += f(std::atoi(reg.c_str() + 1));
        break;
      case 'v':
        out += v(std::atoi(reg.c_str() + 1));
        break;
    }
  }

  if (returnsBool) {
    println(")) {{");

    if (midAsmHook->second.returnOnTrue)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnTrue != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnTrue);

    println("\t}}");

    println("\telse {{");

    if (midAsmHook->second.returnOnFalse)
      println("\t\treturn;");
    else if (midAsmHook->second.jumpAddressOnFalse != 0)
      println("\t\tgoto loc_{:X};", midAsmHook->second.jumpAddressOnFalse);

    println("\t}}");
  } else {
    println(");");

    if (midAsmHook->second.ret)
      println("\treturn;");
    else if (midAsmHook->second.jumpAddress != 0)
      println("\tgoto loc_{:X};", midAsmHook->second.jumpAddress);
  }
}

//=============================================================================
// Vector (SIMD) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_vec_fp_binary(const char* simd_op) {
  println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_{}_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      v(insn.operands[0]), simd_op, v(insn.operands[1]), v(insn.operands[2]));
}

void BuilderContext::emit_vec_fp_unary_expr(std::string_view simd_expr) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);

  // Replace {vA} placeholder in expression with actual register
  std::string expr(simd_expr);
  size_t pos;
  while ((pos = expr.find("{vA}")) != std::string::npos) {
    expr.replace(pos, 4, vA);
  }

  println("\tsimde_mm_store_ps({}.f32, {});", vD, expr);
}

void BuilderContext::emit_vec_int_binary(const char* simd_op, const char* element_type) {
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[1]), element_type,
      v(insn.operands[2]), element_type);
}

void BuilderContext::emit_vec_int_binary_swapped(const char* simd_op, const char* element_type) {
  // Swapped: op(vB, vA) instead of op(vA, vB) - useful for andnot which has reversed operand
  // semantics
  println(
      "\tsimde_mm_store_si128((simde__m128i*){}.{}, "
      "simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.{}), "
      "simde_mm_load_si128((simde__m128i*){}.{})));",
      v(insn.operands[0]), element_type, simd_op, v(insn.operands[2]), element_type,
      v(insn.operands[1]), element_type);
}

void BuilderContext::emit_vec_var_shift(const char* shift_dir, const char* element_type,
                                        uint32_t mask_value) {
  auto vD = v(insn.operands[0]);
  auto vA = v(insn.operands[1]);
  auto vB = v(insn.operands[2]);
  println("\t{{");
  println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_{}(0x{:X}));", element_type,
          mask_value);
  println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_{}_{}"
      "(a, shift));",
      vD, shift_dir, element_type);
  println("\t}}");
}

//=============================================================================
// Memory (Load/Store) Code Generation Helpers
//=============================================================================

void BuilderContext::emit_load_d_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // D-form: rD = LOAD(rA + D) where operands[0]=rD, operands[1]=D, operands[2]=rA
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{});", static_cast<int32_t>(insn.operands[1]));
}

void BuilderContext::emit_load_x_form(const char* load_macro, const char* dest_type,
                                      bool check_mmio) {
  // X-form: rD = LOAD(rA + rB) where operands[0]=rD, operands[1]=rA, operands[2]=rB
  // load_macro should be like "REX_LOAD_U8" - we replace REX_LOAD with REX_MM_LOAD for MMIO
  const char* macro = load_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {
    // Replace "REX_LOAD_" with "REX_MM_LOAD_"
    if (strncmp(load_macro, "REX_LOAD_", 9) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_LOAD_%s", load_macro + 9);
      macro = mm_macro;
    }
  }

  print("\t{}.{} = {}(", r(insn.operands[0]), dest_type, macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32);", r(insn.operands[2]));
}

void BuilderContext::emit_store_d_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // D-form: STORE(rA + D, rS) where operands[0]=rS, operands[1]=D, operands[2]=rA
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_d_form()) {
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  print("\t{}(", macro);
  if (insn.operands[2] != 0)
    print("{}.u32 + ", r(insn.operands[2]));
  println("{}, {}.{});", static_cast<int32_t>(insn.operands[1]), r(insn.operands[0]), src_type);
}

void BuilderContext::emit_store_x_form(const char* store_macro, const char* src_type,
                                       bool check_mmio) {
  // X-form: STORE(rA + rB, rS) where operands[0]=rS, operands[1]=rA, operands[2]=rB
  // store_macro should be like "REX_STORE_U8" - we replace REX_STORE with REX_MM_STORE for MMIO
  const char* macro = store_macro;
  static char mm_macro[64];
  if (check_mmio && mmio_check_x_form()) {  // Use X-form specific check (operands[1] is base)
    // Replace "REX_STORE_" with "REX_MM_STORE_"
    if (strncmp(store_macro, "REX_STORE_", 10) == 0) {
      snprintf(mm_macro, sizeof(mm_macro), "REX_MM_STORE_%s", store_macro + 10);
      macro = mm_macro;
    }
  }

  print("\t{}(", macro);
  if (insn.operands[1] != 0)
    print("{}.u32 + ", r(insn.operands[1]));
  println("{}.u32, {}.{});", r(insn.operands[2]), r(insn.operands[0]), src_type);
}

}  // namespace rex::codegen
