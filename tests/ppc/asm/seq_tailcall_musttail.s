test_tailcall_target:
  addi r3, r3, 5
  blr

test_tailcall_uncond:
  #_ REGISTER_IN r3 0x10
  addi r3, r3, 1
  b test_tailcall_target
  #_ REGISTER_OUT r3 0x16

test_tailcall_chain:
  #_ REGISTER_IN r3 0x100
  addi r3, r3, 2
  b test_tailcall_uncond
  #_ REGISTER_OUT r3 0x108

test_tailcall_cond_taken:
  #_ REGISTER_IN r3 0
  #_ REGISTER_IN r4 0x20
  cmpwi cr6, r3, 0
  mr r3, r4
  beq cr6, test_tailcall_target
  li r3, 100
  blr
  #_ REGISTER_OUT r3 0x25
  #_ REGISTER_OUT r4 0x20

test_tailcall_cond_not_taken:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN r4 0x20
  cmpwi cr6, r3, 0
  mr r3, r4
  beq cr6, test_tailcall_target
  li r3, 100
  blr
  #_ REGISTER_OUT r3 100
  #_ REGISTER_OUT r4 0x20
