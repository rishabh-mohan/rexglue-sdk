# rlwinm / rlwnm / rlwimi with RecompilerConfig::rlwinmLowering (one test per lowering
# class). Expected values come from a reference model of the 64-bit ISA text:
# rA = ROTL32(rS, n) & MASK(mb + 32, me + 32), rlwimi keeps rA outside the mask.
# rS always has garbage in its high word and rA starts as garbage, so rA's high word
# must come out cleared (rlwinm, rlwnm) or kept (rlwimi). The wrap tests (mb > me)
# keep the generic 64-bit form. The text after "->" is the lowered statement.

test_rlw_lowering_rlwinm_clrrwi:
  # clrrwi 4 -> mask: r3.u64 = r4.u32 & 0xFFFFFFF0;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xCAFEBABE12345678
  rlwinm r3, r4, 0, 0, 27
  blr
  #_ REGISTER_OUT r3 0x0000000012345670
  #_ REGISTER_OUT r4 0xCAFEBABE12345678

test_rlw_lowering_rlwinm_mask_mid:
  # rlwinm 0,8,23 -> mask: r3.u64 = r4.u32 & 0xFFFF00;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF89ABCDEF
  rlwinm r3, r4, 0, 8, 23
  blr
  #_ REGISTER_OUT r3 0x0000000000ABCD00
  #_ REGISTER_OUT r4 0xFFFFFFFF89ABCDEF

test_rlw_lowering_rlwinm_slwi:
  # slwi 4 -> shl: r3.u64 = r4.u32 << 4;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  rlwinm r3, r4, 4, 0, 27
  blr
  #_ REGISTER_OUT r3 0x0000000023456780
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678

test_rlw_lowering_rlwinm_slwi_31:
  # slwi 31 -> shl: r3.u64 = r4.u32 << 31;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFE00000003
  rlwinm r3, r4, 31, 0, 0
  blr
  #_ REGISTER_OUT r3 0x0000000080000000
  #_ REGISTER_OUT r4 0xFFFFFFFE00000003

test_rlw_lowering_rlwinm_shl_mask:
  # rlwinm 2,20,29 -> shl+mask: r3.u64 = (r4.u32 << 2) & 0xFFC;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xDEADBEEF123456FF
  rlwinm r3, r4, 2, 20, 29
  blr
  #_ REGISTER_OUT r3 0x0000000000000BFC
  #_ REGISTER_OUT r4 0xDEADBEEF123456FF

test_rlw_lowering_rlwinm_extlwi:
  # extlwi 8,8 -> shl+mask: r3.u64 = (r4.u32 << 8) & 0xFF000000;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0x0123456789ABCDEF
  rlwinm r3, r4, 8, 0, 7
  blr
  #_ REGISTER_OUT r3 0x00000000AB000000
  #_ REGISTER_OUT r4 0x0123456789ABCDEF

test_rlw_lowering_rlwinm_srwi:
  # srwi 8 -> shr: r3.u64 = r4.u32 >> 8;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xA5A5A5A512345678
  rlwinm r3, r4, 24, 8, 31
  blr
  #_ REGISTER_OUT r3 0x0000000000123456
  #_ REGISTER_OUT r4 0xA5A5A5A512345678

test_rlw_lowering_rlwinm_srwi_31:
  # srwi 31 -> shr: r3.u64 = r4.u32 >> 31;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0x0000000180000000
  rlwinm r3, r4, 1, 31, 31
  blr
  #_ REGISTER_OUT r3 0x0000000000000001
  #_ REGISTER_OUT r4 0x0000000180000000

test_rlw_lowering_rlwinm_extrwi:
  # extrwi 4,25 -> shr+mask: r3.u64 = (r4.u32 >> 3) & 0xF;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF00000030
  rlwinm r3, r4, 29, 28, 31
  blr
  #_ REGISTER_OUT r3 0x0000000000000006
  #_ REGISTER_OUT r4 0xFFFFFFFF00000030

test_rlw_lowering_rlwinm_shr_mask:
  # rlwinm 24,8,15 -> shr+mask: r3.u64 = (r4.u32 >> 8) & 0xFF0000;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xA5A5A5A512345678
  rlwinm r3, r4, 24, 8, 15
  blr
  #_ REGISTER_OUT r3 0x0000000000120000
  #_ REGISTER_OUT r4 0xA5A5A5A512345678

