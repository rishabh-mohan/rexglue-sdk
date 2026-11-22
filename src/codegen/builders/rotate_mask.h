/**
 * @file        rexcodegen/builders/rotate_mask.h
 * @brief       32-bit lowering of rlwinm / rlwnm / rlwimi
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The generic rotate-left-word emitters follow the 64-bit ISA text literally:
//
//   rA.u64 = __builtin_rotateleft64(rS.u32 | (rS.u64 << 32), n) & MASK(mb + 32, me + 32)
//
// Rotating the doubled low word by n < 32 gives the doubled 32-bit rotation R:R,
// R = rotl32(rS.u32, n). When mb <= me the mask only covers the low word, so the
// result is zext64(R & mask32(mb, me)). The helpers below write that 32-bit value
// as a shift, an and, or a 32-bit rotate. Each one is the same function of the full
// 64-bit inputs as the generic text (rS high word ignored, rA high word cleared, or
// kept for rlwimi), so it can replace the generic text for every input.
//
// When mb > me the mask wraps and also covers the whole high word (the result is
// R:(R & mask)). Those cases are not lowered: the helpers return nullopt and the
// caller keeps the generic form.
//
// Each helper returns one statement without the leading tab and the newline.
// tests/ppc/asm/instr_rlw_lowering.s covers each form.

namespace rex::codegen::rlw {

enum class Form : uint8_t {
  kNone,            // not lowered: wrapping mask (mb > me) or operand out of range
  kCopy,            // sh == 0, full mask:          x
  kMask,            // sh == 0:                     x & m                 (clrrwi)
  kShiftLeft,       // m == ~0u << sh:              x << sh               (slwi)
  kShiftLeftMask,   // m inside ~0u << sh:          (x << sh) & m         (extlwi, insrwi)
  kShiftRight,      // m == ~0u >> (32 - sh):       x >> (32 - sh)        (srwi)
  kShiftRightMask,  // m inside ~0u >> (32 - sh):   (x >> (32 - sh)) & m  (extrwi, inslwi)
  kRotate,          // full mask:                   rotl32(x, sh)         (rotlwi)
  kRotateMask,      // anything else:               rotl32(x, sh) & m
};

// 32-bit mask with bits mb..me set, bit 0 being the MSB. Requires mb <= me <= 31.
constexpr uint32_t mask32(uint32_t mb, uint32_t me) {
  return (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me));
}

constexpr Form classify(uint32_t sh, uint32_t mb, uint32_t me) {
  if (sh > 31 || mb > 31 || me > 31 || mb > me)
    return Form::kNone;
  const uint32_t m = mask32(mb, me);
  if (sh == 0)
    return m == 0xFFFFFFFFu ? Form::kCopy : Form::kMask;
  // rotl32(x, sh) == (x << sh) | (x >> (32 - sh)). The two halves occupy the
  // disjoint bit sets `left` and `right`, so a mask inside one set only keeps
  // bits of that half.
  const uint32_t left = 0xFFFFFFFFu << sh;
  const uint32_t right = 0xFFFFFFFFu >> (32 - sh);
  if ((m & ~left) == 0)
    return m == left ? Form::kShiftLeft : Form::kShiftLeftMask;
  if ((m & ~right) == 0)
    return m == right ? Form::kShiftRight : Form::kShiftRightMask;
  return m == 0xFFFFFFFFu ? Form::kRotate : Form::kRotateMask;
}

// Same text as fmt's "0x{:X}".
inline std::string hexLiteral(uint64_t v) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  char digits[16];
  int n = 0;
  do {
    digits[n++] = kDigits[v & 0xF];
    v >>= 4;
  } while (v != 0);
  std::string s = "0x";
  while (n != 0)
    s += digits[--n];
  return s;
}

// rotl32(rs.u32, sh) & mask32(mb, me) as an expression of type uint32_t.
// form must be classify(sh, mb, me); returns an empty string for kNone.
inline std::string rotMaskExpr(std::string_view rs, uint32_t sh, uint32_t mb, uint32_t me,
                               Form form) {
  if (form == Form::kNone)
    return {};
  std::string x(rs);
  x += ".u32";
  const std::string m = hexLiteral(mask32(mb, me));
  const std::string l = std::to_string(sh);
  const std::string r = std::to_string(32 - sh);
  switch (form) {
    case Form::kCopy:
      return x;
    case Form::kMask:
      return x + " & " + m;
    case Form::kShiftLeft:
      return x + " << " + l;
    case Form::kShiftLeftMask:
      return "(" + x + " << " + l + ") & " + m;
    case Form::kShiftRight:
      return x + " >> " + r;
    case Form::kShiftRightMask:
      return "(" + x + " >> " + r + ") & " + m;
    case Form::kRotate:
      return "__builtin_rotateleft32(" + x + ", " + l + ")";
    case Form::kRotateMask:
      return "__builtin_rotateleft32(" + x + ", " + l + ") & " + m;
    case Form::kNone:
      break;
  }
  return {};
}

// rlwinm rA,rS,sh,mb,me: "rA.u64 = <expr>;"
inline std::optional<std::string> lowerRlwinm(std::string_view ra, std::string_view rs,
                                              uint32_t sh, uint32_t mb, uint32_t me) {
  const Form form = classify(sh, mb, me);
  if (form == Form::kNone)
    return std::nullopt;
  std::string s(ra);
  s += ".u64 = ";
  s += rotMaskExpr(rs, sh, mb, me, form);
  s += ';';
  return s;
}

// rlwnm rA,rS,rB,mb,me: the rotate count is rB & 0x1F at run time, so only the
// mask is known. Uses the 32-bit rotate with the count expression of the generic form.
inline std::optional<std::string> lowerRlwnm(std::string_view ra, std::string_view rs,
                                             std::string_view rb, uint32_t mb, uint32_t me) {
  if (mb > 31 || me > 31 || mb > me)
    return std::nullopt;
  const uint32_t m = mask32(mb, me);
  std::string s(ra);
  s += ".u64 = __builtin_rotateleft32(";
  s += rs;
  s += ".u32, ";
  s += rb;
  s += ".u8 & 0x1F)";
  if (m != 0xFFFFFFFFu) {
    s += " & ";
    s += hexLiteral(m);
  }
  s += ';';
  return s;
}

// rlwimi rA,rS,sh,mb,me: "rA.u64 = (<expr>) | (rA.u64 & <keep>);". keep is the
// generic form's ~MASK constant, so every bit outside mask32(mb, me), including
// rA's high word, is kept.
inline std::optional<std::string> lowerRlwimi(std::string_view ra, std::string_view rs,
                                              uint32_t sh, uint32_t mb, uint32_t me) {
  const Form form = classify(sh, mb, me);
  if (form == Form::kNone)
    return std::nullopt;
  std::string s(ra);
  s += ".u64 = (";
  s += rotMaskExpr(rs, sh, mb, me, form);
  s += ") | (";
  s += ra;
  s += ".u64 & ";
  s += hexLiteral(~uint64_t{mask32(mb, me)});
  s += ");";
  return s;
}

}  // namespace rex::codegen::rlw
