; Simple test of the select instruction.  The CHECK lines are only
; checking for basic instruction patterns that should be present
; regardless of the optimization level, so there are no special OPTM1
; match lines.

; RUN: %llvm2ice -O2 --verbose none %s | FileCheck %s
; RUN: %llvm2ice -Om1 --verbose none %s | FileCheck %s
; RUN: %llvm2ice -O2 --verbose none %s \
; RUN:     | llvm-mc -triple=i686-none-nacl -x86-asm-syntax=intel -filetype=obj
; RUN: %llvm2ice -Om1 --verbose none %s \
; RUN:     | llvm-mc -triple=i686-none-nacl -x86-asm-syntax=intel -filetype=obj
; RUN: %llvm2ice --verbose none %s | FileCheck --check-prefix=ERRORS %s
; RUN: %llvm2iceinsts %s | %szdiff %s | FileCheck --check-prefix=DUMP %s
; RUN: %llvm2iceinsts --pnacl %s | %szdiff %s \
; RUN:                           | FileCheck --check-prefix=DUMP %s

define void @testSelect(i32 %a, i32 %b) {
entry:
  %cmp = icmp slt i32 %a, %b
  %cond = select i1 %cmp, i32 %a, i32 %b
  tail call void @useInt(i32 %cond)
  %cmp1 = icmp sgt i32 %a, %b
  %cond2 = select i1 %cmp1, i32 10, i32 20
  tail call void @useInt(i32 %cond2)
  ret void
}

declare void @useInt(i32)

; CHECK:      .globl testSelect
; CHECK:      cmp
; CHECK:      cmp
; CHECK:      call useInt
; CHECK:      cmp
; CHECK:      cmp
; CHECK:      call useInt
; CHECK:      ret

; Check for valid addressing mode in the cmp instruction when the
; operand is an immediate.
define i32 @testSelectImm32(i32 %a, i32 %b) {
entry:
  %cond = select i1 0, i32 %a, i32 %b
  ret i32 %cond
}
; CHECK-LABEL: testSelectImm32
; CHECK-NOT: cmp {{[0-9]+}},

; Check for valid addressing mode in the cmp instruction when the
; operand is an immediate.  There is a different x86-32 lowering
; sequence for 64-bit operands.
define i64 @testSelectImm64(i64 %a, i64 %b) {
entry:
  %cond = select i1 1, i64 %a, i64 %b
  ret i64 %cond
}
; CHECK-LABEL: testSelectImm64
; CHECK-NOT: cmp {{[0-9]+}},

; ERRORS-NOT: ICE translation error
; DUMP-NOT: SZ