test_rlw_lowering_rlwinm_rotate_mask:
  # rlwinm 8,16,27 -> rotate+mask: r3.u64 = __builtin_rotateleft32(r4.u32, 8) & 0xFFF0;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  rlwinm r3, r4, 8, 16, 27
  blr
  #_ REGISTER_OUT r3 0x0000000000007810
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678

test_rlw_lowering_rlwinm_rotate_mask_bit31:
  # rlwinm 31,0,15 -> rotate+mask: r3.u64 = __builtin_rotateleft32(r4.u32, 31) & 0xFFFF0000;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF00000001
  rlwinm r3, r4, 31, 0, 15
  blr
  #_ REGISTER_OUT r3 0x0000000080000000
  #_ REGISTER_OUT r4 0xFFFFFFFF00000001

test_rlw_lowering_rlwinm_wrap:
  # rlwinm 8,28,3 -> generic form, mask wraps
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  rlwinm r3, r4, 8, 28, 3
  blr
  #_ REGISTER_OUT r3 0x3456781230000002
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678

test_rlw_lowering_rlwinm_alias:
  # slwi 4, rA == rS -> shl: r4.u64 = r4.u32 << 4;
  #_ REGISTER_IN r4 0xFFFFFFFF87654321
  rlwinm r4, r4, 4, 0, 27
  blr
  #_ REGISTER_OUT r4 0x0000000076543210

test_rlw_lowering_rlwinm_record:
  # slwi. 4 -> shl: r3.u64 = r4.u32 << 4;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF01234567
  rlwinm. r3, r4, 4, 0, 27
  mfcr r12
  blr
  #_ REGISTER_OUT r3 0x0000000012345670
  #_ REGISTER_OUT r4 0xFFFFFFFF01234567
  #_ REGISTER_OUT r12 0x0000000040000000

test_rlw_lowering_rlwinm_record_zero:
  # rlwinm. 0,0,3 -> mask: r3.u64 = r4.u32 & 0xF0000000;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF0FFFFFFF
  rlwinm. r3, r4, 0, 0, 3
  mfcr r12
  blr
  #_ REGISTER_OUT r3 0x0000000000000000
  #_ REGISTER_OUT r4 0xFFFFFFFF0FFFFFFF
  #_ REGISTER_OUT r12 0x0000000020000000

test_rlw_lowering_rlwimi_shl:
  # rlwimi 2,0,29 -> shl: r6.u64 = (r4.u32 << 2) | (r6.u64 & 0xFFFFFFFF00000003);
  #_ REGISTER_IN r4 0xCAFEBABE90003000
  #_ REGISTER_IN r6 0xDEADBEEF00000003
  rlwimi r6, r4, 2, 0, 29
  blr
  #_ REGISTER_OUT r4 0xCAFEBABE90003000
  #_ REGISTER_OUT r6 0xDEADBEEF4000C003

test_rlw_lowering_rlwimi_insrwi:
  # insrwi 8,16 -> shl+mask: r6.u64 = ((r4.u32 << 8) & 0xFF00) | (r6.u64 & 0xFFFFFFFFFFFF00FF);
  #_ REGISTER_IN r4 0xFFFFFFFF000000AB
  #_ REGISTER_IN r6 0x0123456789ABCDEF
  rlwimi r6, r4, 8, 16, 23
  blr
  #_ REGISTER_OUT r4 0xFFFFFFFF000000AB
  #_ REGISTER_OUT r6 0x0123456789ABABEF

test_rlw_lowering_rlwimi_inslwi:
  # inslwi 8,16 -> shr+mask: r6.u64 = ((r4.u32 >> 16) & 0xFF00) | (r6.u64 & 0xFFFFFFFFFFFF00FF);
  #_ REGISTER_IN r4 0x555555553C000000
  #_ REGISTER_IN r6 0x0123456789ABCDEF
  rlwimi r6, r4, 16, 16, 23
  blr
  #_ REGISTER_OUT r4 0x555555553C000000
  #_ REGISTER_OUT r6 0x0123456789AB3CEF

