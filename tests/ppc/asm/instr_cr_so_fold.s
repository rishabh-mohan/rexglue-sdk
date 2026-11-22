# XER.SO in compare results (the cr_compare_so_zero option): mtxer sets XER.SO, then
# cmpw, cmpwi and andi. write CR fields and mfcr reads the whole CR into r3.
# Each field a compare writes must carry SO (field bits: LT 8, GT 4, EQ 2, SO 1):
#   cmpw cr1, r4, r5 with r4 = 5, r5 = 7: LT|SO = 0x9, CR mask 0x0F000000
#   cmpwi cr6, r4, 5: EQ|SO = 0x3, CR mask 0x000000F0
#   andi. r6, r4, 4 gives r6 = 4: cr0 GT|SO = 0x5, CR mask 0xF0000000
#   CR = 0x59000030 with SO set, 0x48000020 with SO clear
# test_cr_so_fold_same: mtxer and the compares in one function.
# test_cr_so_fold_call: mtxer in the test function, the compares in test_cr_so_fold_callee (bl).
#   Under xer_as_local XER is a per-function local that a call does not carry, so this test
#   gives SO = 0 there by design; leave it out of runs with xer_as_local.
# test_cr_so_fold_clear: control, mtxer with SO clear.
# test_cr_so_fold_callee has no annotations, so it is not a test. Its label needs the test_
# prefix (recompile-tests makes functions only from test_ symbols) and it stays last: the
# test parser reads the next label's annotations as its own and that test would be dropped.

test_cr_so_fold_same:
  #_ REGISTER_IN r4 5
  #_ REGISTER_IN r5 7
  lis r11, 0x8000
  mtxer r11
  cmpw cr1, r4, r5
  cmpwi cr6, r4, 5
  andi. r6, r4, 4
  mfcr r3
  blr
  #_ REGISTER_OUT r3 0x59000030
  #_ REGISTER_OUT r6 4

test_cr_so_fold_call:
  #_ REGISTER_IN r4 5
  #_ REGISTER_IN r5 7
  mflr r12
  lis r11, 0x8000
  mtxer r11
  bl test_cr_so_fold_callee
  mtlr r12
  blr
  #_ REGISTER_OUT r3 0x59000030
  #_ REGISTER_OUT r6 4

test_cr_so_fold_clear:
  #_ REGISTER_IN r4 5
  #_ REGISTER_IN r5 7
  li r11, 0
  mtxer r11
  cmpw cr1, r4, r5
  cmpwi cr6, r4, 5
  andi. r6, r4, 4
  mfcr r3
  blr
  #_ REGISTER_OUT r3 0x48000020
  #_ REGISTER_OUT r6 4

test_cr_so_fold_callee:
  cmpw cr1, r4, r5
  cmpwi cr6, r4, 5
  andi. r6, r4, 4
  mfcr r3
  blr
