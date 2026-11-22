/**
 * @file        rexcodegen/builders/flush_mode.h
 * @brief       Flush-mode switch elision for FP/VMX ops whose results do not depend on FTZ/DAZ
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "builder_context.h"

namespace rex::codegen {

// Flush-mode switch for builders whose emitted code gives bit-identical results (outputs, NaN
// payloads, CR/FPSCR/VSCR state) with MXCSR FTZ/DAZ on or off, in every rounding mode: bit ops and
// moves on the raw pattern, loads/stores of raw bits, cvtsi2sd/cvtdq2ps (never denormal), cvtt*
// (denormal -> 0 either way), roundps with an explicit nearest/zero immediate. Machine-checked per
// site in lego-dimensions-recomp bench/agent9/flush (README.md).
// With flush_mode_elision on, nothing is emitted and ctx.csrState is left unchanged, so the next
// mode-dependent op still switches exactly when needed. With it off, this is emit_set_flush_mode().
inline void emitFlushModeUnlessIndependent(BuilderContext& ctx, bool enable) {
  if (ctx.config().flushModeElision)
    return;
  ctx.emit_set_flush_mode(enable);
}

}  // namespace rex::codegen