test_rlw_lowering_rlwimi_mask:
  # rlwimi 0,8,15 -> mask: r6.u64 = (r4.u32 & 0xFF0000) | (r6.u64 & 0xFFFFFFFFFF00FFFF);
  #_ REGISTER_IN r4 0xFFFFFFFF00AA0000
  #_ REGISTER_IN r6 0xDEADBEEF11223344
  rlwimi r6, r4, 0, 8, 15
  blr
  #_ REGISTER_OUT r4 0xFFFFFFFF00AA0000
  #_ REGISTER_OUT r6 0xDEADBEEF11AA3344

test_rlw_lowering_rlwimi_shl_16:
  # rlwimi 16,0,15 -> shl: r6.u64 = (r4.u32 << 16) | (r6.u64 & 0xFFFFFFFF0000FFFF);
  #_ REGISTER_IN r4 0x111111110000BEEF
  #_ REGISTER_IN r6 0x22222222DEAD1234
  rlwimi r6, r4, 16, 0, 15
  blr
  #_ REGISTER_OUT r4 0x111111110000BEEF
  #_ REGISTER_OUT r6 0x22222222BEEF1234

test_rlw_lowering_rlwimi_shr:
  # rlwimi 24,8,31 -> shr: r6.u64 = (r4.u32 >> 8) | (r6.u64 & 0xFFFFFFFFFF000000);
  #_ REGISTER_IN r4 0xFFFFFFFFAABBCCDD
  #_ REGISTER_IN r6 0x8000000077777777
  rlwimi r6, r4, 24, 8, 31
  blr
  #_ REGISTER_OUT r4 0xFFFFFFFFAABBCCDD
  #_ REGISTER_OUT r6 0x8000000077AABBCC

test_rlw_lowering_rlwimi_rotate_mask:
  # rlwimi 8,16,27 -> rotate+mask: r6.u64 = (__builtin_rotateleft32(r4.u32, 8) & 0xFFF0) | (r6.u64 & 0xFFFFFFFFFFFF000F);
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  #_ REGISTER_IN r6 0xCAFEBABE00000000
  rlwimi r6, r4, 8, 16, 27
  blr
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678
  #_ REGISTER_OUT r6 0xCAFEBABE00007810

test_rlw_lowering_rlwimi_copy:
  # rlwimi 0,0,31 -> copy: r6.u64 = (r4.u32) | (r6.u64 & 0xFFFFFFFF00000000);
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  #_ REGISTER_IN r6 0xABCDEF0011111111
  rlwimi r6, r4, 0, 0, 31
  blr
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678
  #_ REGISTER_OUT r6 0xABCDEF0012345678

test_rlw_lowering_rlwimi_rotate:
  # rlwimi 8,0,31 -> rotate: r6.u64 = (__builtin_rotateleft32(r4.u32, 8)) | (r6.u64 & 0xFFFFFFFF00000000);
  #_ REGISTER_IN r4 0x7777777712345678
  #_ REGISTER_IN r6 0xFFFFFFFFFFFFFFFF
  rlwimi r6, r4, 8, 0, 31
  blr
  #_ REGISTER_OUT r4 0x7777777712345678
  #_ REGISTER_OUT r6 0xFFFFFFFF34567812

test_rlw_lowering_rlwimi_wrap:
  # rlwimi 8,28,3 -> generic form, mask wraps
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  #_ REGISTER_IN r6 0xDEADBEEFCAFEBABE
  rlwimi r6, r4, 8, 28, 3
  blr
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678
  #_ REGISTER_OUT r6 0x345678123AFEBAB2

test_rlw_lowering_rlwimi_alias:
  # rlwimi 16,0,15, rA == rS -> shl: r4.u64 = (r4.u32 << 16) | (r4.u64 & 0xFFFFFFFF0000FFFF);
  #_ REGISTER_IN r4 0xDEADBEEF0000CAFE
  rlwimi r4, r4, 16, 0, 15
  blr
  #_ REGISTER_OUT r4 0xDEADBEEFCAFECAFE

