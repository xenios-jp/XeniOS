# db16cyc (or r31, r31, r31) is a spin-wait hint with no architectural
# effect. A run of them in a loop makes the a64 backend release the core
# through a host call on the second consecutive delay; guest state, including
# the vector and scalar FP modes around that call, must be unaffected.
test_db16cyc_1:
  #_ REGISTER_IN r4 0x0000000012345678
  #_ REGISTER_IN r31 0xFFFFFFFFFFFFFFFF
  or r31, r31, r31
  blr
  #_ REGISTER_OUT r4 0x0000000012345678
  #_ REGISTER_OUT r31 0xFFFFFFFFFFFFFFFF

test_db16cyc_2_run:
  #_ REGISTER_IN r5 0x00000000CAFEBABE
  #_ REGISTER_IN r31 0x0000000000000000
  or r31, r31, r31
  or r31, r31, r31
  or r31, r31, r31
  or r31, r31, r31
  blr
  #_ REGISTER_OUT r5 0x00000000CAFEBABE
  #_ REGISTER_OUT r31 0x0000000000000000

test_db16cyc_3_spin_loop:
  #_ REGISTER_IN r3 16
  #_ REGISTER_IN v3 [3F800000, 3F800000, 3F800000, 3F800000]
  #_ REGISTER_IN v4 [3F800000, 3F800000, 3F800000, 3F800000]
  #_ REGISTER_IN f1 0x0000000000000001
  #_ REGISTER_IN f2 0x0000000000000000
.db16cyc_3_loop:
  vaddfp v3, v3, v4
  or r31, r31, r31
  addic. r3, r3, -1
  bne .db16cyc_3_loop
  fadd f3, f1, f2
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT v3 [41880000, 41880000, 41880000, 41880000]
  #_ REGISTER_OUT f3 0x0000000000000001