test_rlw_lowering_rlwimi_record:
  # rlwimi. 4,0,27 -> shl: r6.u64 = (r4.u32 << 4) | (r6.u64 & 0xFFFFFFFF0000000F);
  #_ REGISTER_IN r4 0xFFFFFFFFF1234567
  #_ REGISTER_IN r6 0x000000010000000F
  rlwimi. r6, r4, 4, 0, 27
  mfcr r12
  blr
  #_ REGISTER_OUT r4 0xFFFFFFFFF1234567
  #_ REGISTER_OUT r6 0x000000011234567F
  #_ REGISTER_OUT r12 0x0000000040000000

test_rlw_lowering_rlwnm_rb_40:
  # rB = 0x28 -> rotate+mask: r3.u64 = __builtin_rotateleft32(r4.u32, r5.u8 & 0x1F) & 0xFFF0;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  #_ REGISTER_IN r5 0x0000000000000028
  rlwnm r3, r4, r5, 16, 27
  blr
  #_ REGISTER_OUT r3 0x0000000000007810
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678
  #_ REGISTER_OUT r5 0x0000000000000028

test_rlw_lowering_rlwnm_rb_garbage:
  # rB = 0xFFFFFFFF000000F8 -> rotate+mask: r3.u64 = __builtin_rotateleft32(r4.u32, r5.u8 & 0x1F) & 0xFFFFFF;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xA5A5A5A512345678
  #_ REGISTER_IN r5 0xFFFFFFFF000000F8
  rlwnm r3, r4, r5, 8, 31
  blr
  #_ REGISTER_OUT r3 0x0000000000123456
  #_ REGISTER_OUT r4 0xA5A5A5A512345678
  #_ REGISTER_OUT r5 0xFFFFFFFF000000F8

test_rlw_lowering_rlwnm_rb_32:
  # rB = 32 -> rotate+mask: r3.u64 = __builtin_rotateleft32(r4.u32, r5.u8 & 0x1F) & 0xFFFFFF0;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  #_ REGISTER_IN r5 0x0000000000000020
  rlwnm r3, r4, r5, 4, 27
  blr
  #_ REGISTER_OUT r3 0x0000000002345670
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678
  #_ REGISTER_OUT r5 0x0000000000000020

test_rlw_lowering_rlwnm_rb_63:
  # rB = 63 -> rotate+mask: r3.u64 = __builtin_rotateleft32(r4.u32, r5.u8 & 0x1F) & 0xFFFF0000;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF00000003
  #_ REGISTER_IN r5 0x000000000000003F
  rlwnm r3, r4, r5, 0, 15
  blr
  #_ REGISTER_OUT r3 0x0000000080000000
  #_ REGISTER_OUT r4 0xFFFFFFFF00000003
  #_ REGISTER_OUT r5 0x000000000000003F

test_rlw_lowering_rlwnm_wrap:
  # rlwnm 28,3 -> generic form, mask wraps
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF12345678
  #_ REGISTER_IN r5 0x0000000000000048
  rlwnm r3, r4, r5, 28, 3
  blr
  #_ REGISTER_OUT r3 0x3456781230000002
  #_ REGISTER_OUT r4 0xFFFFFFFF12345678
  #_ REGISTER_OUT r5 0x0000000000000048

test_rlw_lowering_rlwnm_alias:
  # rA == rS == rB -> rotate+mask: r5.u64 = __builtin_rotateleft32(r5.u32, r5.u8 & 0x1F) & 0xFFF0;
  #_ REGISTER_IN r5 0xFFFFFFFF12345668
  rlwnm r5, r5, r5, 16, 27
  blr
  #_ REGISTER_OUT r5 0x0000000000006810

test_rlw_lowering_rlwnm_record:
  # rlwnm. 0,27 -> rotate+mask: r3.u64 = __builtin_rotateleft32(r4.u32, r5.u8 & 0x1F) & 0xFFFFFFF0;
  #_ REGISTER_IN r3 0xDEADBEEFDEADBEEF
  #_ REGISTER_IN r4 0xFFFFFFFF01234567
  #_ REGISTER_IN r5 0x0000000000000024
  rlwnm. r3, r4, r5, 0, 27
  mfcr r12
  blr
  #_ REGISTER_OUT r3 0x0000000012345670
  #_ REGISTER_OUT r4 0xFFFFFFFF01234567
  #_ REGISTER_OUT r5 0x0000000000000024
  #_ REGISTER_OUT r12 0x0000000040000000

