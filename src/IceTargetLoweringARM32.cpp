//===- subzero/src/IceTargetLoweringARM32.cpp - ARM32 lowering ------------===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the TargetLoweringARM32 class, which consists almost
/// entirely of the lowering sequence for each high-level instruction.
///
//===----------------------------------------------------------------------===//
#include "IceTargetLoweringARM32.h"

#include "IceCfg.h"
#include "IceCfgNode.h"
#include "IceClFlags.h"
#include "IceDefs.h"
#include "IceELFObjectWriter.h"
#include "IceGlobalInits.h"
#include "IceInstARM32.def"
#include "IceInstARM32.h"
#include "IceInstVarIter.h"
#include "IceLiveness.h"
#include "IceOperand.h"
#include "IcePhiLoweringImpl.h"
#include "IceRegistersARM32.h"
#include "IceTargetLoweringARM32.def"
#include "IceUtils.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Ice {

namespace {

// The following table summarizes the logic for lowering the icmp instruction
// for i32 and narrower types. Each icmp condition has a clear mapping to an
// ARM32 conditional move instruction.

const struct TableIcmp32_ {
  CondARM32::Cond Mapping;
} TableIcmp32[] = {
#define X(val, is_signed, swapped64, C_32, C1_64, C2_64)                       \
  { CondARM32::C_32 }                                                          \
  ,
    ICMPARM32_TABLE
#undef X
};

// The following table summarizes the logic for lowering the icmp instruction
// for the i64 type. Two conditional moves are needed for setting to 1 or 0.
// The operands may need to be swapped, and there is a slight difference for
// signed vs unsigned (comparing hi vs lo first, and using cmp vs sbc).
const struct TableIcmp64_ {
  bool IsSigned;
  bool Swapped;
  CondARM32::Cond C1, C2;
} TableIcmp64[] = {
#define X(val, is_signed, swapped64, C_32, C1_64, C2_64)                       \
  { is_signed, swapped64, CondARM32::C1_64, CondARM32::C2_64 }                 \
  ,
    ICMPARM32_TABLE
#undef X
};

CondARM32::Cond getIcmp32Mapping(InstIcmp::ICond Cond) {
  size_t Index = static_cast<size_t>(Cond);
  assert(Index < llvm::array_lengthof(TableIcmp32));
  return TableIcmp32[Index].Mapping;
}

// In some cases, there are x-macros tables for both high-level and low-level
// instructions/operands that use the same enum key value. The tables are kept
// separate to maintain a proper separation between abstraction layers. There
// is a risk that the tables could get out of sync if enum values are reordered
// or if entries are added or deleted. The following anonymous namespaces use
// static_asserts to ensure everything is kept in sync.

// Validate the enum values in ICMPARM32_TABLE.
namespace {
// Define a temporary set of enum values based on low-level table entries.
enum _icmp_ll_enum {
#define X(val, signed, swapped64, C_32, C1_64, C2_64) _icmp_ll_##val,
  ICMPARM32_TABLE
#undef X
      _num
};
// Define a set of constants based on high-level table entries.
#define X(tag, str) static constexpr int _icmp_hl_##tag = InstIcmp::tag;
ICEINSTICMP_TABLE
#undef X
// Define a set of constants based on low-level table entries, and ensure the
// table entry keys are consistent.
#define X(val, signed, swapped64, C_32, C1_64, C2_64)                          \
  static_assert(                                                               \
      _icmp_ll_##val == _icmp_hl_##val,                                        \
      "Inconsistency between ICMPARM32_TABLE and ICEINSTICMP_TABLE: " #val);
ICMPARM32_TABLE
#undef X
// Repeat the static asserts with respect to the high-level table entries in
// case the high-level table has extra entries.
#define X(tag, str)                                                            \
  static_assert(                                                               \
      _icmp_hl_##tag == _icmp_ll_##tag,                                        \
      "Inconsistency between ICMPARM32_TABLE and ICEINSTICMP_TABLE: " #tag);
ICEINSTICMP_TABLE
#undef X
} // end of anonymous namespace

// Stack alignment
const uint32_t ARM32_STACK_ALIGNMENT_BYTES = 16;

// Value is in bytes. Return Value adjusted to the next highest multiple of the
// stack alignment.
uint32_t applyStackAlignment(uint32_t Value) {
  return Utils::applyAlignment(Value, ARM32_STACK_ALIGNMENT_BYTES);
}

// Value is in bytes. Return Value adjusted to the next highest multiple of the
// stack alignment required for the given type.
uint32_t applyStackAlignmentTy(uint32_t Value, Type Ty) {
  // Use natural alignment, except that normally (non-NaCl) ARM only aligns
  // vectors to 8 bytes.
  // TODO(jvoung): Check this ...
  size_t typeAlignInBytes = typeWidthInBytes(Ty);
  if (isVectorType(Ty))
    typeAlignInBytes = 8;
  return Utils::applyAlignment(Value, typeAlignInBytes);
}

// Conservatively check if at compile time we know that the operand is
// definitely a non-zero integer.
bool isGuaranteedNonzeroInt(const Operand *Op) {
  if (auto *Const = llvm::dyn_cast_or_null<ConstantInteger32>(Op)) {
    return Const->getValue() != 0;
  }
  return false;
}

} // end of anonymous namespace

TargetARM32Features::TargetARM32Features(const ClFlags &Flags) {
  static_assert(
      (ARM32InstructionSet::End - ARM32InstructionSet::Begin) ==
          (TargetInstructionSet::ARM32InstructionSet_End -
           TargetInstructionSet::ARM32InstructionSet_Begin),
      "ARM32InstructionSet range different from TargetInstructionSet");
  if (Flags.getTargetInstructionSet() !=
      TargetInstructionSet::BaseInstructionSet) {
    InstructionSet = static_cast<ARM32InstructionSet>(
        (Flags.getTargetInstructionSet() -
         TargetInstructionSet::ARM32InstructionSet_Begin) +
        ARM32InstructionSet::Begin);
  }
}

TargetARM32::TargetARM32(Cfg *Func)
    : TargetLowering(Func), CPUFeatures(Func->getContext()->getFlags()) {}

void TargetARM32::staticInit() {
  // Limit this size (or do all bitsets need to be the same width)???
  llvm::SmallBitVector IntegerRegisters(RegARM32::Reg_NUM);
  llvm::SmallBitVector I64PairRegisters(RegARM32::Reg_NUM);
  llvm::SmallBitVector Float32Registers(RegARM32::Reg_NUM);
  llvm::SmallBitVector Float64Registers(RegARM32::Reg_NUM);
  llvm::SmallBitVector VectorRegisters(RegARM32::Reg_NUM);
  llvm::SmallBitVector InvalidRegisters(RegARM32::Reg_NUM);
  ScratchRegs.resize(RegARM32::Reg_NUM);
#define X(val, encode, name, scratch, preserved, stackptr, frameptr, isInt,    \
          isI64Pair, isFP32, isFP64, isVec128, alias_init)                     \
  IntegerRegisters[RegARM32::val] = isInt;                                     \
  I64PairRegisters[RegARM32::val] = isI64Pair;                                 \
  Float32Registers[RegARM32::val] = isFP32;                                    \
  Float64Registers[RegARM32::val] = isFP64;                                    \
  VectorRegisters[RegARM32::val] = isVec128;                                   \
  RegisterAliases[RegARM32::val].resize(RegARM32::Reg_NUM);                    \
  for (SizeT RegAlias : alias_init) {                                          \
    assert(!RegisterAliases[RegARM32::val][RegAlias] &&                        \
           "Duplicate alias for " #val);                                       \
    RegisterAliases[RegARM32::val].set(RegAlias);                              \
  }                                                                            \
  RegisterAliases[RegARM32::val].set(RegARM32::val);                           \
  ScratchRegs[RegARM32::val] = scratch;
  REGARM32_TABLE;
#undef X
  TypeToRegisterSet[IceType_void] = InvalidRegisters;
  TypeToRegisterSet[IceType_i1] = IntegerRegisters;
  TypeToRegisterSet[IceType_i8] = IntegerRegisters;
  TypeToRegisterSet[IceType_i16] = IntegerRegisters;
  TypeToRegisterSet[IceType_i32] = IntegerRegisters;
  TypeToRegisterSet[IceType_i64] = I64PairRegisters;
  TypeToRegisterSet[IceType_f32] = Float32Registers;
  TypeToRegisterSet[IceType_f64] = Float64Registers;
  TypeToRegisterSet[IceType_v4i1] = VectorRegisters;
  TypeToRegisterSet[IceType_v8i1] = VectorRegisters;
  TypeToRegisterSet[IceType_v16i1] = VectorRegisters;
  TypeToRegisterSet[IceType_v16i8] = VectorRegisters;
  TypeToRegisterSet[IceType_v8i16] = VectorRegisters;
  TypeToRegisterSet[IceType_v4i32] = VectorRegisters;
  TypeToRegisterSet[IceType_v4f32] = VectorRegisters;
}

namespace {
void copyRegAllocFromInfWeightVariable64On32(const VarList &Vars) {
  for (Variable *Var : Vars) {
    auto *Var64 = llvm::dyn_cast<Variable64On32>(Var);
    if (!Var64) {
      // This is not the variable we are looking for.
      continue;
    }
    assert(Var64->hasReg() || !Var64->mustHaveReg());
    if (!Var64->hasReg()) {
      continue;
    }
    SizeT FirstReg = RegARM32::getI64PairFirstGPRNum(Var->getRegNum());
    // This assumes little endian.
    Variable *Lo = Var64->getLo();
    Variable *Hi = Var64->getHi();
    assert(Lo->hasReg() == Hi->hasReg());
    if (Lo->hasReg()) {
      continue;
    }
    Lo->setRegNum(FirstReg);
    Lo->setMustHaveReg();
    Hi->setRegNum(FirstReg + 1);
    Hi->setMustHaveReg();
  }
}
} // end of anonymous namespace

uint32_t TargetARM32::getCallStackArgumentsSizeBytes(const InstCall *Call) {
  TargetARM32::CallingConv CC;
  size_t OutArgsSizeBytes = 0;
  for (SizeT i = 0, NumArgs = Call->getNumArgs(); i < NumArgs; ++i) {
    Operand *Arg = legalizeUndef(Call->getArg(i));
    Type Ty = Arg->getType();
    if (Ty == IceType_i64) {
      std::pair<int32_t, int32_t> Regs;
      if (CC.I64InRegs(&Regs)) {
        continue;
      }
    } else if (isVectorType(Ty) || isFloatingType(Ty)) {
      int32_t Reg;
      if (CC.FPInReg(Ty, &Reg)) {
        continue;
      }
    } else {
      assert(Ty == IceType_i32);
      int32_t Reg;
      if (CC.I32InReg(&Reg)) {
        continue;
      }
    }

    OutArgsSizeBytes = applyStackAlignmentTy(OutArgsSizeBytes, Ty);
    OutArgsSizeBytes += typeWidthInBytesOnStack(Ty);
  }

  return applyStackAlignment(OutArgsSizeBytes);
}

void TargetARM32::findMaxStackOutArgsSize() {
  // MinNeededOutArgsBytes should be updated if the Target ever creates a
  // high-level InstCall that requires more stack bytes.
  constexpr size_t MinNeededOutArgsBytes = 0;
  MaxOutArgsSizeBytes = MinNeededOutArgsBytes;
  for (CfgNode *Node : Func->getNodes()) {
    Context.init(Node);
    while (!Context.atEnd()) {
      PostIncrLoweringContext PostIncrement(Context);
      Inst *CurInstr = Context.getCur();
      if (auto *Call = llvm::dyn_cast<InstCall>(CurInstr)) {
        SizeT OutArgsSizeBytes = getCallStackArgumentsSizeBytes(Call);
        MaxOutArgsSizeBytes = std::max(MaxOutArgsSizeBytes, OutArgsSizeBytes);
      }
    }
  }
}

void TargetARM32::translateO2() {
  TimerMarker T(TimerStack::TT_O2, Func);

  // TODO(stichnot): share passes with X86?
  // https://code.google.com/p/nativeclient/issues/detail?id=4094
  genTargetHelperCalls();
  findMaxStackOutArgsSize();

  // Do not merge Alloca instructions, and lay out the stack.
  static constexpr bool SortAndCombineAllocas = true;
  Func->processAllocas(SortAndCombineAllocas);
  Func->dump("After Alloca processing");

  if (!Ctx->getFlags().getPhiEdgeSplit()) {
    // Lower Phi instructions.
    Func->placePhiLoads();
    if (Func->hasError())
      return;
    Func->placePhiStores();
    if (Func->hasError())
      return;
    Func->deletePhis();
    if (Func->hasError())
      return;
    Func->dump("After Phi lowering");
  }

  // Address mode optimization.
  Func->getVMetadata()->init(VMK_SingleDefs);
  Func->doAddressOpt();

  // Argument lowering
  Func->doArgLowering();

  // Target lowering. This requires liveness analysis for some parts of the
  // lowering decisions, such as compare/branch fusing. If non-lightweight
  // liveness analysis is used, the instructions need to be renumbered first.
  // TODO: This renumbering should only be necessary if we're actually
  // calculating live intervals, which we only do for register allocation.
  Func->renumberInstructions();
  if (Func->hasError())
    return;

  // TODO: It should be sufficient to use the fastest liveness calculation,
  // i.e. livenessLightweight(). However, for some reason that slows down the
  // rest of the translation. Investigate.
  Func->liveness(Liveness_Basic);
  if (Func->hasError())
    return;
  Func->dump("After ARM32 address mode opt");

  Func->genCode();
  if (Func->hasError())
    return;
  Func->dump("After ARM32 codegen");

  // Register allocation. This requires instruction renumbering and full
  // liveness analysis.
  Func->renumberInstructions();
  if (Func->hasError())
    return;
  Func->liveness(Liveness_Intervals);
  if (Func->hasError())
    return;
  // Validate the live range computations. The expensive validation call is
  // deliberately only made when assertions are enabled.
  assert(Func->validateLiveness());
  // The post-codegen dump is done here, after liveness analysis and associated
  // cleanup, to make the dump cleaner and more useful.
  Func->dump("After initial ARM32 codegen");
  Func->getVMetadata()->init(VMK_All);
  regAlloc(RAK_Global);
  if (Func->hasError())
    return;

  copyRegAllocFromInfWeightVariable64On32(Func->getVariables());
  Func->dump("After linear scan regalloc");

  if (Ctx->getFlags().getPhiEdgeSplit()) {
    Func->advancedPhiLowering();
    Func->dump("After advanced Phi lowering");
  }

  ForbidTemporaryWithoutReg _(this);

  // Stack frame mapping.
  Func->genFrame();
  if (Func->hasError())
    return;
  Func->dump("After stack frame mapping");

  legalizeStackSlots();
  if (Func->hasError())
    return;
  Func->dump("After legalizeStackSlots");

  Func->contractEmptyNodes();
  Func->reorderNodes();

  // Branch optimization. This needs to be done just before code emission. In
  // particular, no transformations that insert or reorder CfgNodes should be
  // done after branch optimization. We go ahead and do it before nop insertion
  // to reduce the amount of work needed for searching for opportunities.
  Func->doBranchOpt();
  Func->dump("After branch optimization");

  // Nop insertion
  if (Ctx->getFlags().shouldDoNopInsertion()) {
    Func->doNopInsertion();
  }
}

void TargetARM32::translateOm1() {
  TimerMarker T(TimerStack::TT_Om1, Func);

  // TODO: share passes with X86?
  genTargetHelperCalls();
  findMaxStackOutArgsSize();

  // Do not merge Alloca instructions, and lay out the stack.
  static constexpr bool DontSortAndCombineAllocas = false;
  Func->processAllocas(DontSortAndCombineAllocas);
  Func->dump("After Alloca processing");

  Func->placePhiLoads();
  if (Func->hasError())
    return;
  Func->placePhiStores();
  if (Func->hasError())
    return;
  Func->deletePhis();
  if (Func->hasError())
    return;
  Func->dump("After Phi lowering");

  Func->doArgLowering();

  Func->genCode();
  if (Func->hasError())
    return;
  Func->dump("After initial ARM32 codegen");

  regAlloc(RAK_InfOnly);
  if (Func->hasError())
    return;

  copyRegAllocFromInfWeightVariable64On32(Func->getVariables());
  Func->dump("After regalloc of infinite-weight variables");

  ForbidTemporaryWithoutReg _(this);

  Func->genFrame();
  if (Func->hasError())
    return;
  Func->dump("After stack frame mapping");

  legalizeStackSlots();
  if (Func->hasError())
    return;
  Func->dump("After legalizeStackSlots");

  // Nop insertion
  if (Ctx->getFlags().shouldDoNopInsertion()) {
    Func->doNopInsertion();
  }
}

uint32_t TargetARM32::getStackAlignment() const {
  return ARM32_STACK_ALIGNMENT_BYTES;
}

bool TargetARM32::doBranchOpt(Inst *I, const CfgNode *NextNode) {
  if (InstARM32Br *Br = llvm::dyn_cast<InstARM32Br>(I)) {
    return Br->optimizeBranch(NextNode);
  }
  return false;
}

const char *RegARM32::RegNames[] = {
#define X(val, encode, name, scratch, preserved, stackptr, frameptr, isInt,    \
          isI64Pair, isFP32, isFP64, isVec128, alias_init)                     \
  name,
    REGARM32_TABLE
#undef X
};

IceString TargetARM32::getRegName(SizeT RegNum, Type Ty) const {
  assert(RegNum < RegARM32::Reg_NUM);
  (void)Ty;
  return RegARM32::RegNames[RegNum];
}

Variable *TargetARM32::getPhysicalRegister(SizeT RegNum, Type Ty) {
  static const Type DefaultType[] = {
#define X(val, encode, name, scratch, preserved, stackptr, frameptr, isInt,    \
          isI64Pair, isFP32, isFP64, isVec128, alias_init)                     \
  (isFP32)                                                                     \
      ? IceType_f32                                                            \
      : ((isFP64) ? IceType_f64 : ((isVec128 ? IceType_v4i32 : IceType_i32))),
      REGARM32_TABLE
#undef X
  };

  assert(RegNum < RegARM32::Reg_NUM);
  if (Ty == IceType_void) {
    assert(RegNum < llvm::array_lengthof(DefaultType));
    Ty = DefaultType[RegNum];
  }
  if (PhysicalRegisters[Ty].empty())
    PhysicalRegisters[Ty].resize(RegARM32::Reg_NUM);
  assert(RegNum < PhysicalRegisters[Ty].size());
  Variable *Reg = PhysicalRegisters[Ty][RegNum];
  if (Reg == nullptr) {
    Reg = Func->makeVariable(Ty);
    Reg->setRegNum(RegNum);
    PhysicalRegisters[Ty][RegNum] = Reg;
    // Specially mark a named physical register as an "argument" so that it is
    // considered live upon function entry.  Otherwise it's possible to get
    // liveness validation errors for saving callee-save registers.
    Func->addImplicitArg(Reg);
    // Don't bother tracking the live range of a named physical register.
    Reg->setIgnoreLiveness();
  }
  return Reg;
}

void TargetARM32::emitJumpTable(const Cfg *Func,
                                const InstJumpTable *JumpTable) const {
  (void)JumpTable;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::emitVariable(const Variable *Var) const {
  if (!BuildDefs::dump())
    return;
  Ostream &Str = Ctx->getStrEmit();
  if (Var->hasReg()) {
    Str << getRegName(Var->getRegNum(), Var->getType());
    return;
  }
  if (Var->mustHaveReg()) {
    llvm::report_fatal_error(
        "Infinite-weight Variable has no register assigned");
  }
  assert(!Var->isRematerializable());
  int32_t Offset = Var->getStackOffset();
  int32_t BaseRegNum = Var->getBaseRegNum();
  if (BaseRegNum == Variable::NoRegister) {
    BaseRegNum = getFrameOrStackReg();
  }
  const Type VarTy = Var->getType();
  Str << "[" << getRegName(BaseRegNum, VarTy);
  if (Offset != 0) {
    Str << ", " << getConstantPrefix() << Offset;
  }
  Str << "]";
}

bool TargetARM32::CallingConv::I64InRegs(std::pair<int32_t, int32_t> *Regs) {
  if (NumGPRRegsUsed >= ARM32_MAX_GPR_ARG)
    return false;
  int32_t RegLo, RegHi;
  // Always start i64 registers at an even register, so this may end up padding
  // away a register.
  NumGPRRegsUsed = Utils::applyAlignment(NumGPRRegsUsed, 2);
  RegLo = RegARM32::Reg_r0 + NumGPRRegsUsed;
  ++NumGPRRegsUsed;
  RegHi = RegARM32::Reg_r0 + NumGPRRegsUsed;
  ++NumGPRRegsUsed;
  // If this bumps us past the boundary, don't allocate to a register and leave
  // any previously speculatively consumed registers as consumed.
  if (NumGPRRegsUsed > ARM32_MAX_GPR_ARG)
    return false;
  Regs->first = RegLo;
  Regs->second = RegHi;
  return true;
}

bool TargetARM32::CallingConv::I32InReg(int32_t *Reg) {
  if (NumGPRRegsUsed >= ARM32_MAX_GPR_ARG)
    return false;
  *Reg = RegARM32::Reg_r0 + NumGPRRegsUsed;
  ++NumGPRRegsUsed;
  return true;
}

bool TargetARM32::CallingConv::FPInReg(Type Ty, int32_t *Reg) {
  if (!VFPRegsFree.any()) {
    return false;
  }

  if (isVectorType(Ty)) {
    // Q registers are declared in reverse order, so RegARM32::Reg_q0 >
    // RegARM32::Reg_q1. Therefore, we need to subtract QRegStart from Reg_q0.
    // Same thing goes for D registers.
    static_assert(RegARM32::Reg_q0 > RegARM32::Reg_q1,
                  "ARM32 Q registers are possibly declared incorrectly.");

    int32_t QRegStart = (VFPRegsFree & ValidV128Regs).find_first();
    if (QRegStart >= 0) {
      VFPRegsFree.reset(QRegStart, QRegStart + 4);
      *Reg = RegARM32::Reg_q0 - (QRegStart / 4);
      return true;
    }
  } else if (Ty == IceType_f64) {
    static_assert(RegARM32::Reg_d0 > RegARM32::Reg_d1,
                  "ARM32 D registers are possibly declared incorrectly.");

    int32_t DRegStart = (VFPRegsFree & ValidF64Regs).find_first();
    if (DRegStart >= 0) {
      VFPRegsFree.reset(DRegStart, DRegStart + 2);
      *Reg = RegARM32::Reg_d0 - (DRegStart / 2);
      return true;
    }
  } else {
    static_assert(RegARM32::Reg_s0 < RegARM32::Reg_s1,
                  "ARM32 S registers are possibly declared incorrectly.");

    assert(Ty == IceType_f32);
    int32_t SReg = VFPRegsFree.find_first();
    assert(SReg >= 0);
    VFPRegsFree.reset(SReg);
    *Reg = RegARM32::Reg_s0 + SReg;
    return true;
  }

  // Parameter allocation failed. From now on, every fp register must be placed
  // on the stack. We clear VFRegsFree in case there are any "holes" from S and
  // D registers.
  VFPRegsFree.clear();
  return false;
}

void TargetARM32::lowerArguments() {
  VarList &Args = Func->getArgs();
  TargetARM32::CallingConv CC;

  // For each register argument, replace Arg in the argument list with the home
  // register. Then generate an instruction in the prolog to copy the home
  // register to the assigned location of Arg.
  Context.init(Func->getEntryNode());
  Context.setInsertPoint(Context.getCur());

  for (SizeT I = 0, E = Args.size(); I < E; ++I) {
    Variable *Arg = Args[I];
    Type Ty = Arg->getType();
    if (Ty == IceType_i64) {
      std::pair<int32_t, int32_t> RegPair;
      if (!CC.I64InRegs(&RegPair))
        continue;
      Variable *RegisterArg = Func->makeVariable(Ty);
      auto *RegisterArg64On32 = llvm::cast<Variable64On32>(RegisterArg);
      if (BuildDefs::dump())
        RegisterArg64On32->setName(Func, "home_reg:" + Arg->getName(Func));
      RegisterArg64On32->initHiLo(Func);
      RegisterArg64On32->setIsArg();
      RegisterArg64On32->getLo()->setRegNum(RegPair.first);
      RegisterArg64On32->getHi()->setRegNum(RegPair.second);
      Arg->setIsArg(false);

      Args[I] = RegisterArg64On32;
      Context.insert(InstAssign::create(Func, Arg, RegisterArg));
      continue;
    } else {
      int32_t RegNum;
      if (isVectorType(Ty) || isFloatingType(Ty)) {
        if (!CC.FPInReg(Ty, &RegNum))
          continue;
      } else {
        assert(Ty == IceType_i32);
        if (!CC.I32InReg(&RegNum))
          continue;
      }
      Variable *RegisterArg = Func->makeVariable(Ty);
      if (BuildDefs::dump()) {
        RegisterArg->setName(Func, "home_reg:" + Arg->getName(Func));
      }
      RegisterArg->setRegNum(RegNum);
      RegisterArg->setIsArg();
      Arg->setIsArg(false);

      Args[I] = RegisterArg;
      Context.insert(InstAssign::create(Func, Arg, RegisterArg));
      continue;
    }
  }
}

// Helper function for addProlog().
//
// This assumes Arg is an argument passed on the stack. This sets the frame
// offset for Arg and updates InArgsSizeBytes according to Arg's width. For an
// I64 arg that has been split into Lo and Hi components, it calls itself
// recursively on the components, taking care to handle Lo first because of the
// little-endian architecture. Lastly, this function generates an instruction
// to copy Arg into its assigned register if applicable.
void TargetARM32::finishArgumentLowering(Variable *Arg, Variable *FramePtr,
                                         size_t BasicFrameOffset,
                                         size_t *InArgsSizeBytes) {
  if (auto *Arg64On32 = llvm::dyn_cast<Variable64On32>(Arg)) {
    Variable *Lo = Arg64On32->getLo();
    Variable *Hi = Arg64On32->getHi();
    finishArgumentLowering(Lo, FramePtr, BasicFrameOffset, InArgsSizeBytes);
    finishArgumentLowering(Hi, FramePtr, BasicFrameOffset, InArgsSizeBytes);
    return;
  }
  Type Ty = Arg->getType();
  assert(Ty != IceType_i64);

  *InArgsSizeBytes = applyStackAlignmentTy(*InArgsSizeBytes, Ty);
  const int32_t ArgStackOffset = BasicFrameOffset + *InArgsSizeBytes;
  *InArgsSizeBytes += typeWidthInBytesOnStack(Ty);

  if (!Arg->hasReg()) {
    Arg->setStackOffset(ArgStackOffset);
    return;
  }

  // If the argument variable has been assigned a register, we need to copy the
  // value from the stack slot.
  Variable *Parameter = Func->makeVariable(Ty);
  Parameter->setMustNotHaveReg();
  Parameter->setStackOffset(ArgStackOffset);
  _mov(Arg, Parameter);
}

Type TargetARM32::stackSlotType() { return IceType_i32; }

void TargetARM32::addProlog(CfgNode *Node) {
  // Stack frame layout:
  //
  // +------------------------+
  // | 1. preserved registers |
  // +------------------------+
  // | 2. padding             |
  // +------------------------+ <--- FramePointer (if used)
  // | 3. global spill area   |
  // +------------------------+
  // | 4. padding             |
  // +------------------------+
  // | 5. local spill area    |
  // +------------------------+
  // | 6. padding             |
  // +------------------------+
  // | 7. allocas (variable)  |
  // +------------------------+
  // | 8. padding             |
  // +------------------------+
  // | 9. out args            |
  // +------------------------+ <--- StackPointer
  //
  // The following variables record the size in bytes of the given areas:
  //  * PreservedRegsSizeBytes: area 1
  //  * SpillAreaPaddingBytes:  area 2
  //  * GlobalsSize:            area 3
  //  * GlobalsAndSubsequentPaddingSize: areas 3 - 4
  //  * LocalsSpillAreaSize:    area 5
  //  * SpillAreaSizeBytes:     areas 2 - 6, and 9
  //  * MaxOutArgsSizeBytes:    area 9
  //
  // Determine stack frame offsets for each Variable without a register
  // assignment.  This can be done as one variable per stack slot.  Or, do
  // coalescing by running the register allocator again with an infinite set of
  // registers (as a side effect, this gives variables a second chance at
  // physical register assignment).
  //
  // A middle ground approach is to leverage sparsity and allocate one block of
  // space on the frame for globals (variables with multi-block lifetime), and
  // one block to share for locals (single-block lifetime).

  Context.init(Node);
  Context.setInsertPoint(Context.getCur());

  llvm::SmallBitVector CalleeSaves =
      getRegisterSet(RegSet_CalleeSave, RegSet_None);
  RegsUsed = llvm::SmallBitVector(CalleeSaves.size());
  VarList SortedSpilledVariables;
  size_t GlobalsSize = 0;
  // If there is a separate locals area, this represents that area. Otherwise
  // it counts any variable not counted by GlobalsSize.
  SpillAreaSizeBytes = 0;
  // If there is a separate locals area, this specifies the alignment for it.
  uint32_t LocalsSlotsAlignmentBytes = 0;
  // The entire spill locations area gets aligned to largest natural alignment
  // of the variables that have a spill slot.
  uint32_t SpillAreaAlignmentBytes = 0;
  // For now, we don't have target-specific variables that need special
  // treatment (no stack-slot-linked SpillVariable type).
  std::function<bool(Variable *)> TargetVarHook = [](Variable *Var) {
    static constexpr bool AssignStackSlot = false;
    static constexpr bool DontAssignStackSlot = !AssignStackSlot;
    if (llvm::isa<Variable64On32>(Var)) {
      return DontAssignStackSlot;
    }
    return AssignStackSlot;
  };

  // Compute the list of spilled variables and bounds for GlobalsSize, etc.
  getVarStackSlotParams(SortedSpilledVariables, RegsUsed, &GlobalsSize,
                        &SpillAreaSizeBytes, &SpillAreaAlignmentBytes,
                        &LocalsSlotsAlignmentBytes, TargetVarHook);
  uint32_t LocalsSpillAreaSize = SpillAreaSizeBytes;
  SpillAreaSizeBytes += GlobalsSize;

  // Add push instructions for preserved registers. On ARM, "push" can push a
  // whole list of GPRs via a bitmask (0-15). Unlike x86, ARM also has
  // callee-saved float/vector registers. The "vpush" instruction can handle a
  // whole list of float/vector registers, but it only handles contiguous
  // sequences of registers by specifying the start and the length.
  VarList GPRsToPreserve;
  GPRsToPreserve.reserve(CalleeSaves.size());
  uint32_t NumCallee = 0;
  size_t PreservedRegsSizeBytes = 0;
  // Consider FP and LR as callee-save / used as needed.
  if (UsesFramePointer) {
    CalleeSaves[RegARM32::Reg_fp] = true;
    assert(RegsUsed[RegARM32::Reg_fp] == false);
    RegsUsed[RegARM32::Reg_fp] = true;
  }
  if (!MaybeLeafFunc) {
    CalleeSaves[RegARM32::Reg_lr] = true;
    RegsUsed[RegARM32::Reg_lr] = true;
  }
  for (SizeT i = 0; i < CalleeSaves.size(); ++i) {
    if (RegARM32::isI64RegisterPair(i)) {
      // We don't save register pairs explicitly. Instead, we rely on the code
      // fake-defing/fake-using each register in the pair.
      continue;
    }
    if (CalleeSaves[i] && RegsUsed[i]) {
      ++NumCallee;
      Variable *PhysicalRegister = getPhysicalRegister(i);
      PreservedRegsSizeBytes +=
          typeWidthInBytesOnStack(PhysicalRegister->getType());
      GPRsToPreserve.push_back(getPhysicalRegister(i));
    }
  }
  Ctx->statsUpdateRegistersSaved(NumCallee);
  if (!GPRsToPreserve.empty())
    _push(GPRsToPreserve);

  // Generate "mov FP, SP" if needed.
  if (UsesFramePointer) {
    Variable *FP = getPhysicalRegister(RegARM32::Reg_fp);
    Variable *SP = getPhysicalRegister(RegARM32::Reg_sp);
    _mov(FP, SP);
    // Keep FP live for late-stage liveness analysis (e.g. asm-verbose mode).
    Context.insert(InstFakeUse::create(Func, FP));
  }

  // Align the variables area. SpillAreaPaddingBytes is the size of the region
  // after the preserved registers and before the spill areas.
  // LocalsSlotsPaddingBytes is the amount of padding between the globals and
  // locals area if they are separate.
  assert(SpillAreaAlignmentBytes <= ARM32_STACK_ALIGNMENT_BYTES);
  assert(LocalsSlotsAlignmentBytes <= SpillAreaAlignmentBytes);
  uint32_t SpillAreaPaddingBytes = 0;
  uint32_t LocalsSlotsPaddingBytes = 0;
  alignStackSpillAreas(PreservedRegsSizeBytes, SpillAreaAlignmentBytes,
                       GlobalsSize, LocalsSlotsAlignmentBytes,
                       &SpillAreaPaddingBytes, &LocalsSlotsPaddingBytes);
  SpillAreaSizeBytes += SpillAreaPaddingBytes + LocalsSlotsPaddingBytes;
  uint32_t GlobalsAndSubsequentPaddingSize =
      GlobalsSize + LocalsSlotsPaddingBytes;

  // Adds the out args space to the stack, and align SP if necessary.
  if (!NeedsStackAlignment) {
    SpillAreaSizeBytes += MaxOutArgsSizeBytes;
  } else {
    uint32_t StackOffset = PreservedRegsSizeBytes;
    uint32_t StackSize = applyStackAlignment(StackOffset + SpillAreaSizeBytes);
    StackSize = applyStackAlignment(StackSize + MaxOutArgsSizeBytes);
    SpillAreaSizeBytes = StackSize - StackOffset;
  }

  // Combine fixed alloca with SpillAreaSize.
  SpillAreaSizeBytes += FixedAllocaSizeBytes;

  // Generate "sub sp, SpillAreaSizeBytes"
  if (SpillAreaSizeBytes) {
    // Use the scratch register if needed to legalize the immediate.
    Operand *SubAmount = legalize(Ctx->getConstantInt32(SpillAreaSizeBytes),
                                  Legal_Reg | Legal_Flex, getReservedTmpReg());
    Variable *SP = getPhysicalRegister(RegARM32::Reg_sp);
    _sub(SP, SP, SubAmount);
    if (FixedAllocaAlignBytes > ARM32_STACK_ALIGNMENT_BYTES) {
      alignRegisterPow2(SP, FixedAllocaAlignBytes);
    }
  }

  Ctx->statsUpdateFrameBytes(SpillAreaSizeBytes);

  // Fill in stack offsets for stack args, and copy args into registers for
  // those that were register-allocated. Args are pushed right to left, so
  // Arg[0] is closest to the stack/frame pointer.
  Variable *FramePtr = getPhysicalRegister(getFrameOrStackReg());
  size_t BasicFrameOffset = PreservedRegsSizeBytes;
  if (!UsesFramePointer)
    BasicFrameOffset += SpillAreaSizeBytes;

  const VarList &Args = Func->getArgs();
  size_t InArgsSizeBytes = 0;
  TargetARM32::CallingConv CC;
  for (Variable *Arg : Args) {
    Type Ty = Arg->getType();
    bool InRegs = false;
    // Skip arguments passed in registers.
    if (isVectorType(Ty) || isFloatingType(Ty)) {
      int32_t DummyReg;
      InRegs = CC.FPInReg(Ty, &DummyReg);
    } else if (Ty == IceType_i64) {
      std::pair<int32_t, int32_t> DummyRegs;
      InRegs = CC.I64InRegs(&DummyRegs);
    } else {
      assert(Ty == IceType_i32);
      int32_t DummyReg;
      InRegs = CC.I32InReg(&DummyReg);
    }
    if (!InRegs)
      finishArgumentLowering(Arg, FramePtr, BasicFrameOffset, &InArgsSizeBytes);
  }

  // Fill in stack offsets for locals.
  assignVarStackSlots(SortedSpilledVariables, SpillAreaPaddingBytes,
                      SpillAreaSizeBytes, GlobalsAndSubsequentPaddingSize,
                      UsesFramePointer);
  this->HasComputedFrame = true;

  if (BuildDefs::dump() && Func->isVerbose(IceV_Frame)) {
    OstreamLocker _(Func->getContext());
    Ostream &Str = Func->getContext()->getStrDump();

    Str << "Stack layout:\n";
    uint32_t SPAdjustmentPaddingSize =
        SpillAreaSizeBytes - LocalsSpillAreaSize -
        GlobalsAndSubsequentPaddingSize - SpillAreaPaddingBytes -
        MaxOutArgsSizeBytes;
    Str << " in-args = " << InArgsSizeBytes << " bytes\n"
        << " preserved registers = " << PreservedRegsSizeBytes << " bytes\n"
        << " spill area padding = " << SpillAreaPaddingBytes << " bytes\n"
        << " globals spill area = " << GlobalsSize << " bytes\n"
        << " globals-locals spill areas intermediate padding = "
        << GlobalsAndSubsequentPaddingSize - GlobalsSize << " bytes\n"
        << " locals spill area = " << LocalsSpillAreaSize << " bytes\n"
        << " SP alignment padding = " << SPAdjustmentPaddingSize << " bytes\n";

    Str << "Stack details:\n"
        << " SP adjustment = " << SpillAreaSizeBytes << " bytes\n"
        << " spill area alignment = " << SpillAreaAlignmentBytes << " bytes\n"
        << " outgoing args size = " << MaxOutArgsSizeBytes << " bytes\n"
        << " locals spill area alignment = " << LocalsSlotsAlignmentBytes
        << " bytes\n"
        << " is FP based = " << UsesFramePointer << "\n";
  }
}

void TargetARM32::addEpilog(CfgNode *Node) {
  InstList &Insts = Node->getInsts();
  InstList::reverse_iterator RI, E;
  for (RI = Insts.rbegin(), E = Insts.rend(); RI != E; ++RI) {
    if (llvm::isa<InstARM32Ret>(*RI))
      break;
  }
  if (RI == E)
    return;

  // Convert the reverse_iterator position into its corresponding (forward)
  // iterator position.
  InstList::iterator InsertPoint = RI.base();
  --InsertPoint;
  Context.init(Node);
  Context.setInsertPoint(InsertPoint);

  Variable *SP = getPhysicalRegister(RegARM32::Reg_sp);
  if (UsesFramePointer) {
    Variable *FP = getPhysicalRegister(RegARM32::Reg_fp);
    // For late-stage liveness analysis (e.g. asm-verbose mode), adding a fake
    // use of SP before the assignment of SP=FP keeps previous SP adjustments
    // from being dead-code eliminated.
    Context.insert(InstFakeUse::create(Func, SP));
    _mov(SP, FP);
  } else {
    // add SP, SpillAreaSizeBytes
    if (SpillAreaSizeBytes) {
      // Use the scratch register if needed to legalize the immediate.
      Operand *AddAmount =
          legalize(Ctx->getConstantInt32(SpillAreaSizeBytes),
                   Legal_Reg | Legal_Flex, getReservedTmpReg());
      _add(SP, SP, AddAmount);
    }
  }

  // Add pop instructions for preserved registers.
  llvm::SmallBitVector CalleeSaves =
      getRegisterSet(RegSet_CalleeSave, RegSet_None);
  VarList GPRsToRestore;
  GPRsToRestore.reserve(CalleeSaves.size());
  // Consider FP and LR as callee-save / used as needed.
  if (UsesFramePointer) {
    CalleeSaves[RegARM32::Reg_fp] = true;
  }
  if (!MaybeLeafFunc) {
    CalleeSaves[RegARM32::Reg_lr] = true;
  }
  // Pop registers in ascending order just like push (instead of in reverse
  // order).
  for (SizeT i = 0; i < CalleeSaves.size(); ++i) {
    if (RegARM32::isI64RegisterPair(i)) {
      continue;
    }

    if (CalleeSaves[i] && RegsUsed[i]) {
      GPRsToRestore.push_back(getPhysicalRegister(i));
    }
  }
  if (!GPRsToRestore.empty())
    _pop(GPRsToRestore);

  if (!Ctx->getFlags().getUseSandboxing())
    return;

  // Change the original ret instruction into a sandboxed return sequence.
  // bundle_lock
  // bic lr, #0xc000000f
  // bx lr
  // bundle_unlock
  // This isn't just aligning to the getBundleAlignLog2Bytes(). It needs to
  // restrict to the lower 1GB as well.
  Operand *RetMask =
      legalize(Ctx->getConstantInt32(0xc000000f), Legal_Reg | Legal_Flex);
  Variable *LR = makeReg(IceType_i32, RegARM32::Reg_lr);
  Variable *RetValue = nullptr;
  if (RI->getSrcSize())
    RetValue = llvm::cast<Variable>(RI->getSrc(0));
  _bundle_lock();
  _bic(LR, LR, RetMask);
  _ret(LR, RetValue);
  _bundle_unlock();
  RI->setDeleted();
}

bool TargetARM32::isLegalMemOffset(Type Ty, int32_t Offset) const {
  constexpr bool ZeroExt = false;
  return OperandARM32Mem::canHoldOffset(Ty, ZeroExt, Offset);
}

Variable *TargetARM32::newBaseRegister(int32_t Offset, Variable *OrigBaseReg) {
  // Legalize will likely need a movw/movt combination, but if the top bits are
  // all 0 from negating the offset and subtracting, we could use that instead.
  bool ShouldSub = (-Offset & 0xFFFF0000) == 0;
  if (ShouldSub)
    Offset = -Offset;
  Operand *OffsetVal = legalize(Ctx->getConstantInt32(Offset),
                                Legal_Reg | Legal_Flex, getReservedTmpReg());
  Variable *ScratchReg = makeReg(IceType_i32, getReservedTmpReg());
  if (ShouldSub)
    _sub(ScratchReg, OrigBaseReg, OffsetVal);
  else
    _add(ScratchReg, OrigBaseReg, OffsetVal);
  return ScratchReg;
}

OperandARM32Mem *TargetARM32::createMemOperand(Type Ty, int32_t Offset,
                                               Variable *OrigBaseReg,
                                               Variable **NewBaseReg,
                                               int32_t *NewBaseOffset) {
  assert(!OrigBaseReg->isRematerializable());
  if (isLegalMemOffset(Ty, Offset)) {
    return OperandARM32Mem::create(
        Func, Ty, OrigBaseReg,
        llvm::cast<ConstantInteger32>(Ctx->getConstantInt32(Offset)),
        OperandARM32Mem::Offset);
  }

  if (*NewBaseReg == nullptr) {
    *NewBaseReg = newBaseRegister(Offset, OrigBaseReg);
    *NewBaseOffset = Offset;
  }

  int32_t OffsetDiff = Offset - *NewBaseOffset;
  if (!isLegalMemOffset(Ty, OffsetDiff)) {
    *NewBaseReg = newBaseRegister(Offset, OrigBaseReg);
    *NewBaseOffset = Offset;
    OffsetDiff = 0;
  }

  assert(!(*NewBaseReg)->isRematerializable());
  return OperandARM32Mem::create(
      Func, Ty, *NewBaseReg,
      llvm::cast<ConstantInteger32>(Ctx->getConstantInt32(OffsetDiff)),
      OperandARM32Mem::Offset);
}

void TargetARM32::legalizeMov(InstARM32Mov *MovInstr, Variable *OrigBaseReg,
                              Variable **NewBaseReg, int32_t *NewBaseOffset) {
  Variable *Dest = MovInstr->getDest();
  assert(Dest != nullptr);
  Type DestTy = Dest->getType();
  assert(DestTy != IceType_i64);

  Operand *Src = MovInstr->getSrc(0);
  Type SrcTy = Src->getType();
  (void)SrcTy;
  assert(SrcTy != IceType_i64);

  if (MovInstr->isMultiDest() || MovInstr->isMultiSource())
    return;

  bool Legalized = false;
  if (!Dest->hasReg()) {
    auto *SrcR = llvm::cast<Variable>(Src);
    assert(SrcR->hasReg());
    assert(!SrcR->isRematerializable());
    const int32_t Offset = Dest->getStackOffset();
    // This is a _mov(Mem(), Variable), i.e., a store.
    _str(SrcR, createMemOperand(DestTy, Offset, OrigBaseReg, NewBaseReg,
                                NewBaseOffset),
         MovInstr->getPredicate());
    // _str() does not have a Dest, so we add a fake-def(Dest).
    Context.insert(InstFakeDef::create(Func, Dest));
    Legalized = true;
  } else if (auto *Var = llvm::dyn_cast<Variable>(Src)) {
    if (Var->isRematerializable()) {
      // Rematerialization arithmetic.
      const int32_t ExtraOffset =
          (static_cast<SizeT>(Var->getRegNum()) == getFrameReg())
              ? getFrameFixedAllocaOffset()
              : 0;

      const int32_t Offset = Var->getStackOffset() + ExtraOffset;
      Operand *OffsetRF = legalize(Ctx->getConstantInt32(Offset),
                                   Legal_Reg | Legal_Flex, Dest->getRegNum());
      _add(Dest, Var, OffsetRF);
      Legalized = true;
    } else {
      if (!Var->hasReg()) {
        const int32_t Offset = Var->getStackOffset();
        _ldr(Dest, createMemOperand(DestTy, Offset, OrigBaseReg, NewBaseReg,
                                    NewBaseOffset),
             MovInstr->getPredicate());
        Legalized = true;
      }
    }
  }

  if (Legalized) {
    if (MovInstr->isDestRedefined()) {
      _set_dest_redefined();
    }
    MovInstr->setDeleted();
  }
}

void TargetARM32::legalizeStackSlots() {
  // If a stack variable's frame offset doesn't fit, convert from:
  //   ldr X, OFF[SP]
  // to:
  //   movw/movt TMP, OFF_PART
  //   add TMP, TMP, SP
  //   ldr X, OFF_MORE[TMP]
  //
  // This is safe because we have reserved TMP, and add for ARM does not
  // clobber the flags register.
  Func->dump("Before legalizeStackSlots");
  assert(hasComputedFrame());
  Variable *OrigBaseReg = getPhysicalRegister(getFrameOrStackReg());
  // Do a fairly naive greedy clustering for now. Pick the first stack slot
  // that's out of bounds and make a new base reg using the architecture's temp
  // register. If that works for the next slot, then great. Otherwise, create a
  // new base register, clobbering the previous base register. Never share a
  // base reg across different basic blocks. This isn't ideal if local and
  // multi-block variables are far apart and their references are interspersed.
  // It may help to be more coordinated about assign stack slot numbers and may
  // help to assign smaller offsets to higher-weight variables so that they
  // don't depend on this legalization.
  for (CfgNode *Node : Func->getNodes()) {
    Context.init(Node);
    Variable *NewBaseReg = nullptr;
    int32_t NewBaseOffset = 0;
    while (!Context.atEnd()) {
      PostIncrLoweringContext PostIncrement(Context);
      Inst *CurInstr = Context.getCur();
      Variable *Dest = CurInstr->getDest();

      // Check if the previous NewBaseReg is clobbered, and reset if needed.
      if ((Dest && NewBaseReg && Dest->hasReg() &&
           Dest->getRegNum() == NewBaseReg->getBaseRegNum()) ||
          llvm::isa<InstFakeKill>(CurInstr)) {
        NewBaseReg = nullptr;
        NewBaseOffset = 0;
      }

      if (auto *MovInstr = llvm::dyn_cast<InstARM32Mov>(CurInstr)) {
        legalizeMov(MovInstr, OrigBaseReg, &NewBaseReg, &NewBaseOffset);
      }
    }
  }
}

Operand *TargetARM32::loOperand(Operand *Operand) {
  assert(Operand->getType() == IceType_i64);
  if (Operand->getType() != IceType_i64)
    return Operand;
  if (auto *Var64On32 = llvm::dyn_cast<Variable64On32>(Operand))
    return Var64On32->getLo();
  if (auto *Const = llvm::dyn_cast<ConstantInteger64>(Operand))
    return Ctx->getConstantInt32(static_cast<uint32_t>(Const->getValue()));
  if (auto *Mem = llvm::dyn_cast<OperandARM32Mem>(Operand)) {
    // Conservatively disallow memory operands with side-effects (pre/post
    // increment) in case of duplication.
    assert(Mem->getAddrMode() == OperandARM32Mem::Offset ||
           Mem->getAddrMode() == OperandARM32Mem::NegOffset);
    Variable *BaseR = legalizeToReg(Mem->getBase());
    if (Mem->isRegReg()) {
      Variable *IndexR = legalizeToReg(Mem->getIndex());
      return OperandARM32Mem::create(Func, IceType_i32, BaseR, IndexR,
                                     Mem->getShiftOp(), Mem->getShiftAmt(),
                                     Mem->getAddrMode());
    } else {
      return OperandARM32Mem::create(Func, IceType_i32, BaseR, Mem->getOffset(),
                                     Mem->getAddrMode());
    }
  }
  llvm_unreachable("Unsupported operand type");
  return nullptr;
}

Operand *TargetARM32::hiOperand(Operand *Operand) {
  assert(Operand->getType() == IceType_i64);
  if (Operand->getType() != IceType_i64)
    return Operand;
  if (auto *Var64On32 = llvm::dyn_cast<Variable64On32>(Operand))
    return Var64On32->getHi();
  if (auto *Const = llvm::dyn_cast<ConstantInteger64>(Operand)) {
    return Ctx->getConstantInt32(
        static_cast<uint32_t>(Const->getValue() >> 32));
  }
  if (auto *Mem = llvm::dyn_cast<OperandARM32Mem>(Operand)) {
    // Conservatively disallow memory operands with side-effects in case of
    // duplication.
    assert(Mem->getAddrMode() == OperandARM32Mem::Offset ||
           Mem->getAddrMode() == OperandARM32Mem::NegOffset);
    const Type SplitType = IceType_i32;
    if (Mem->isRegReg()) {
      // We have to make a temp variable T, and add 4 to either Base or Index.
      // The Index may be shifted, so adding 4 can mean something else. Thus,
      // prefer T := Base + 4, and use T as the new Base.
      Variable *Base = Mem->getBase();
      Constant *Four = Ctx->getConstantInt32(4);
      Variable *NewBase = Func->makeVariable(Base->getType());
      lowerArithmetic(InstArithmetic::create(Func, InstArithmetic::Add, NewBase,
                                             Base, Four));
      Variable *BaseR = legalizeToReg(NewBase);
      Variable *IndexR = legalizeToReg(Mem->getIndex());
      return OperandARM32Mem::create(Func, SplitType, BaseR, IndexR,
                                     Mem->getShiftOp(), Mem->getShiftAmt(),
                                     Mem->getAddrMode());
    } else {
      Variable *Base = Mem->getBase();
      ConstantInteger32 *Offset = Mem->getOffset();
      assert(!Utils::WouldOverflowAdd(Offset->getValue(), 4));
      int32_t NextOffsetVal = Offset->getValue() + 4;
      constexpr bool ZeroExt = false;
      if (!OperandARM32Mem::canHoldOffset(SplitType, ZeroExt, NextOffsetVal)) {
        // We have to make a temp variable and add 4 to either Base or Offset.
        // If we add 4 to Offset, this will convert a non-RegReg addressing
        // mode into a RegReg addressing mode. Since NaCl sandboxing disallows
        // RegReg addressing modes, prefer adding to base and replacing
        // instead. Thus we leave the old offset alone.
        Constant *_4 = Ctx->getConstantInt32(4);
        Variable *NewBase = Func->makeVariable(Base->getType());
        lowerArithmetic(InstArithmetic::create(Func, InstArithmetic::Add,
                                               NewBase, Base, _4));
        Base = NewBase;
      } else {
        Offset =
            llvm::cast<ConstantInteger32>(Ctx->getConstantInt32(NextOffsetVal));
      }
      Variable *BaseR = legalizeToReg(Base);
      return OperandARM32Mem::create(Func, SplitType, BaseR, Offset,
                                     Mem->getAddrMode());
    }
  }
  llvm_unreachable("Unsupported operand type");
  return nullptr;
}

llvm::SmallBitVector TargetARM32::getRegisterSet(RegSetMask Include,
                                                 RegSetMask Exclude) const {
  llvm::SmallBitVector Registers(RegARM32::Reg_NUM);

#define X(val, encode, name, scratch, preserved, stackptr, frameptr, isInt,    \
          isI64Pair, isFP32, isFP64, isVec128, alias_init)                     \
  if (scratch && (Include & RegSet_CallerSave))                                \
    Registers[RegARM32::val] = true;                                           \
  if (preserved && (Include & RegSet_CalleeSave))                              \
    Registers[RegARM32::val] = true;                                           \
  if (stackptr && (Include & RegSet_StackPointer))                             \
    Registers[RegARM32::val] = true;                                           \
  if (frameptr && (Include & RegSet_FramePointer))                             \
    Registers[RegARM32::val] = true;                                           \
  if (scratch && (Exclude & RegSet_CallerSave))                                \
    Registers[RegARM32::val] = false;                                          \
  if (preserved && (Exclude & RegSet_CalleeSave))                              \
    Registers[RegARM32::val] = false;                                          \
  if (stackptr && (Exclude & RegSet_StackPointer))                             \
    Registers[RegARM32::val] = false;                                          \
  if (frameptr && (Exclude & RegSet_FramePointer))                             \
    Registers[RegARM32::val] = false;

  REGARM32_TABLE

#undef X

  return Registers;
}

void TargetARM32::lowerAlloca(const InstAlloca *Inst) {
  // Conservatively require the stack to be aligned. Some stack adjustment
  // operations implemented below assume that the stack is aligned before the
  // alloca. All the alloca code ensures that the stack alignment is preserved
  // after the alloca. The stack alignment restriction can be relaxed in some
  // cases.
  NeedsStackAlignment = true;

  // For default align=0, set it to the real value 1, to avoid any
  // bit-manipulation problems below.
  const uint32_t AlignmentParam = std::max(1u, Inst->getAlignInBytes());

  // LLVM enforces power of 2 alignment.
  assert(llvm::isPowerOf2_32(AlignmentParam));
  assert(llvm::isPowerOf2_32(ARM32_STACK_ALIGNMENT_BYTES));

  const uint32_t Alignment =
      std::max(AlignmentParam, ARM32_STACK_ALIGNMENT_BYTES);
  const bool OverAligned = Alignment > ARM32_STACK_ALIGNMENT_BYTES;
  const bool OptM1 = Ctx->getFlags().getOptLevel() == Opt_m1;
  const bool AllocaWithKnownOffset = Inst->getKnownFrameOffset();
  const bool UseFramePointer =
      hasFramePointer() || OverAligned || !AllocaWithKnownOffset || OptM1;

  if (UseFramePointer)
    setHasFramePointer();

  Variable *SP = getPhysicalRegister(RegARM32::Reg_sp);
  if (OverAligned) {
    alignRegisterPow2(SP, Alignment);
  }

  Variable *Dest = Inst->getDest();
  Operand *TotalSize = Inst->getSizeInBytes();

  if (const auto *ConstantTotalSize =
          llvm::dyn_cast<ConstantInteger32>(TotalSize)) {
    const uint32_t Value =
        Utils::applyAlignment(ConstantTotalSize->getValue(), Alignment);
    // Constant size alloca.
    if (!UseFramePointer) {
      // If we don't need a Frame Pointer, this alloca has a known offset to the
      // stack pointer. We don't need adjust the stack pointer, nor assign any
      // value to Dest, as Dest is rematerializable.
      assert(Dest->isRematerializable());
      FixedAllocaSizeBytes += Value;
      Context.insert(InstFakeDef::create(Func, Dest));
      return;
    }

    // If a frame pointer is required, then we need to store the alloca'd result
    // in Dest.
    Operand *SubAmountRF =
        legalize(Ctx->getConstantInt32(Value), Legal_Reg | Legal_Flex);
    _sub(SP, SP, SubAmountRF);
  } else {
    // Non-constant sizes need to be adjusted to the next highest multiple of
    // the required alignment at runtime.
    TotalSize = legalize(TotalSize, Legal_Reg | Legal_Flex);
    Variable *T = makeReg(IceType_i32);
    _mov(T, TotalSize);
    Operand *AddAmount = legalize(Ctx->getConstantInt32(Alignment - 1));
    _add(T, T, AddAmount);
    alignRegisterPow2(T, Alignment);
    _sub(SP, SP, T);
  }

  // Adds back a few bytes to SP to account for the out args area.
  Variable *T = SP;
  if (MaxOutArgsSizeBytes != 0) {
    T = makeReg(getPointerType());
    Operand *OutArgsSizeRF = legalize(
        Ctx->getConstantInt32(MaxOutArgsSizeBytes), Legal_Reg | Legal_Flex);
    _add(T, SP, OutArgsSizeRF);
  }

  _mov(Dest, T);
}

void TargetARM32::div0Check(Type Ty, Operand *SrcLo, Operand *SrcHi) {
  if (isGuaranteedNonzeroInt(SrcLo) || isGuaranteedNonzeroInt(SrcHi))
    return;
  Variable *SrcLoReg = legalizeToReg(SrcLo);
  switch (Ty) {
  default:
    llvm::report_fatal_error("Unexpected type");
  case IceType_i8:
  case IceType_i16: {
    Operand *ShAmtImm = shAmtImm(32 - getScalarIntBitWidth(Ty));
    Variable *T = makeReg(IceType_i32);
    _lsls(T, SrcLoReg, ShAmtImm);
    Context.insert(InstFakeUse::create(Func, T));
  } break;
  case IceType_i32: {
    _tst(SrcLoReg, SrcLoReg);
    break;
  }
  case IceType_i64: {
    Variable *T = makeReg(IceType_i32);
    _orrs(T, SrcLoReg, legalize(SrcHi, Legal_Reg | Legal_Flex));
    // T isn't going to be used, but we need the side-effect of setting flags
    // from this operation.
    Context.insert(InstFakeUse::create(Func, T));
  }
  }
  InstARM32Label *Label = InstARM32Label::create(Func, this);
  _br(Label, CondARM32::NE);
  _trap();
  Context.insert(Label);
}

void TargetARM32::lowerIDivRem(Variable *Dest, Variable *T, Variable *Src0R,
                               Operand *Src1, ExtInstr ExtFunc,
                               DivInstr DivFunc, const char *DivHelperName,
                               bool IsRemainder) {
  div0Check(Dest->getType(), Src1, nullptr);
  Variable *Src1R = legalizeToReg(Src1);
  Variable *T0R = Src0R;
  Variable *T1R = Src1R;
  if (Dest->getType() != IceType_i32) {
    T0R = makeReg(IceType_i32);
    (this->*ExtFunc)(T0R, Src0R, CondARM32::AL);
    T1R = makeReg(IceType_i32);
    (this->*ExtFunc)(T1R, Src1R, CondARM32::AL);
  }
  if (hasCPUFeature(TargetARM32Features::HWDivArm)) {
    (this->*DivFunc)(T, T0R, T1R, CondARM32::AL);
    if (IsRemainder) {
      Variable *T2 = makeReg(IceType_i32);
      _mls(T2, T, T1R, T0R);
      T = T2;
    }
    _mov(Dest, T);
  } else {
    constexpr SizeT MaxSrcs = 2;
    InstCall *Call = makeHelperCall(DivHelperName, Dest, MaxSrcs);
    Call->addArg(T0R);
    Call->addArg(T1R);
    lowerCall(Call);
  }
  return;
}

TargetARM32::SafeBoolChain
TargetARM32::lowerInt1Arithmetic(const InstArithmetic *Inst) {
  Variable *Dest = Inst->getDest();
  assert(Dest->getType() == IceType_i1);

  // So folding didn't work for Inst. Not a problem: We just need to
  // materialize the Sources, and perform the operation. We create regular
  // Variables (and not infinite-weight ones) because this call might recurse a
  // lot, and we might end up with tons of infinite weight temporaries.
  assert(Inst->getSrcSize() == 2);
  Variable *Src0 = Func->makeVariable(IceType_i1);
  SafeBoolChain Src0Safe = lowerInt1(Src0, Inst->getSrc(0));

  Operand *Src1 = Inst->getSrc(1);
  SafeBoolChain Src1Safe = SBC_Yes;

  if (!llvm::isa<Constant>(Src1)) {
    Variable *Src1V = Func->makeVariable(IceType_i1);
    Src1Safe = lowerInt1(Src1V, Src1);
    Src1 = Src1V;
  }

  Variable *T = makeReg(IceType_i1);
  Src0 = legalizeToReg(Src0);
  Operand *Src1RF = legalize(Src1, Legal_Reg | Legal_Flex);
  switch (Inst->getOp()) {
  default:
    // If this Unreachable is ever executed, add the offending operation to
    // the list of valid consumers.
    llvm::report_fatal_error("Unhandled i1 Op");
  case InstArithmetic::And:
    _and(T, Src0, Src1RF);
    break;
  case InstArithmetic::Or:
    _orr(T, Src0, Src1RF);
    break;
  case InstArithmetic::Xor:
    _eor(T, Src0, Src1RF);
    break;
  }
  _mov(Dest, T);
  return Src0Safe == SBC_Yes && Src1Safe == SBC_Yes ? SBC_Yes : SBC_No;
}

namespace {
// NumericOperands is used during arithmetic/icmp lowering for constant folding.
// It holds the two sources operands, and maintains some state as to whether one
// of them is a constant. If one of the operands is a constant, then it will be
// be stored as the operation's second source, with a bit indicating whether the
// operands were swapped.
//
// The class is split into a base class with operand type-independent methods,
// and a derived, templated class, for each type of operand we want to fold
// constants for:
//
// NumericOperandsBase --> NumericOperands<ConstantFloat>
//                     --> NumericOperands<ConstantDouble>
//                     --> NumericOperands<ConstantInt32>
//
// NumericOperands<ConstantInt32> also exposes helper methods for emitting
// inverted/negated immediates.
class NumericOperandsBase {
  NumericOperandsBase() = delete;
  NumericOperandsBase(const NumericOperandsBase &) = delete;
  NumericOperandsBase &operator=(const NumericOperandsBase &) = delete;

public:
  NumericOperandsBase(Operand *S0, Operand *S1)
      : Src0(NonConstOperand(S0, S1)), Src1(ConstOperand(S0, S1)),
        Swapped(Src0 == S1 && S0 != S1) {
    assert(Src0 != nullptr);
    assert(Src1 != nullptr);
    assert(Src0 != Src1 || S0 == S1);
  }

  bool hasConstOperand() const {
    return llvm::isa<Constant>(Src1) && !llvm::isa<ConstantRelocatable>(Src1);
  }

  bool swappedOperands() const { return Swapped; }

  Variable *src0R(TargetARM32 *Target) const {
    return legalizeToReg(Target, Src0);
  }

  Variable *unswappedSrc0R(TargetARM32 *Target) const {
    return legalizeToReg(Target, Swapped ? Src1 : Src0);
  }

  Operand *src1RF(TargetARM32 *Target) const {
    return legalizeToRegOrFlex(Target, Src1);
  }

  Variable *unswappedSrc1R(TargetARM32 *Target) const {
    return legalizeToReg(Target, Swapped ? Src0 : Src1);
  }

protected:
  Operand *const Src0;
  Operand *const Src1;
  const bool Swapped;

  static Variable *legalizeToReg(TargetARM32 *Target, Operand *Src) {
    return Target->legalizeToReg(Src);
  }

  static Operand *legalizeToRegOrFlex(TargetARM32 *Target, Operand *Src) {
    return Target->legalize(Src,
                            TargetARM32::Legal_Reg | TargetARM32::Legal_Flex);
  }

private:
  static Operand *NonConstOperand(Operand *S0, Operand *S1) {
    if (!llvm::isa<Constant>(S0))
      return S0;
    if (!llvm::isa<Constant>(S1))
      return S1;
    if (llvm::isa<ConstantRelocatable>(S1) &&
        !llvm::isa<ConstantRelocatable>(S0))
      return S1;
    return S0;
  }

  static Operand *ConstOperand(Operand *S0, Operand *S1) {
    if (!llvm::isa<Constant>(S0))
      return S1;
    if (!llvm::isa<Constant>(S1))
      return S0;
    if (llvm::isa<ConstantRelocatable>(S1) &&
        !llvm::isa<ConstantRelocatable>(S0))
      return S0;
    return S1;
  }
};

template <typename C> class NumericOperands : public NumericOperandsBase {
  NumericOperands() = delete;
  NumericOperands(const NumericOperands &) = delete;
  NumericOperands &operator=(const NumericOperands &) = delete;

public:
  NumericOperands(Operand *S0, Operand *S1) : NumericOperandsBase(S0, S1) {
    assert(!hasConstOperand() || llvm::isa<C>(this->Src1));
  }

  typename C::PrimType getConstantValue() const {
    return llvm::cast<C>(Src1)->getValue();
  }
};

using FloatOperands = NumericOperands<ConstantFloat>;
using DoubleOperands = NumericOperands<ConstantDouble>;

class Int32Operands : public NumericOperands<ConstantInteger32> {
  Int32Operands() = delete;
  Int32Operands(const Int32Operands &) = delete;
  Int32Operands &operator=(const Int32Operands &) = delete;

public:
  Int32Operands(Operand *S0, Operand *S1) : NumericOperands(S0, S1) {}

  Operand *unswappedSrc1RShAmtImm(TargetARM32 *Target) const {
    if (!swappedOperands() && hasConstOperand()) {
      return Target->shAmtImm(getConstantValue() & 0x1F);
    }
    return legalizeToReg(Target, Swapped ? Src0 : Src1);
  }

  bool immediateIsFlexEncodable() const {
    uint32_t Rotate, Imm8;
    return OperandARM32FlexImm::canHoldImm(getConstantValue(), &Rotate, &Imm8);
  }

  bool negatedImmediateIsFlexEncodable() const {
    uint32_t Rotate, Imm8;
    return OperandARM32FlexImm::canHoldImm(
        -static_cast<int32_t>(getConstantValue()), &Rotate, &Imm8);
  }

  Operand *negatedSrc1F(TargetARM32 *Target) const {
    return legalizeToRegOrFlex(Target,
                               Target->getCtx()->getConstantInt32(
                                   -static_cast<int32_t>(getConstantValue())));
  }

  bool invertedImmediateIsFlexEncodable() const {
    uint32_t Rotate, Imm8;
    return OperandARM32FlexImm::canHoldImm(
        ~static_cast<uint32_t>(getConstantValue()), &Rotate, &Imm8);
  }

  Operand *invertedSrc1F(TargetARM32 *Target) const {
    return legalizeToRegOrFlex(Target,
                               Target->getCtx()->getConstantInt32(
                                   ~static_cast<uint32_t>(getConstantValue())));
  }
};
} // end of anonymous namespace

void TargetARM32::lowerInt64Arithmetic(InstArithmetic::OpKind Op,
                                       Variable *Dest, Operand *Src0,
                                       Operand *Src1) {
  Int32Operands SrcsLo(loOperand(Src0), loOperand(Src1));
  Int32Operands SrcsHi(hiOperand(Src0), hiOperand(Src1));
  assert(SrcsLo.swappedOperands() == SrcsHi.swappedOperands());
  assert(SrcsLo.hasConstOperand() == SrcsHi.hasConstOperand());

  // These helper-call-involved instructions are lowered in this separate
  // switch. This is because we would otherwise assume that we need to
  // legalize Src0 to Src0RLo and Src0Hi. However, those go unused with
  // helper calls, and such unused/redundant instructions will fail liveness
  // analysis under -Om1 setting.
  switch (Op) {
  default:
    break;
  case InstArithmetic::Udiv:
  case InstArithmetic::Sdiv:
  case InstArithmetic::Urem:
  case InstArithmetic::Srem: {
    // Check for divide by 0 (ARM normally doesn't trap, but we want it to
    // trap for NaCl). Src1Lo and Src1Hi may have already been legalized to a
    // register, which will hide a constant source operand. Instead, check
    // the not-yet-legalized Src1 to optimize-out a divide by 0 check.
    if (!SrcsLo.swappedOperands() && SrcsLo.hasConstOperand()) {
      if (SrcsLo.getConstantValue() == 0 && SrcsHi.getConstantValue() == 0) {
        _trap();
        return;
      }
    } else {
      Operand *Src1Lo = SrcsLo.unswappedSrc1R(this);
      Operand *Src1Hi = SrcsHi.unswappedSrc1R(this);
      div0Check(IceType_i64, Src1Lo, Src1Hi);
    }
    // Technically, ARM has its own aeabi routines, but we can use the
    // non-aeabi routine as well. LLVM uses __aeabi_ldivmod for div, but uses
    // the more standard __moddi3 for rem.
    const char *HelperName = "";
    switch (Op) {
    default:
      llvm::report_fatal_error("Should have only matched div ops.");
      break;
    case InstArithmetic::Udiv:
      HelperName = H_udiv_i64;
      break;
    case InstArithmetic::Sdiv:
      HelperName = H_sdiv_i64;
      break;
    case InstArithmetic::Urem:
      HelperName = H_urem_i64;
      break;
    case InstArithmetic::Srem:
      HelperName = H_srem_i64;
      break;
    }
    constexpr SizeT MaxSrcs = 2;
    InstCall *Call = makeHelperCall(HelperName, Dest, MaxSrcs);
    Call->addArg(Src0);
    Call->addArg(Src1);
    lowerCall(Call);
    return;
  }
  }

  Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
  Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
  Variable *T_Lo = makeReg(DestLo->getType());
  Variable *T_Hi = makeReg(DestHi->getType());

  switch (Op) {
  case InstArithmetic::_num:
    llvm::report_fatal_error("Unknown arithmetic operator");
    return;
  case InstArithmetic::Add: {
    Variable *Src0LoR = SrcsLo.src0R(this);
    Operand *Src1LoRF = SrcsLo.src1RF(this);
    Variable *Src0HiR = SrcsHi.src0R(this);
    Operand *Src1HiRF = SrcsHi.src1RF(this);
    _adds(T_Lo, Src0LoR, Src1LoRF);
    _mov(DestLo, T_Lo);
    _adc(T_Hi, Src0HiR, Src1HiRF);
    _mov(DestHi, T_Hi);
    return;
  }
  case InstArithmetic::And: {
    Variable *Src0LoR = SrcsLo.src0R(this);
    Operand *Src1LoRF = SrcsLo.src1RF(this);
    Variable *Src0HiR = SrcsHi.src0R(this);
    Operand *Src1HiRF = SrcsHi.src1RF(this);
    _and(T_Lo, Src0LoR, Src1LoRF);
    _mov(DestLo, T_Lo);
    _and(T_Hi, Src0HiR, Src1HiRF);
    _mov(DestHi, T_Hi);
    return;
  }
  case InstArithmetic::Or: {
    Variable *Src0LoR = SrcsLo.src0R(this);
    Operand *Src1LoRF = SrcsLo.src1RF(this);
    Variable *Src0HiR = SrcsHi.src0R(this);
    Operand *Src1HiRF = SrcsHi.src1RF(this);
    _orr(T_Lo, Src0LoR, Src1LoRF);
    _mov(DestLo, T_Lo);
    _orr(T_Hi, Src0HiR, Src1HiRF);
    _mov(DestHi, T_Hi);
    return;
  }
  case InstArithmetic::Xor: {
    Variable *Src0LoR = SrcsLo.src0R(this);
    Operand *Src1LoRF = SrcsLo.src1RF(this);
    Variable *Src0HiR = SrcsHi.src0R(this);
    Operand *Src1HiRF = SrcsHi.src1RF(this);
    _eor(T_Lo, Src0LoR, Src1LoRF);
    _mov(DestLo, T_Lo);
    _eor(T_Hi, Src0HiR, Src1HiRF);
    _mov(DestHi, T_Hi);
    return;
  }
  case InstArithmetic::Sub: {
    Variable *Src0LoR = SrcsLo.src0R(this);
    Operand *Src1LoRF = SrcsLo.src1RF(this);
    Variable *Src0HiR = SrcsHi.src0R(this);
    Operand *Src1HiRF = SrcsHi.src1RF(this);
    if (SrcsLo.swappedOperands()) {
      _rsbs(T_Lo, Src0LoR, Src1LoRF);
      _mov(DestLo, T_Lo);
      _rsc(T_Hi, Src0HiR, Src1HiRF);
      _mov(DestHi, T_Hi);
    } else {
      _subs(T_Lo, Src0LoR, Src1LoRF);
      _mov(DestLo, T_Lo);
      _sbc(T_Hi, Src0HiR, Src1HiRF);
      _mov(DestHi, T_Hi);
    }
    return;
  }
  case InstArithmetic::Mul: {
    // GCC 4.8 does:
    // a=b*c ==>
    //   t_acc =(mul) (b.lo * c.hi)
    //   t_acc =(mla) (c.lo * b.hi) + t_acc
    //   t.hi,t.lo =(umull) b.lo * c.lo
    //   t.hi += t_acc
    //   a.lo = t.lo
    //   a.hi = t.hi
    //
    // LLVM does:
    //   t.hi,t.lo =(umull) b.lo * c.lo
    //   t.hi =(mla) (b.lo * c.hi) + t.hi
    //   t.hi =(mla) (b.hi * c.lo) + t.hi
    //   a.lo = t.lo
    //   a.hi = t.hi
    //
    // LLVM's lowering has fewer instructions, but more register pressure:
    // t.lo is live from beginning to end, while GCC delays the two-dest
    // instruction till the end, and kills c.hi immediately.
    Variable *T_Acc = makeReg(IceType_i32);
    Variable *T_Acc1 = makeReg(IceType_i32);
    Variable *T_Hi1 = makeReg(IceType_i32);
    Variable *Src0RLo = SrcsLo.unswappedSrc0R(this);
    Variable *Src0RHi = SrcsHi.unswappedSrc0R(this);
    Variable *Src1RLo = SrcsLo.unswappedSrc1R(this);
    Variable *Src1RHi = SrcsHi.unswappedSrc1R(this);
    _mul(T_Acc, Src0RLo, Src1RHi);
    _mla(T_Acc1, Src1RLo, Src0RHi, T_Acc);
    _umull(T_Lo, T_Hi1, Src0RLo, Src1RLo);
    _add(T_Hi, T_Hi1, T_Acc1);
    _mov(DestLo, T_Lo);
    _mov(DestHi, T_Hi);
    return;
  }
  case InstArithmetic::Shl: {
    if (!SrcsLo.swappedOperands() && SrcsLo.hasConstOperand()) {
      Variable *Src0RLo = SrcsLo.src0R(this);
      // Truncating the ShAmt to [0, 63] because that's what ARM does anyway.
      const int32_t ShAmtImm = SrcsLo.getConstantValue() & 0x3F;
      if (ShAmtImm == 0) {
        _mov(DestLo, Src0RLo);
        _mov(DestHi, SrcsHi.src0R(this));
        return;
      }

      if (ShAmtImm >= 32) {
        if (ShAmtImm == 32) {
          _mov(DestHi, Src0RLo);
        } else {
          Operand *ShAmtOp = shAmtImm(ShAmtImm - 32);
          _lsl(T_Hi, Src0RLo, ShAmtOp);
          _mov(DestHi, T_Hi);
        }

        Operand *_0 =
            legalize(Ctx->getConstantZero(IceType_i32), Legal_Reg | Legal_Flex);
        _mov(T_Lo, _0);
        _mov(DestLo, T_Lo);
        return;
      }

      Variable *Src0RHi = SrcsHi.src0R(this);
      Operand *ShAmtOp = shAmtImm(ShAmtImm);
      Operand *ComplShAmtOp = shAmtImm(32 - ShAmtImm);
      _lsl(T_Hi, Src0RHi, ShAmtOp);
      _orr(T_Hi, T_Hi,
           OperandARM32FlexReg::create(Func, IceType_i32, Src0RLo,
                                       OperandARM32::LSR, ComplShAmtOp));
      _mov(DestHi, T_Hi);

      _lsl(T_Lo, Src0RLo, ShAmtOp);
      _mov(DestLo, T_Lo);
      return;
    }

    // a=b<<c ==>
    // pnacl-llc does:
    // mov     t_b.lo, b.lo
    // mov     t_b.hi, b.hi
    // mov     t_c.lo, c.lo
    // rsb     T0, t_c.lo, #32
    // lsr     T1, t_b.lo, T0
    // orr     t_a.hi, T1, t_b.hi, lsl t_c.lo
    // sub     T2, t_c.lo, #32
    // cmp     T2, #0
    // lslge   t_a.hi, t_b.lo, T2
    // lsl     t_a.lo, t_b.lo, t_c.lo
    // mov     a.lo, t_a.lo
    // mov     a.hi, t_a.hi
    //
    // GCC 4.8 does:
    // sub t_c1, c.lo, #32
    // lsl t_hi, b.hi, c.lo
    // orr t_hi, t_hi, b.lo, lsl t_c1
    // rsb t_c2, c.lo, #32
    // orr t_hi, t_hi, b.lo, lsr t_c2
    // lsl t_lo, b.lo, c.lo
    // a.lo = t_lo
    // a.hi = t_hi
    //
    // These are incompatible, therefore we mimic pnacl-llc.
    // Can be strength-reduced for constant-shifts, but we don't do that for
    // now.
    // Given the sub/rsb T_C, C.lo, #32, one of the T_C will be negative. On
    // ARM, shifts only take the lower 8 bits of the shift register, and
    // saturate to the range 0-32, so the negative value will saturate to 32.
    Operand *_32 = legalize(Ctx->getConstantInt32(32), Legal_Reg | Legal_Flex);
    Operand *_0 =
        legalize(Ctx->getConstantZero(IceType_i32), Legal_Reg | Legal_Flex);
    Variable *T0 = makeReg(IceType_i32);
    Variable *T1 = makeReg(IceType_i32);
    Variable *T2 = makeReg(IceType_i32);
    Variable *TA_Hi = makeReg(IceType_i32);
    Variable *TA_Lo = makeReg(IceType_i32);
    Variable *Src0RLo = SrcsLo.src0R(this);
    Variable *Src0RHi = SrcsHi.unswappedSrc0R(this);
    Variable *Src1RLo = SrcsLo.unswappedSrc1R(this);
    _rsb(T0, Src1RLo, _32);
    _lsr(T1, Src0RLo, T0);
    _orr(TA_Hi, T1, OperandARM32FlexReg::create(Func, IceType_i32, Src0RHi,
                                                OperandARM32::LSL, Src1RLo));
    _sub(T2, Src1RLo, _32);
    _cmp(T2, _0);
    _lsl(TA_Hi, Src0RLo, T2, CondARM32::GE);
    _set_dest_redefined();
    _lsl(TA_Lo, Src0RLo, Src1RLo);
    _mov(DestLo, TA_Lo);
    _mov(DestHi, TA_Hi);
    return;
  }
  case InstArithmetic::Lshr:
  case InstArithmetic::Ashr: {
    const bool ASR = Op == InstArithmetic::Ashr;
    if (!SrcsLo.swappedOperands() && SrcsLo.hasConstOperand()) {
      Variable *Src0RHi = SrcsHi.src0R(this);
      // Truncating the ShAmt to [0, 63] because that's what ARM does anyway.
      const int32_t ShAmt = SrcsLo.getConstantValue() & 0x3F;
      if (ShAmt == 0) {
        _mov(DestHi, Src0RHi);
        _mov(DestLo, SrcsLo.src0R(this));
        return;
      }

      if (ShAmt >= 32) {
        if (ShAmt == 32) {
          _mov(DestLo, Src0RHi);
        } else {
          Operand *ShAmtImm = shAmtImm(ShAmt - 32);
          if (ASR) {
            _asr(T_Lo, Src0RHi, ShAmtImm);
          } else {
            _lsr(T_Lo, Src0RHi, ShAmtImm);
          }
          _mov(DestLo, T_Lo);
        }

        if (ASR) {
          Operand *_31 = shAmtImm(31);
          _asr(T_Hi, Src0RHi, _31);
        } else {
          Operand *_0 = legalize(Ctx->getConstantZero(IceType_i32),
                                 Legal_Reg | Legal_Flex);
          _mov(T_Hi, _0);
        }
        _mov(DestHi, T_Hi);
        return;
      }

      Variable *Src0RLo = SrcsLo.src0R(this);
      Operand *ShAmtImm = shAmtImm(ShAmt);
      Operand *ComplShAmtImm = shAmtImm(32 - ShAmt);
      _lsr(T_Lo, Src0RLo, ShAmtImm);
      _orr(T_Lo, T_Lo,
           OperandARM32FlexReg::create(Func, IceType_i32, Src0RHi,
                                       OperandARM32::LSL, ComplShAmtImm));
      _mov(DestLo, T_Lo);

      if (ASR) {
        _asr(T_Hi, Src0RHi, ShAmtImm);
      } else {
        _lsr(T_Hi, Src0RHi, ShAmtImm);
      }
      _mov(DestHi, T_Hi);
      return;
    }

    // a=b>>c
    // pnacl-llc does:
    // mov        t_b.lo, b.lo
    // mov        t_b.hi, b.hi
    // mov        t_c.lo, c.lo
    // lsr        T0, t_b.lo, t_c.lo
    // rsb        T1, t_c.lo, #32
    // orr        t_a.lo, T0, t_b.hi, lsl T1
    // sub        T2, t_c.lo, #32
    // cmp        T2, #0
    // [al]srge   t_a.lo, t_b.hi, T2
    // [al]sr     t_a.hi, t_b.hi, t_c.lo
    // mov        a.lo, t_a.lo
    // mov        a.hi, t_a.hi
    //
    // GCC 4.8 does (lsr):
    // rsb        t_c1, c.lo, #32
    // lsr        t_lo, b.lo, c.lo
    // orr        t_lo, t_lo, b.hi, lsl t_c1
    // sub        t_c2, c.lo, #32
    // orr        t_lo, t_lo, b.hi, lsr t_c2
    // lsr        t_hi, b.hi, c.lo
    // mov        a.lo, t_lo
    // mov        a.hi, t_hi
    //
    // These are incompatible, therefore we mimic pnacl-llc.
    Operand *_32 = legalize(Ctx->getConstantInt32(32), Legal_Reg | Legal_Flex);
    Operand *_0 =
        legalize(Ctx->getConstantZero(IceType_i32), Legal_Reg | Legal_Flex);
    Variable *T0 = makeReg(IceType_i32);
    Variable *T1 = makeReg(IceType_i32);
    Variable *T2 = makeReg(IceType_i32);
    Variable *TA_Lo = makeReg(IceType_i32);
    Variable *TA_Hi = makeReg(IceType_i32);
    Variable *Src0RLo = SrcsLo.unswappedSrc0R(this);
    Variable *Src0RHi = SrcsHi.unswappedSrc0R(this);
    Variable *Src1RLo = SrcsLo.unswappedSrc1R(this);
    _lsr(T0, Src0RLo, Src1RLo);
    _rsb(T1, Src1RLo, _32);
    _orr(TA_Lo, T0, OperandARM32FlexReg::create(Func, IceType_i32, Src0RHi,
                                                OperandARM32::LSL, T1));
    _sub(T2, Src1RLo, _32);
    _cmp(T2, _0);
    if (ASR) {
      _asr(TA_Lo, Src0RHi, T2, CondARM32::GE);
      _set_dest_redefined();
      _asr(TA_Hi, Src0RHi, Src1RLo);
    } else {
      _lsr(TA_Lo, Src0RHi, T2, CondARM32::GE);
      _set_dest_redefined();
      _lsr(TA_Hi, Src0RHi, Src1RLo);
    }
    _mov(DestLo, TA_Lo);
    _mov(DestHi, TA_Hi);
    return;
  }
  case InstArithmetic::Fadd:
  case InstArithmetic::Fsub:
  case InstArithmetic::Fmul:
  case InstArithmetic::Fdiv:
  case InstArithmetic::Frem:
    llvm::report_fatal_error("FP instruction with i64 type");
    return;
  case InstArithmetic::Udiv:
  case InstArithmetic::Sdiv:
  case InstArithmetic::Urem:
  case InstArithmetic::Srem:
    llvm::report_fatal_error("Call-helper-involved instruction for i64 type "
                             "should have already been handled before");
    return;
  }
}

namespace {
// StrengthReduction is a namespace with the strength reduction machinery. The
// entry point is the StrengthReduction::tryToOptimize method. It returns true
// if the optimization can be performed, and false otherwise.
//
// If the optimization can be performed, tryToOptimize sets its NumOperations
// parameter to the number of shifts that are needed to perform the
// multiplication; and it sets the Operations parameter with <ShAmt, AddOrSub>
// tuples that describe how to materialize the multiplication.
//
// The algorithm finds contiguous 1s in the Multiplication source, and uses one
// or two shifts to materialize it. A sequence of 1s, e.g.,
//
//                  M           N
//   ...00000000000011111...111110000000...
//
// is materializable with (1 << (M + 1)) - (1 << N):
//
//   ...00000000000100000...000000000000...      [1 << (M + 1)]
//   ...00000000000000000...000010000000... (-)  [1 << N]
//   --------------------------------------
//   ...00000000000011111...111110000000...
//
// And a single bit set, which is just a left shift.
namespace StrengthReduction {
enum AggregationOperation {
  AO_Invalid,
  AO_Add,
  AO_Sub,
};

// AggregateElement is a glorified <ShAmt, AddOrSub> tuple.
class AggregationElement {
  AggregationElement(const AggregationElement &) = delete;

public:
  AggregationElement() = default;
  AggregationElement &operator=(const AggregationElement &) = default;
  AggregationElement(AggregationOperation Op, uint32_t ShAmt)
      : Op(Op), ShAmt(ShAmt) {}

  Operand *createShiftedOperand(Cfg *Func, Variable *OpR) const {
    assert(OpR->mustHaveReg());
    if (ShAmt == 0) {
      return OpR;
    }
    return OperandARM32FlexReg::create(
        Func, IceType_i32, OpR, OperandARM32::LSL,
        OperandARM32ShAmtImm::create(
            Func, llvm::cast<ConstantInteger32>(
                      Func->getContext()->getConstantInt32(ShAmt))));
  }

  bool aggregateWithAdd() const {
    switch (Op) {
    case AO_Invalid:
      llvm::report_fatal_error("Invalid Strength Reduction Operations.");
    case AO_Add:
      return true;
    case AO_Sub:
      return false;
    }
  }

  uint32_t shAmt() const { return ShAmt; }

private:
  AggregationOperation Op = AO_Invalid;
  uint32_t ShAmt;
};

// [RangeStart, RangeEnd] is a range of 1s in Src.
template <std::size_t N>
bool addOperations(uint32_t RangeStart, uint32_t RangeEnd, SizeT *NumOperations,
                   std::array<AggregationElement, N> *Operations) {
  assert(*NumOperations < N);
  if (RangeStart == RangeEnd) {
    // Single bit set:
    // Src           : 0...00010...
    // RangeStart    :        ^
    // RangeEnd      :        ^
    // NegSrc        : 0...00001...
    (*Operations)[*NumOperations] = AggregationElement(AO_Add, RangeStart);
    ++(*NumOperations);
    return true;
  }

  // Sequence of 1s: (two operations required.)
  // Src           : 0...00011...110...
  // RangeStart    :        ^
  // RangeEnd      :              ^
  // NegSrc        : 0...00000...001...
  if (*NumOperations + 1 >= N) {
    return false;
  }
  (*Operations)[*NumOperations] = AggregationElement(AO_Add, RangeStart + 1);
  ++(*NumOperations);
  (*Operations)[*NumOperations] = AggregationElement(AO_Sub, RangeEnd);
  ++(*NumOperations);
  return true;
}

// tryToOptmize scans Src looking for sequences of 1s (including the unitary bit
// 1 surrounded by zeroes.
template <std::size_t N>
bool tryToOptimize(uint32_t Src, SizeT *NumOperations,
                   std::array<AggregationElement, N> *Operations) {
  constexpr uint32_t SrcSizeBits = sizeof(Src) * CHAR_BIT;
  uint32_t NegSrc = ~Src;

  *NumOperations = 0;
  while (Src != 0 && *NumOperations < N) {
    // Each step of the algorithm:
    //   * finds L, the last bit set in Src;
    //   * clears all the upper bits in NegSrc up to bit L;
    //   * finds nL, the last bit set in NegSrc;
    //   * clears all the upper bits in Src up to bit nL;
    //
    // if L == nL + 1, then a unitary 1 was found in Src. Otherwise, a sequence
    // of 1s starting at L, and ending at nL + 1, was found.
    const uint32_t SrcLastBitSet = llvm::findLastSet(Src);
    const uint32_t NegSrcClearMask =
        (SrcLastBitSet == 0) ? 0
                             : (0xFFFFFFFFu) >> (SrcSizeBits - SrcLastBitSet);
    NegSrc &= NegSrcClearMask;
    if (NegSrc == 0) {
      if (addOperations(SrcLastBitSet, 0, NumOperations, Operations)) {
        return true;
      }
      return false;
    }
    const uint32_t NegSrcLastBitSet = llvm::findLastSet(NegSrc);
    assert(NegSrcLastBitSet < SrcLastBitSet);
    const uint32_t SrcClearMask =
        (NegSrcLastBitSet == 0) ? 0 : (0xFFFFFFFFu) >>
                                          (SrcSizeBits - NegSrcLastBitSet);
    Src &= SrcClearMask;
    if (!addOperations(SrcLastBitSet, NegSrcLastBitSet + 1, NumOperations,
                       Operations)) {
      return false;
    }
  }

  return Src == 0;
}
} // end of namespace StrengthReduction
} // end of anonymous namespace

void TargetARM32::lowerArithmetic(const InstArithmetic *Inst) {
  Variable *Dest = Inst->getDest();

  if (Dest->isRematerializable()) {
    Context.insert(InstFakeDef::create(Func, Dest));
    return;
  }

  Type DestTy = Dest->getType();
  if (DestTy == IceType_i1) {
    lowerInt1Arithmetic(Inst);
    return;
  }

  Operand *Src0 = legalizeUndef(Inst->getSrc(0));
  Operand *Src1 = legalizeUndef(Inst->getSrc(1));
  if (DestTy == IceType_i64) {
    lowerInt64Arithmetic(Inst->getOp(), Inst->getDest(), Src0, Src1);
    return;
  }

  if (isVectorType(DestTy)) {
    // Add a fake def to keep liveness consistent in the meantime.
    Variable *T = makeReg(DestTy);
    Context.insert(InstFakeDef::create(Func, T));
    _mov(Dest, T);
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }

  // DestTy is a non-i64 scalar.
  Variable *T = makeReg(DestTy);

  // * Handle div/rem separately. They require a non-legalized Src1 to inspect
  // whether or not Src1 is a non-zero constant. Once legalized it is more
  // difficult to determine (constant may be moved to a register).
  // * Handle floating point arithmetic separately: they require Src1 to be
  // legalized to a register.
  switch (Inst->getOp()) {
  default:
    break;
  case InstArithmetic::Udiv: {
    constexpr bool NotRemainder = false;
    Variable *Src0R = legalizeToReg(Src0);
    lowerIDivRem(Dest, T, Src0R, Src1, &TargetARM32::_uxt, &TargetARM32::_udiv,
                 H_udiv_i32, NotRemainder);
    return;
  }
  case InstArithmetic::Sdiv: {
    constexpr bool NotRemainder = false;
    Variable *Src0R = legalizeToReg(Src0);
    lowerIDivRem(Dest, T, Src0R, Src1, &TargetARM32::_sxt, &TargetARM32::_sdiv,
                 H_sdiv_i32, NotRemainder);
    return;
  }
  case InstArithmetic::Urem: {
    constexpr bool IsRemainder = true;
    Variable *Src0R = legalizeToReg(Src0);
    lowerIDivRem(Dest, T, Src0R, Src1, &TargetARM32::_uxt, &TargetARM32::_udiv,
                 H_urem_i32, IsRemainder);
    return;
  }
  case InstArithmetic::Srem: {
    constexpr bool IsRemainder = true;
    Variable *Src0R = legalizeToReg(Src0);
    lowerIDivRem(Dest, T, Src0R, Src1, &TargetARM32::_sxt, &TargetARM32::_sdiv,
                 H_srem_i32, IsRemainder);
    return;
  }
  case InstArithmetic::Frem: {
    constexpr SizeT MaxSrcs = 2;
    Variable *Src0R = legalizeToReg(Src0);
    Type Ty = DestTy;
    InstCall *Call = makeHelperCall(
        isFloat32Asserting32Or64(Ty) ? H_frem_f32 : H_frem_f64, Dest, MaxSrcs);
    Call->addArg(Src0R);
    Call->addArg(Src1);
    lowerCall(Call);
    return;
  }
  case InstArithmetic::Fadd: {
    Variable *Src0R = legalizeToReg(Src0);
    Variable *Src1R = legalizeToReg(Src1);
    _vadd(T, Src0R, Src1R);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Fsub: {
    Variable *Src0R = legalizeToReg(Src0);
    Variable *Src1R = legalizeToReg(Src1);
    _vsub(T, Src0R, Src1R);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Fmul: {
    Variable *Src0R = legalizeToReg(Src0);
    Variable *Src1R = legalizeToReg(Src1);
    _vmul(T, Src0R, Src1R);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Fdiv: {
    Variable *Src0R = legalizeToReg(Src0);
    Variable *Src1R = legalizeToReg(Src1);
    _vdiv(T, Src0R, Src1R);
    _mov(Dest, T);
    return;
  }
  }

  // Handle everything else here.
  Int32Operands Srcs(Src0, Src1);
  switch (Inst->getOp()) {
  case InstArithmetic::_num:
    llvm::report_fatal_error("Unknown arithmetic operator");
    return;
  case InstArithmetic::Add: {
    if (Srcs.hasConstOperand()) {
      if (!Srcs.immediateIsFlexEncodable() &&
          Srcs.negatedImmediateIsFlexEncodable()) {
        Variable *Src0R = Srcs.src0R(this);
        Operand *Src1F = Srcs.negatedSrc1F(this);
        if (!Srcs.swappedOperands()) {
          _sub(T, Src0R, Src1F);
        } else {
          _rsb(T, Src0R, Src1F);
        }
        _mov(Dest, T);
        return;
      }
    }
    Variable *Src0R = Srcs.src0R(this);
    Operand *Src1RF = Srcs.src1RF(this);
    _add(T, Src0R, Src1RF);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::And: {
    if (Srcs.hasConstOperand()) {
      if (!Srcs.immediateIsFlexEncodable() &&
          Srcs.invertedImmediateIsFlexEncodable()) {
        Variable *Src0R = Srcs.src0R(this);
        Operand *Src1F = Srcs.invertedSrc1F(this);
        _bic(T, Src0R, Src1F);
        _mov(Dest, T);
        return;
      }
    }
    Variable *Src0R = Srcs.src0R(this);
    Operand *Src1RF = Srcs.src1RF(this);
    _and(T, Src0R, Src1RF);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Or: {
    Variable *Src0R = Srcs.src0R(this);
    Operand *Src1RF = Srcs.src1RF(this);
    _orr(T, Src0R, Src1RF);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Xor: {
    Variable *Src0R = Srcs.src0R(this);
    Operand *Src1RF = Srcs.src1RF(this);
    _eor(T, Src0R, Src1RF);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Sub: {
    if (Srcs.hasConstOperand()) {
      if (Srcs.immediateIsFlexEncodable()) {
        Variable *Src0R = Srcs.src0R(this);
        Operand *Src1RF = Srcs.src1RF(this);
        if (Srcs.swappedOperands()) {
          _rsb(T, Src0R, Src1RF);
        } else {
          _sub(T, Src0R, Src1RF);
        }
        _mov(Dest, T);
        return;
      }
      if (!Srcs.swappedOperands() && Srcs.negatedImmediateIsFlexEncodable()) {
        Variable *Src0R = Srcs.src0R(this);
        Operand *Src1F = Srcs.negatedSrc1F(this);
        _add(T, Src0R, Src1F);
        _mov(Dest, T);
        return;
      }
    }
    Variable *Src0R = Srcs.unswappedSrc0R(this);
    Variable *Src1R = Srcs.unswappedSrc1R(this);
    _sub(T, Src0R, Src1R);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Mul: {
    const bool OptM1 = Ctx->getFlags().getOptLevel() == Opt_m1;
    if (!OptM1 && Srcs.hasConstOperand()) {
      constexpr std::size_t MaxShifts = 4;
      std::array<StrengthReduction::AggregationElement, MaxShifts> Shifts;
      SizeT NumOperations;
      int32_t Const = Srcs.getConstantValue();
      const bool Invert = Const < 0;
      const bool MultiplyByZero = Const == 0;
      Operand *_0 =
          legalize(Ctx->getConstantZero(DestTy), Legal_Reg | Legal_Flex);

      if (MultiplyByZero) {
        _mov(T, _0);
        _mov(Dest, T);
        return;
      }

      if (Invert) {
        Const = -Const;
      }

      if (StrengthReduction::tryToOptimize(Const, &NumOperations, &Shifts)) {
        assert(NumOperations >= 1);
        Variable *Src0R = Srcs.src0R(this);
        int32_t Start;
        int32_t End;
        if (NumOperations == 1 || Shifts[NumOperations - 1].shAmt() != 0) {
          // Multiplication by a power of 2 (NumOperations == 1); or
          // Multiplication by a even number not a power of 2.
          Start = 1;
          End = NumOperations;
          assert(Shifts[0].aggregateWithAdd());
          _lsl(T, Src0R, shAmtImm(Shifts[0].shAmt()));
        } else {
          // Multiplication by an odd number. Put the free barrel shifter to a
          // good use.
          Start = 0;
          End = NumOperations - 2;
          const StrengthReduction::AggregationElement &Last =
              Shifts[NumOperations - 1];
          const StrengthReduction::AggregationElement &SecondToLast =
              Shifts[NumOperations - 2];
          if (!Last.aggregateWithAdd()) {
            assert(SecondToLast.aggregateWithAdd());
            _rsb(T, Src0R, SecondToLast.createShiftedOperand(Func, Src0R));
          } else if (!SecondToLast.aggregateWithAdd()) {
            assert(Last.aggregateWithAdd());
            _sub(T, Src0R, SecondToLast.createShiftedOperand(Func, Src0R));
          } else {
            _add(T, Src0R, SecondToLast.createShiftedOperand(Func, Src0R));
          }
        }

        // Odd numbers :   S                                 E   I   I
        //               +---+---+---+---+---+---+ ... +---+---+---+---+
        //     Shifts  = |   |   |   |   |   |   | ... |   |   |   |   |
        //               +---+---+---+---+---+---+ ... +---+---+---+---+
        // Even numbers:   I   S                                     E
        //
        // S: Start; E: End; I: Init
        for (int32_t I = Start; I < End; ++I) {
          const StrengthReduction::AggregationElement &Current = Shifts[I];
          Operand *SrcF = Current.createShiftedOperand(Func, Src0R);
          if (Current.aggregateWithAdd()) {
            _add(T, T, SrcF);
          } else {
            _sub(T, T, SrcF);
          }
        }

        if (Invert) {
          // T = 0 - T.
          _rsb(T, T, _0);
        }

        _mov(Dest, T);
        return;
      }
    }
    Variable *Src0R = Srcs.unswappedSrc0R(this);
    Variable *Src1R = Srcs.unswappedSrc1R(this);
    _mul(T, Src0R, Src1R);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Shl: {
    Variable *Src0R = Srcs.unswappedSrc0R(this);
    Operand *Src1R = Srcs.unswappedSrc1RShAmtImm(this);
    _lsl(T, Src0R, Src1R);
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Lshr: {
    Variable *Src0R = Srcs.unswappedSrc0R(this);
    if (DestTy != IceType_i32) {
      _uxt(Src0R, Src0R);
    }
    _lsr(T, Src0R, Srcs.unswappedSrc1RShAmtImm(this));
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Ashr: {
    Variable *Src0R = Srcs.unswappedSrc0R(this);
    if (DestTy != IceType_i32) {
      _sxt(Src0R, Src0R);
    }
    _asr(T, Src0R, Srcs.unswappedSrc1RShAmtImm(this));
    _mov(Dest, T);
    return;
  }
  case InstArithmetic::Udiv:
  case InstArithmetic::Sdiv:
  case InstArithmetic::Urem:
  case InstArithmetic::Srem:
    llvm::report_fatal_error(
        "Integer div/rem should have been handled earlier.");
    return;
  case InstArithmetic::Fadd:
  case InstArithmetic::Fsub:
  case InstArithmetic::Fmul:
  case InstArithmetic::Fdiv:
  case InstArithmetic::Frem:
    llvm::report_fatal_error(
        "Floating point arith should have been handled earlier.");
    return;
  }
}

void TargetARM32::lowerAssign(const InstAssign *Inst) {
  Variable *Dest = Inst->getDest();

  if (Dest->isRematerializable()) {
    Context.insert(InstFakeDef::create(Func, Dest));
    return;
  }

  Operand *Src0 = Inst->getSrc(0);
  assert(Dest->getType() == Src0->getType());
  if (Dest->getType() == IceType_i64) {
    Src0 = legalizeUndef(Src0);

    Variable *T_Lo = makeReg(IceType_i32);
    auto *DestLo = llvm::cast<Variable>(loOperand(Dest));
    Operand *Src0Lo = legalize(loOperand(Src0), Legal_Reg | Legal_Flex);
    _mov(T_Lo, Src0Lo);
    _mov(DestLo, T_Lo);

    Variable *T_Hi = makeReg(IceType_i32);
    auto *DestHi = llvm::cast<Variable>(hiOperand(Dest));
    Operand *Src0Hi = legalize(hiOperand(Src0), Legal_Reg | Legal_Flex);
    _mov(T_Hi, Src0Hi);
    _mov(DestHi, T_Hi);

    return;
  }

  Operand *NewSrc;
  if (Dest->hasReg()) {
    // If Dest already has a physical register, then legalize the Src operand
    // into a Variable with the same register assignment. This especially
    // helps allow the use of Flex operands.
    NewSrc = legalize(Src0, Legal_Reg | Legal_Flex, Dest->getRegNum());
  } else {
    // Dest could be a stack operand. Since we could potentially need to do a
    // Store (and store can only have Register operands), legalize this to a
    // register.
    NewSrc = legalize(Src0, Legal_Reg);
  }

  if (isVectorType(Dest->getType()) || isScalarFloatingType(Dest->getType())) {
    NewSrc = legalize(NewSrc, Legal_Reg | Legal_Mem);
  }
  _mov(Dest, NewSrc);
}

TargetARM32::ShortCircuitCondAndLabel TargetARM32::lowerInt1ForBranch(
    Operand *Boolean, const LowerInt1BranchTarget &TargetTrue,
    const LowerInt1BranchTarget &TargetFalse, uint32_t ShortCircuitable) {
  InstARM32Label *NewShortCircuitLabel = nullptr;
  Operand *_1 = legalize(Ctx->getConstantInt1(1), Legal_Reg | Legal_Flex);

  const Inst *Producer = BoolComputations.getProducerOf(Boolean);

  if (Producer == nullptr) {
    // No producer, no problem: just do emit code to perform (Boolean & 1) and
    // set the flags register. The branch should be taken if the resulting flags
    // indicate a non-zero result.
    _tst(legalizeToReg(Boolean), _1);
    return ShortCircuitCondAndLabel(CondWhenTrue(CondARM32::NE));
  }

  switch (Producer->getKind()) {
  default:
    llvm_unreachable("Unexpected producer.");
  case Inst::Icmp: {
    return ShortCircuitCondAndLabel(
        lowerIcmpCond(llvm::cast<InstIcmp>(Producer)));
  } break;
  case Inst::Fcmp: {
    return ShortCircuitCondAndLabel(
        lowerFcmpCond(llvm::cast<InstFcmp>(Producer)));
  } break;
  case Inst::Cast: {
    const auto *CastProducer = llvm::cast<InstCast>(Producer);
    assert(CastProducer->getCastKind() == InstCast::Trunc);
    Operand *Src = CastProducer->getSrc(0);
    if (Src->getType() == IceType_i64)
      Src = loOperand(Src);
    _tst(legalizeToReg(Src), _1);
    return ShortCircuitCondAndLabel(CondWhenTrue(CondARM32::NE));
  } break;
  case Inst::Arithmetic: {
    const auto *ArithProducer = llvm::cast<InstArithmetic>(Producer);
    switch (ArithProducer->getOp()) {
    default:
      llvm_unreachable("Unhandled Arithmetic Producer.");
    case InstArithmetic::And: {
      if (!(ShortCircuitable & SC_And)) {
        NewShortCircuitLabel = InstARM32Label::create(Func, this);
      }

      LowerInt1BranchTarget NewTarget =
          TargetFalse.createForLabelOrDuplicate(NewShortCircuitLabel);

      ShortCircuitCondAndLabel CondAndLabel = lowerInt1ForBranch(
          Producer->getSrc(0), TargetTrue, NewTarget, SC_And);
      const CondWhenTrue &Cond = CondAndLabel.Cond;

      _br_short_circuit(NewTarget, Cond.invert());

      InstARM32Label *const ShortCircuitLabel = CondAndLabel.ShortCircuitTarget;
      if (ShortCircuitLabel != nullptr)
        Context.insert(ShortCircuitLabel);

      return ShortCircuitCondAndLabel(
          lowerInt1ForBranch(Producer->getSrc(1), TargetTrue, NewTarget, SC_All)
              .assertNoLabelAndReturnCond(),
          NewShortCircuitLabel);
    } break;
    case InstArithmetic::Or: {
      if (!(ShortCircuitable & SC_Or)) {
        NewShortCircuitLabel = InstARM32Label::create(Func, this);
      }

      LowerInt1BranchTarget NewTarget =
          TargetTrue.createForLabelOrDuplicate(NewShortCircuitLabel);

      ShortCircuitCondAndLabel CondAndLabel = lowerInt1ForBranch(
          Producer->getSrc(0), NewTarget, TargetFalse, SC_Or);
      const CondWhenTrue &Cond = CondAndLabel.Cond;

      _br_short_circuit(NewTarget, Cond);

      InstARM32Label *const ShortCircuitLabel = CondAndLabel.ShortCircuitTarget;
      if (ShortCircuitLabel != nullptr)
        Context.insert(ShortCircuitLabel);

      return ShortCircuitCondAndLabel(lowerInt1ForBranch(Producer->getSrc(1),
                                                         NewTarget, TargetFalse,
                                                         SC_All)
                                          .assertNoLabelAndReturnCond(),
                                      NewShortCircuitLabel);
    } break;
    }
  }
  }
}

void TargetARM32::lowerBr(const InstBr *Instr) {
  if (Instr->isUnconditional()) {
    _br(Instr->getTargetUnconditional());
    return;
  }

  CfgNode *TargetTrue = Instr->getTargetTrue();
  CfgNode *TargetFalse = Instr->getTargetFalse();
  ShortCircuitCondAndLabel CondAndLabel = lowerInt1ForBranch(
      Instr->getCondition(), LowerInt1BranchTarget(TargetTrue),
      LowerInt1BranchTarget(TargetFalse), SC_All);
  assert(CondAndLabel.ShortCircuitTarget == nullptr);

  const CondWhenTrue &Cond = CondAndLabel.Cond;
  if (Cond.WhenTrue1 != CondARM32::kNone) {
    assert(Cond.WhenTrue0 != CondARM32::AL);
    _br(TargetTrue, Cond.WhenTrue1);
  }

  switch (Cond.WhenTrue0) {
  default:
    _br(TargetTrue, TargetFalse, Cond.WhenTrue0);
    break;
  case CondARM32::kNone:
    _br(TargetFalse);
    break;
  case CondARM32::AL:
    _br(TargetTrue);
    break;
  }
}

void TargetARM32::lowerCall(const InstCall *Instr) {
  MaybeLeafFunc = false;
  NeedsStackAlignment = true;

  // Assign arguments to registers and stack. Also reserve stack.
  TargetARM32::CallingConv CC;
  // Pair of Arg Operand -> GPR number assignments.
  llvm::SmallVector<std::pair<Operand *, int32_t>,
                    TargetARM32::CallingConv::ARM32_MAX_GPR_ARG> GPRArgs;
  llvm::SmallVector<std::pair<Operand *, int32_t>,
                    TargetARM32::CallingConv::ARM32_MAX_FP_REG_UNITS> FPArgs;
  // Pair of Arg Operand -> stack offset.
  llvm::SmallVector<std::pair<Operand *, int32_t>, 8> StackArgs;
  size_t ParameterAreaSizeBytes = 0;

  // Classify each argument operand according to the location where the
  // argument is passed.
  for (SizeT i = 0, NumArgs = Instr->getNumArgs(); i < NumArgs; ++i) {
    Operand *Arg = legalizeUndef(Instr->getArg(i));
    Type Ty = Arg->getType();
    bool InRegs = false;
    if (Ty == IceType_i64) {
      std::pair<int32_t, int32_t> Regs;
      if (CC.I64InRegs(&Regs)) {
        InRegs = true;
        Operand *Lo = loOperand(Arg);
        Operand *Hi = hiOperand(Arg);
        GPRArgs.push_back(std::make_pair(Lo, Regs.first));
        GPRArgs.push_back(std::make_pair(Hi, Regs.second));
      }
    } else if (isVectorType(Ty) || isFloatingType(Ty)) {
      int32_t Reg;
      if (CC.FPInReg(Ty, &Reg)) {
        InRegs = true;
        FPArgs.push_back(std::make_pair(Arg, Reg));
      }
    } else {
      assert(Ty == IceType_i32);
      int32_t Reg;
      if (CC.I32InReg(&Reg)) {
        InRegs = true;
        GPRArgs.push_back(std::make_pair(Arg, Reg));
      }
    }

    if (!InRegs) {
      ParameterAreaSizeBytes =
          applyStackAlignmentTy(ParameterAreaSizeBytes, Ty);
      StackArgs.push_back(std::make_pair(Arg, ParameterAreaSizeBytes));
      ParameterAreaSizeBytes += typeWidthInBytesOnStack(Ty);
    }
  }

  // Adjust the parameter area so that the stack is aligned. It is assumed that
  // the stack is already aligned at the start of the calling sequence.
  ParameterAreaSizeBytes = applyStackAlignment(ParameterAreaSizeBytes);

  if (ParameterAreaSizeBytes > MaxOutArgsSizeBytes) {
    llvm::report_fatal_error("MaxOutArgsSizeBytes is not really a max.");
  }

  // Copy arguments that are passed on the stack to the appropriate stack
  // locations.
  Variable *SP = getPhysicalRegister(RegARM32::Reg_sp);
  for (auto &StackArg : StackArgs) {
    ConstantInteger32 *Loc =
        llvm::cast<ConstantInteger32>(Ctx->getConstantInt32(StackArg.second));
    Type Ty = StackArg.first->getType();
    OperandARM32Mem *Addr;
    constexpr bool SignExt = false;
    if (OperandARM32Mem::canHoldOffset(Ty, SignExt, StackArg.second)) {
      Addr = OperandARM32Mem::create(Func, Ty, SP, Loc);
    } else {
      Variable *NewBase = Func->makeVariable(SP->getType());
      lowerArithmetic(
          InstArithmetic::create(Func, InstArithmetic::Add, NewBase, SP, Loc));
      Addr = formMemoryOperand(NewBase, Ty);
    }
    lowerStore(InstStore::create(Func, StackArg.first, Addr));
  }

  // Generate the call instruction. Assign its result to a temporary with high
  // register allocation weight.
  Variable *Dest = Instr->getDest();
  // ReturnReg doubles as ReturnRegLo as necessary.
  Variable *ReturnReg = nullptr;
  Variable *ReturnRegHi = nullptr;
  if (Dest) {
    switch (Dest->getType()) {
    case IceType_NUM:
      llvm_unreachable("Invalid Call dest type");
      break;
    case IceType_void:
      break;
    case IceType_i1:
      assert(BoolComputations.getProducerOf(Dest) == nullptr);
    // Fall-through intended.
    case IceType_i8:
    case IceType_i16:
    case IceType_i32:
      ReturnReg = makeReg(Dest->getType(), RegARM32::Reg_r0);
      break;
    case IceType_i64:
      ReturnReg = makeReg(IceType_i32, RegARM32::Reg_r0);
      ReturnRegHi = makeReg(IceType_i32, RegARM32::Reg_r1);
      break;
    case IceType_f32:
      ReturnReg = makeReg(Dest->getType(), RegARM32::Reg_s0);
      break;
    case IceType_f64:
      ReturnReg = makeReg(Dest->getType(), RegARM32::Reg_d0);
      break;
    case IceType_v4i1:
    case IceType_v8i1:
    case IceType_v16i1:
    case IceType_v16i8:
    case IceType_v8i16:
    case IceType_v4i32:
    case IceType_v4f32:
      ReturnReg = makeReg(Dest->getType(), RegARM32::Reg_q0);
      break;
    }
  }
  Operand *CallTarget = Instr->getCallTarget();
  // TODO(jvoung): Handle sandboxing. const bool NeedSandboxing =
  // Ctx->getFlags().getUseSandboxing();

  // Allow ConstantRelocatable to be left alone as a direct call, but force
  // other constants like ConstantInteger32 to be in a register and make it an
  // indirect call.
  if (!llvm::isa<ConstantRelocatable>(CallTarget)) {
    CallTarget = legalize(CallTarget, Legal_Reg);
  }

  // Copy arguments to be passed in registers to the appropriate registers.
  for (auto &FPArg : FPArgs) {
    Variable *Reg = legalizeToReg(FPArg.first, FPArg.second);
    Context.insert(InstFakeUse::create(Func, Reg));
  }
  for (auto &GPRArg : GPRArgs) {
    Variable *Reg = legalizeToReg(GPRArg.first, GPRArg.second);
    // Generate a FakeUse of register arguments so that they do not get dead
    // code eliminated as a result of the FakeKill of scratch registers after
    // the call.
    Context.insert(InstFakeUse::create(Func, Reg));
  }
  Inst *NewCall = InstARM32Call::create(Func, ReturnReg, CallTarget);
  Context.insert(NewCall);
  if (ReturnRegHi)
    Context.insert(InstFakeDef::create(Func, ReturnRegHi));

  // Insert a register-kill pseudo instruction.
  Context.insert(InstFakeKill::create(Func, NewCall));

  // Generate a FakeUse to keep the call live if necessary.
  if (Instr->hasSideEffects() && ReturnReg) {
    Inst *FakeUse = InstFakeUse::create(Func, ReturnReg);
    Context.insert(FakeUse);
  }

  if (!Dest)
    return;

  // Assign the result of the call to Dest.
  if (ReturnReg) {
    if (ReturnRegHi) {
      auto *Dest64On32 = llvm::cast<Variable64On32>(Dest);
      Variable *DestLo = Dest64On32->getLo();
      Variable *DestHi = Dest64On32->getHi();
      _mov(DestLo, ReturnReg);
      _mov(DestHi, ReturnRegHi);
    } else {
      if (isFloatingType(Dest->getType()) || isVectorType(Dest->getType())) {
        _mov(Dest, ReturnReg);
      } else {
        assert(isIntegerType(Dest->getType()) &&
               typeWidthInBytes(Dest->getType()) <= 4);
        _mov(Dest, ReturnReg);
      }
    }
  }
}

namespace {
void configureBitcastTemporary(Variable64On32 *Var) {
  Var->setMustNotHaveReg();
  Var->getHi()->setMustHaveReg();
  Var->getLo()->setMustHaveReg();
}
} // end of anonymous namespace

void TargetARM32::lowerCast(const InstCast *Inst) {
  InstCast::OpKind CastKind = Inst->getCastKind();
  Variable *Dest = Inst->getDest();
  Operand *Src0 = legalizeUndef(Inst->getSrc(0));
  switch (CastKind) {
  default:
    Func->setError("Cast type not supported");
    return;
  case InstCast::Sext: {
    if (isVectorType(Dest->getType())) {
      Variable *T = makeReg(Dest->getType());
      Context.insert(InstFakeDef::create(Func, T, legalizeToReg(Src0)));
      _mov(Dest, T);
      UnimplementedError(Func->getContext()->getFlags());
    } else if (Dest->getType() == IceType_i64) {
      // t1=sxtb src; t2= mov t1 asr #31; dst.lo=t1; dst.hi=t2
      Constant *ShiftAmt = Ctx->getConstantInt32(31);
      Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
      Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
      Variable *T_Lo = makeReg(DestLo->getType());
      if (Src0->getType() == IceType_i32) {
        Operand *Src0RF = legalize(Src0, Legal_Reg | Legal_Flex);
        _mov(T_Lo, Src0RF);
      } else if (Src0->getType() != IceType_i1) {
        Variable *Src0R = legalizeToReg(Src0);
        _sxt(T_Lo, Src0R);
      } else {
        Operand *_0 = Ctx->getConstantZero(IceType_i32);
        Operand *_m1 = Ctx->getConstantInt32(-1);
        lowerInt1ForSelect(T_Lo, Src0, _m1, _0);
      }
      _mov(DestLo, T_Lo);
      Variable *T_Hi = makeReg(DestHi->getType());
      if (Src0->getType() != IceType_i1) {
        _mov(T_Hi, OperandARM32FlexReg::create(Func, IceType_i32, T_Lo,
                                               OperandARM32::ASR, ShiftAmt));
      } else {
        // For i1, the asr instruction is already done above.
        _mov(T_Hi, T_Lo);
      }
      _mov(DestHi, T_Hi);
    } else if (Src0->getType() != IceType_i1) {
      // t1 = sxt src; dst = t1
      Variable *Src0R = legalizeToReg(Src0);
      Variable *T = makeReg(Dest->getType());
      _sxt(T, Src0R);
      _mov(Dest, T);
    } else {
      Constant *_0 = Ctx->getConstantZero(IceType_i32);
      Operand *_m1 = Ctx->getConstantInt(Dest->getType(), -1);
      Variable *T = makeReg(Dest->getType());
      lowerInt1ForSelect(T, Src0, _m1, _0);
      _mov(Dest, T);
    }
    break;
  }
  case InstCast::Zext: {
    if (isVectorType(Dest->getType())) {
      Variable *T = makeReg(Dest->getType());
      Context.insert(InstFakeDef::create(Func, T, legalizeToReg(Src0)));
      _mov(Dest, T);
      UnimplementedError(Func->getContext()->getFlags());
    } else if (Dest->getType() == IceType_i64) {
      // t1=uxtb src; dst.lo=t1; dst.hi=0
      Operand *_0 =
          legalize(Ctx->getConstantZero(IceType_i32), Legal_Reg | Legal_Flex);
      Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
      Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
      Variable *T_Lo = makeReg(DestLo->getType());

      switch (Src0->getType()) {
      default: {
        assert(Src0->getType() != IceType_i64);
        _uxt(T_Lo, legalizeToReg(Src0));
      } break;
      case IceType_i32: {
        _mov(T_Lo, legalize(Src0, Legal_Reg | Legal_Flex));
      } break;
      case IceType_i1: {
        SafeBoolChain Safe = lowerInt1(T_Lo, Src0);
        if (Safe == SBC_No) {
          Operand *_1 =
              legalize(Ctx->getConstantInt1(1), Legal_Reg | Legal_Flex);
          _and(T_Lo, T_Lo, _1);
        }
      } break;
      }

      _mov(DestLo, T_Lo);

      Variable *T_Hi = makeReg(DestLo->getType());
      _mov(T_Hi, _0);
      _mov(DestHi, T_Hi);
    } else if (Src0->getType() == IceType_i1) {
      Variable *T = makeReg(Dest->getType());

      SafeBoolChain Safe = lowerInt1(T, Src0);
      if (Safe == SBC_No) {
        Operand *_1 = legalize(Ctx->getConstantInt1(1), Legal_Reg | Legal_Flex);
        _and(T, T, _1);
      }

      _mov(Dest, T);
    } else {
      // t1 = uxt src; dst = t1
      Variable *Src0R = legalizeToReg(Src0);
      Variable *T = makeReg(Dest->getType());
      _uxt(T, Src0R);
      _mov(Dest, T);
    }
    break;
  }
  case InstCast::Trunc: {
    if (isVectorType(Dest->getType())) {
      Variable *T = makeReg(Dest->getType());
      Context.insert(InstFakeDef::create(Func, T, legalizeToReg(Src0)));
      _mov(Dest, T);
      UnimplementedError(Func->getContext()->getFlags());
    } else {
      if (Src0->getType() == IceType_i64)
        Src0 = loOperand(Src0);
      Operand *Src0RF = legalize(Src0, Legal_Reg | Legal_Flex);
      // t1 = trunc Src0RF; Dest = t1
      Variable *T = makeReg(Dest->getType());
      _mov(T, Src0RF);
      if (Dest->getType() == IceType_i1)
        _and(T, T, Ctx->getConstantInt1(1));
      _mov(Dest, T);
    }
    break;
  }
  case InstCast::Fptrunc:
  case InstCast::Fpext: {
    // fptrunc: dest.f32 = fptrunc src0.fp64
    // fpext: dest.f64 = fptrunc src0.fp32
    const bool IsTrunc = CastKind == InstCast::Fptrunc;
    if (isVectorType(Dest->getType())) {
      Variable *T = makeReg(Dest->getType());
      Context.insert(InstFakeDef::create(Func, T, legalizeToReg(Src0)));
      _mov(Dest, T);
      UnimplementedError(Func->getContext()->getFlags());
      break;
    }
    assert(Dest->getType() == (IsTrunc ? IceType_f32 : IceType_f64));
    assert(Src0->getType() == (IsTrunc ? IceType_f64 : IceType_f32));
    Variable *Src0R = legalizeToReg(Src0);
    Variable *T = makeReg(Dest->getType());
    _vcvt(T, Src0R, IsTrunc ? InstARM32Vcvt::D2s : InstARM32Vcvt::S2d);
    _mov(Dest, T);
    break;
  }
  case InstCast::Fptosi:
  case InstCast::Fptoui: {
    if (isVectorType(Dest->getType())) {
      Variable *T = makeReg(Dest->getType());
      Context.insert(InstFakeDef::create(Func, T, legalizeToReg(Src0)));
      _mov(Dest, T);
      UnimplementedError(Func->getContext()->getFlags());
      break;
    }

    const bool DestIsSigned = CastKind == InstCast::Fptosi;
    const bool Src0IsF32 = isFloat32Asserting32Or64(Src0->getType());
    if (llvm::isa<Variable64On32>(Dest)) {
      const char *HelperName =
          Src0IsF32 ? (DestIsSigned ? H_fptosi_f32_i64 : H_fptoui_f32_i64)
                    : (DestIsSigned ? H_fptosi_f64_i64 : H_fptoui_f64_i64);
      static constexpr SizeT MaxSrcs = 1;
      InstCall *Call = makeHelperCall(HelperName, Dest, MaxSrcs);
      Call->addArg(Src0);
      lowerCall(Call);
      break;
    }
    // fptosi:
    //     t1.fp = vcvt src0.fp
    //     t2.i32 = vmov t1.fp
    //     dest.int = conv t2.i32     @ Truncates the result if needed.
    // fptoui:
    //     t1.fp = vcvt src0.fp
    //     t2.u32 = vmov t1.fp
    //     dest.uint = conv t2.u32    @ Truncates the result if needed.
    Variable *Src0R = legalizeToReg(Src0);
    Variable *T_fp = makeReg(IceType_f32);
    const InstARM32Vcvt::VcvtVariant Conversion =
        Src0IsF32 ? (DestIsSigned ? InstARM32Vcvt::S2si : InstARM32Vcvt::S2ui)
                  : (DestIsSigned ? InstARM32Vcvt::D2si : InstARM32Vcvt::D2ui);
    _vcvt(T_fp, Src0R, Conversion);
    Variable *T = makeReg(IceType_i32);
    _mov(T, T_fp);
    if (Dest->getType() != IceType_i32) {
      Variable *T_1 = makeReg(Dest->getType());
      lowerCast(InstCast::create(Func, InstCast::Trunc, T_1, T));
      T = T_1;
    }
    _mov(Dest, T);
    break;
  }
  case InstCast::Sitofp:
  case InstCast::Uitofp: {
    if (isVectorType(Dest->getType())) {
      Variable *T = makeReg(Dest->getType());
      Context.insert(InstFakeDef::create(Func, T, legalizeToReg(Src0)));
      _mov(Dest, T);
      UnimplementedError(Func->getContext()->getFlags());
      break;
    }
    const bool SourceIsSigned = CastKind == InstCast::Sitofp;
    const bool DestIsF32 = isFloat32Asserting32Or64(Dest->getType());
    if (Src0->getType() == IceType_i64) {
      const char *HelperName =
          DestIsF32 ? (SourceIsSigned ? H_sitofp_i64_f32 : H_uitofp_i64_f32)
                    : (SourceIsSigned ? H_sitofp_i64_f64 : H_uitofp_i64_f64);
      static constexpr SizeT MaxSrcs = 1;
      InstCall *Call = makeHelperCall(HelperName, Dest, MaxSrcs);
      Call->addArg(Src0);
      lowerCall(Call);
      break;
    }
    // sitofp:
    //     t1.i32 = sext src.int    @ sign-extends src0 if needed.
    //     t2.fp32 = vmov t1.i32
    //     t3.fp = vcvt.{fp}.s32    @ fp is either f32 or f64
    // uitofp:
    //     t1.i32 = zext src.int    @ zero-extends src0 if needed.
    //     t2.fp32 = vmov t1.i32
    //     t3.fp = vcvt.{fp}.s32    @ fp is either f32 or f64
    if (Src0->getType() != IceType_i32) {
      Variable *Src0R_32 = makeReg(IceType_i32);
      lowerCast(InstCast::create(Func, SourceIsSigned ? InstCast::Sext
                                                      : InstCast::Zext,
                                 Src0R_32, Src0));
      Src0 = Src0R_32;
    }
    Variable *Src0R = legalizeToReg(Src0);
    Variable *Src0R_f32 = makeReg(IceType_f32);
    _mov(Src0R_f32, Src0R);
    Src0R = Src0R_f32;
    Variable *T = makeReg(Dest->getType());
    const InstARM32Vcvt::VcvtVariant Conversion =
        DestIsF32
            ? (SourceIsSigned ? InstARM32Vcvt::Si2s : InstARM32Vcvt::Ui2s)
            : (SourceIsSigned ? InstARM32Vcvt::Si2d : InstARM32Vcvt::Ui2d);
    _vcvt(T, Src0R, Conversion);
    _mov(Dest, T);
    break;
  }
  case InstCast::Bitcast: {
    Operand *Src0 = Inst->getSrc(0);
    if (Dest->getType() == Src0->getType()) {
      InstAssign *Assign = InstAssign::create(Func, Dest, Src0);
      lowerAssign(Assign);
      return;
    }
    Type DestType = Dest->getType();
    switch (DestType) {
    case IceType_NUM:
    case IceType_void:
      llvm::report_fatal_error("Unexpected bitcast.");
    case IceType_i1:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case IceType_i8:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case IceType_i16:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case IceType_i32:
    case IceType_f32: {
      Variable *Src0R = legalizeToReg(Src0);
      Variable *T = makeReg(DestType);
      _mov(T, Src0R);
      lowerAssign(InstAssign::create(Func, Dest, T));
      break;
    }
    case IceType_i64: {
      // t0, t1 <- src0
      // dest[31..0]  = t0
      // dest[63..32] = t1
      assert(Src0->getType() == IceType_f64);
      auto *T = llvm::cast<Variable64On32>(Func->makeVariable(IceType_i64));
      T->initHiLo(Func);
      configureBitcastTemporary(T);
      Variable *Src0R = legalizeToReg(Src0);
      _mov(T, Src0R);
      Context.insert(InstFakeUse::create(Func, T->getHi()));
      Context.insert(InstFakeUse::create(Func, T->getLo()));
      lowerAssign(InstAssign::create(Func, Dest, T));
      break;
    }
    case IceType_f64: {
      // T0 <- lo(src)
      // T1 <- hi(src)
      // vmov T2, T0, T1
      // Dest <- T2
      assert(Src0->getType() == IceType_i64);
      Variable *T = makeReg(DestType);
      auto *Src64 = llvm::cast<Variable64On32>(Func->makeVariable(IceType_i64));
      Src64->initHiLo(Func);
      configureBitcastTemporary(Src64);
      lowerAssign(InstAssign::create(Func, Src64, Src0));
      _mov(T, Src64);
      lowerAssign(InstAssign::create(Func, Dest, T));
      break;
    }
    case IceType_v4i1:
    case IceType_v8i1:
    case IceType_v16i1:
    case IceType_v8i16:
    case IceType_v16i8:
    case IceType_v4f32:
    case IceType_v4i32: {
      // avoid liveness errors
      Variable *T = makeReg(DestType);
      Context.insert(InstFakeDef::create(Func, T, legalizeToReg(Src0)));
      _mov(Dest, T);
      UnimplementedError(Func->getContext()->getFlags());
      break;
    }
    }
    break;
  }
  }
}

void TargetARM32::lowerExtractElement(const InstExtractElement *Inst) {
  Variable *Dest = Inst->getDest();
  Type DestType = Dest->getType();
  Variable *T = makeReg(DestType);
  Context.insert(InstFakeDef::create(Func, T));
  _mov(Dest, T);
  UnimplementedError(Func->getContext()->getFlags());
}

namespace {
// Validates FCMPARM32_TABLE's declaration w.r.t. InstFcmp::FCondition ordering
// (and naming).
enum {
#define X(val, CC0, CC1) _fcmp_ll_##val,
  FCMPARM32_TABLE
#undef X
      _fcmp_ll_NUM
};

enum {
#define X(tag, str) _fcmp_hl_##tag = InstFcmp::tag,
  ICEINSTFCMP_TABLE
#undef X
      _fcmp_hl_NUM
};

static_assert(_fcmp_hl_NUM == _fcmp_ll_NUM,
              "Inconsistency between high-level and low-level fcmp tags.");
#define X(tag, str)                                                            \
  static_assert(                                                               \
      _fcmp_hl_##tag == _fcmp_ll_##tag,                                        \
      "Inconsistency between high-level and low-level fcmp tag " #tag);
ICEINSTFCMP_TABLE
#undef X

struct {
  CondARM32::Cond CC0;
  CondARM32::Cond CC1;
} TableFcmp[] = {
#define X(val, CC0, CC1)                                                       \
  { CondARM32::CC0, CondARM32::CC1 }                                           \
  ,
    FCMPARM32_TABLE
#undef X
};

bool isFloatingPointZero(Operand *Src) {
  if (const auto *F32 = llvm::dyn_cast<ConstantFloat>(Src)) {
    return Utils::isPositiveZero(F32->getValue());
  }

  if (const auto *F64 = llvm::dyn_cast<ConstantDouble>(Src)) {
    return Utils::isPositiveZero(F64->getValue());
  }

  return false;
}
} // end of anonymous namespace

TargetARM32::CondWhenTrue TargetARM32::lowerFcmpCond(const InstFcmp *Instr) {
  InstFcmp::FCond Condition = Instr->getCondition();
  switch (Condition) {
  case InstFcmp::False:
    return CondWhenTrue(CondARM32::kNone);
  case InstFcmp::True:
    return CondWhenTrue(CondARM32::AL);
    break;
  default: {
    Variable *Src0R = legalizeToReg(Instr->getSrc(0));
    Operand *Src1 = Instr->getSrc(1);
    if (isFloatingPointZero(Src1)) {
      _vcmp(Src0R, OperandARM32FlexFpZero::create(Func, Src0R->getType()));
    } else {
      _vcmp(Src0R, legalizeToReg(Src1));
    }
    _vmrs();
    assert(Condition < llvm::array_lengthof(TableFcmp));
    return CondWhenTrue(TableFcmp[Condition].CC0, TableFcmp[Condition].CC1);
  }
  }
}

void TargetARM32::lowerFcmp(const InstFcmp *Instr) {
  Variable *Dest = Instr->getDest();
  if (isVectorType(Dest->getType())) {
    Variable *T = makeReg(Dest->getType());
    Context.insert(InstFakeDef::create(Func, T));
    _mov(Dest, T);
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }

  Variable *T = makeReg(IceType_i1);
  Operand *_1 = legalize(Ctx->getConstantInt32(1), Legal_Reg | Legal_Flex);
  Operand *_0 =
      legalize(Ctx->getConstantZero(IceType_i32), Legal_Reg | Legal_Flex);

  CondWhenTrue Cond = lowerFcmpCond(Instr);

  bool RedefineT = false;
  if (Cond.WhenTrue0 != CondARM32::AL) {
    _mov(T, _0);
    RedefineT = true;
  }

  if (Cond.WhenTrue0 == CondARM32::kNone) {
    _mov(Dest, T);
    return;
  }

  if (RedefineT) {
    _mov_redefined(T, _1, Cond.WhenTrue0);
  } else {
    _mov(T, _1, Cond.WhenTrue0);
  }

  if (Cond.WhenTrue1 != CondARM32::kNone) {
    _mov_redefined(T, _1, Cond.WhenTrue1);
  }

  _mov(Dest, T);
}

TargetARM32::CondWhenTrue
TargetARM32::lowerInt64IcmpCond(InstIcmp::ICond Condition, Operand *Src0,
                                Operand *Src1) {
  size_t Index = static_cast<size_t>(Condition);
  assert(Index < llvm::array_lengthof(TableIcmp64));

  Int32Operands SrcsLo(loOperand(Src0), loOperand(Src1));
  Int32Operands SrcsHi(hiOperand(Src0), hiOperand(Src1));
  assert(SrcsLo.hasConstOperand() == SrcsHi.hasConstOperand());
  assert(SrcsLo.swappedOperands() == SrcsHi.swappedOperands());

  if (SrcsLo.hasConstOperand()) {
    const uint32_t ValueLo = SrcsLo.getConstantValue();
    const uint32_t ValueHi = SrcsHi.getConstantValue();
    const uint64_t Value = (static_cast<uint64_t>(ValueHi) << 32) | ValueLo;
    if ((Condition == InstIcmp::Eq || Condition == InstIcmp::Ne) &&
        Value == 0) {
      Variable *T = makeReg(IceType_i32);
      Variable *Src0LoR = SrcsLo.src0R(this);
      Variable *Src0HiR = SrcsHi.src0R(this);
      _orrs(T, Src0LoR, Src0HiR);
      Context.insert(InstFakeUse::create(Func, T));
      return CondWhenTrue(TableIcmp64[Index].C1);
    }

    Variable *Src0RLo = SrcsLo.src0R(this);
    Variable *Src0RHi = SrcsHi.src0R(this);
    Operand *Src1RFLo = SrcsLo.src1RF(this);
    Operand *Src1RFHi = ValueLo == ValueHi ? Src1RFLo : SrcsHi.src1RF(this);

    const bool UseRsb = TableIcmp64[Index].Swapped != SrcsLo.swappedOperands();

    if (UseRsb) {
      if (TableIcmp64[Index].IsSigned) {
        Variable *T = makeReg(IceType_i32);
        _rsbs(T, Src0RLo, Src1RFLo);
        Context.insert(InstFakeUse::create(Func, T));

        T = makeReg(IceType_i32);
        _rscs(T, Src0RHi, Src1RFHi);
        // We need to add a FakeUse here because liveness gets mad at us (Def
        // without Use.) Note that flag-setting instructions are considered to
        // have side effects and, therefore, are not DCE'ed.
        Context.insert(InstFakeUse::create(Func, T));
      } else {
        Variable *T = makeReg(IceType_i32);
        _rsbs(T, Src0RHi, Src1RFHi);
        Context.insert(InstFakeUse::create(Func, T));

        T = makeReg(IceType_i32);
        _rsbs(T, Src0RLo, Src1RFLo, CondARM32::EQ);
        Context.insert(InstFakeUse::create(Func, T));
      }
    } else {
      if (TableIcmp64[Index].IsSigned) {
        _cmp(Src0RLo, Src1RFLo);
        Variable *T = makeReg(IceType_i32);
        _sbcs(T, Src0RHi, Src1RFHi);
        Context.insert(InstFakeUse::create(Func, T));
      } else {
        _cmp(Src0RHi, Src1RFHi);
        _cmp(Src0RLo, Src1RFLo, CondARM32::EQ);
      }
    }

    return CondWhenTrue(TableIcmp64[Index].C1);
  }

  Variable *Src0RLo, *Src0RHi;
  Operand *Src1RFLo, *Src1RFHi;
  if (TableIcmp64[Index].Swapped) {
    Src0RLo = legalizeToReg(loOperand(Src1));
    Src0RHi = legalizeToReg(hiOperand(Src1));
    Src1RFLo = legalizeToReg(loOperand(Src0));
    Src1RFHi = legalizeToReg(hiOperand(Src0));
  } else {
    Src0RLo = legalizeToReg(loOperand(Src0));
    Src0RHi = legalizeToReg(hiOperand(Src0));
    Src1RFLo = legalizeToReg(loOperand(Src1));
    Src1RFHi = legalizeToReg(hiOperand(Src1));
  }

  // a=icmp cond, b, c ==>
  // GCC does:
  //   cmp      b.hi, c.hi     or  cmp      b.lo, c.lo
  //   cmp.eq   b.lo, c.lo         sbcs t1, b.hi, c.hi
  //   mov.<C1> t, #1              mov.<C1> t, #1
  //   mov.<C2> t, #0              mov.<C2> t, #0
  //   mov      a, t               mov      a, t
  // where the "cmp.eq b.lo, c.lo" is used for unsigned and "sbcs t1, hi, hi"
  // is used for signed compares. In some cases, b and c need to be swapped as
  // well.
  //
  // LLVM does:
  // for EQ and NE:
  //   eor  t1, b.hi, c.hi
  //   eor  t2, b.lo, c.hi
  //   orrs t, t1, t2
  //   mov.<C> t, #1
  //   mov  a, t
  //
  // that's nice in that it's just as short but has fewer dependencies for
  // better ILP at the cost of more registers.
  //
  // Otherwise for signed/unsigned <, <=, etc. LLVM uses a sequence with two
  // unconditional mov #0, two cmps, two conditional mov #1, and one
  // conditional reg mov. That has few dependencies for good ILP, but is a
  // longer sequence.
  //
  // So, we are going with the GCC version since it's usually better (except
  // perhaps for eq/ne). We could revisit special-casing eq/ne later.
  if (TableIcmp64[Index].IsSigned) {
    Variable *ScratchReg = makeReg(IceType_i32);
    _cmp(Src0RLo, Src1RFLo);
    _sbcs(ScratchReg, Src0RHi, Src1RFHi);
    // ScratchReg isn't going to be used, but we need the side-effect of
    // setting flags from this operation.
    Context.insert(InstFakeUse::create(Func, ScratchReg));
  } else {
    _cmp(Src0RHi, Src1RFHi);
    _cmp(Src0RLo, Src1RFLo, CondARM32::EQ);
  }
  return CondWhenTrue(TableIcmp64[Index].C1);
}

TargetARM32::CondWhenTrue
TargetARM32::lowerInt32IcmpCond(InstIcmp::ICond Condition, Operand *Src0,
                                Operand *Src1) {
  Int32Operands Srcs(Src0, Src1);
  if (!Srcs.hasConstOperand()) {

    Variable *Src0R = Srcs.src0R(this);
    Operand *Src1RF = Srcs.src1RF(this);
    _cmp(Src0R, Src1RF);
    return CondWhenTrue(getIcmp32Mapping(Condition));
  }

  Variable *Src0R = Srcs.src0R(this);
  const int32_t Value = Srcs.getConstantValue();
  if ((Condition == InstIcmp::Eq || Condition == InstIcmp::Ne) && Value == 0) {
    _tst(Src0R, Src0R);
    return CondWhenTrue(getIcmp32Mapping(Condition));
  }

  if (!Srcs.swappedOperands() && !Srcs.immediateIsFlexEncodable() &&
      Srcs.negatedImmediateIsFlexEncodable()) {
    Operand *Src1F = Srcs.negatedSrc1F(this);
    _cmn(Src0R, Src1F);
    return CondWhenTrue(getIcmp32Mapping(Condition));
  }

  Operand *Src1RF = Srcs.src1RF(this);
  if (!Srcs.swappedOperands()) {
    _cmp(Src0R, Src1RF);
  } else {
    Variable *T = makeReg(IceType_i32);
    _rsbs(T, Src0R, Src1RF);
    Context.insert(InstFakeUse::create(Func, T));
  }
  return CondWhenTrue(getIcmp32Mapping(Condition));
}

TargetARM32::CondWhenTrue
TargetARM32::lowerInt8AndInt16IcmpCond(InstIcmp::ICond Condition, Operand *Src0,
                                       Operand *Src1) {
  Int32Operands Srcs(Src0, Src1);
  const int32_t ShAmt = 32 - getScalarIntBitWidth(Src0->getType());
  assert(ShAmt >= 0);

  if (!Srcs.hasConstOperand()) {
    Variable *Src0R = makeReg(IceType_i32);
    Operand *ShAmtImm = shAmtImm(ShAmt);
    _lsl(Src0R, legalizeToReg(Src0), ShAmtImm);

    Variable *Src1R = legalizeToReg(Src1);
    OperandARM32FlexReg *Src1F = OperandARM32FlexReg::create(
        Func, IceType_i32, Src1R, OperandARM32::LSL, ShAmtImm);
    _cmp(Src0R, Src1F);
    return CondWhenTrue(getIcmp32Mapping(Condition));
  }

  const int32_t Value = Srcs.getConstantValue();
  if ((Condition == InstIcmp::Eq || Condition == InstIcmp::Ne) && Value == 0) {
    Operand *ShAmtImm = shAmtImm(ShAmt);
    Variable *T = makeReg(IceType_i32);
    _lsls(T, Srcs.src0R(this), ShAmtImm);
    Context.insert(InstFakeUse::create(Func, T));
    return CondWhenTrue(getIcmp32Mapping(Condition));
  }

  Variable *ConstR = makeReg(IceType_i32);
  _mov(ConstR,
       legalize(Ctx->getConstantInt32(Value << ShAmt), Legal_Reg | Legal_Flex));
  Operand *NonConstF = OperandARM32FlexReg::create(
      Func, IceType_i32, Srcs.src0R(this), OperandARM32::LSL,
      Ctx->getConstantInt32(ShAmt));

  if (Srcs.swappedOperands()) {
    _cmp(ConstR, NonConstF);
  } else {
    Variable *T = makeReg(IceType_i32);
    _rsbs(T, ConstR, NonConstF);
    Context.insert(InstFakeUse::create(Func, T));
  }
  return CondWhenTrue(getIcmp32Mapping(Condition));
}

TargetARM32::CondWhenTrue TargetARM32::lowerIcmpCond(const InstIcmp *Inst) {
  assert(Inst->getSrc(0)->getType() != IceType_i1);
  assert(Inst->getSrc(1)->getType() != IceType_i1);

  Operand *Src0 = legalizeUndef(Inst->getSrc(0));
  Operand *Src1 = legalizeUndef(Inst->getSrc(1));

  const InstIcmp::ICond Condition = Inst->getCondition();
  // a=icmp cond b, c ==>
  // GCC does:
  //   <u/s>xtb tb, b
  //   <u/s>xtb tc, c
  //   cmp      tb, tc
  //   mov.C1   t, #0
  //   mov.C2   t, #1
  //   mov      a, t
  // where the unsigned/sign extension is not needed for 32-bit. They also have
  // special cases for EQ and NE. E.g., for NE:
  //   <extend to tb, tc>
  //   subs     t, tb, tc
  //   movne    t, #1
  //   mov      a, t
  //
  // LLVM does:
  //   lsl     tb, b, #<N>
  //   mov     t, #0
  //   cmp     tb, c, lsl #<N>
  //   mov.<C> t, #1
  //   mov     a, t
  //
  // the left shift is by 0, 16, or 24, which allows the comparison to focus on
  // the digits that actually matter (for 16-bit or 8-bit signed/unsigned). For
  // the unsigned case, for some reason it does similar to GCC and does a uxtb
  // first. It's not clear to me why that special-casing is needed.
  //
  // We'll go with the LLVM way for now, since it's shorter and has just as few
  // dependencies.
  switch (Src0->getType()) {
  default:
    llvm::report_fatal_error("Unhandled type in lowerIcmpCond");
  case IceType_i8:
  case IceType_i16:
    return lowerInt8AndInt16IcmpCond(Condition, Src0, Src1);
  case IceType_i32:
    return lowerInt32IcmpCond(Condition, Src0, Src1);
  case IceType_i64:
    return lowerInt64IcmpCond(Condition, Src0, Src1);
  }
}

void TargetARM32::lowerIcmp(const InstIcmp *Inst) {
  Variable *Dest = Inst->getDest();

  if (isVectorType(Dest->getType())) {
    Variable *T = makeReg(Dest->getType());
    Context.insert(InstFakeDef::create(Func, T));
    _mov(Dest, T);
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }

  Operand *_0 =
      legalize(Ctx->getConstantZero(IceType_i32), Legal_Reg | Legal_Flex);
  Operand *_1 = legalize(Ctx->getConstantInt32(1), Legal_Reg | Legal_Flex);
  Variable *T = makeReg(IceType_i1);

  _mov(T, _0);
  CondWhenTrue Cond = lowerIcmpCond(Inst);
  _mov_redefined(T, _1, Cond.WhenTrue0);
  _mov(Dest, T);

  assert(Cond.WhenTrue1 == CondARM32::kNone);

  return;
}

void TargetARM32::lowerInsertElement(const InstInsertElement *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

namespace {
inline uint64_t getConstantMemoryOrder(Operand *Opnd) {
  if (auto *Integer = llvm::dyn_cast<ConstantInteger32>(Opnd))
    return Integer->getValue();
  return Intrinsics::MemoryOrderInvalid;
}
} // end of anonymous namespace

void TargetARM32::lowerAtomicRMW(Variable *Dest, uint32_t Operation,
                                 Operand *Ptr, Operand *Val) {
  // retry:
  //     ldrex contents, [addr]
  //     op tmp, contents, operand
  //     strex success, tmp, [addr]
  //     jne retry
  //     fake-use(addr, operand)  @ prevents undesirable clobbering.
  //     mov dest, contents
  assert(Dest != nullptr);
  Type DestTy = Dest->getType();
  (void)Ptr;
  (void)Val;

  OperandARM32Mem *Mem;
  Variable *PtrContentsReg;
  Variable *PtrContentsHiReg;
  Variable *PtrContentsLoReg;
  Variable *Value = Func->makeVariable(DestTy);
  Variable *ValueReg;
  Variable *ValueHiReg;
  Variable *ValueLoReg;
  Variable *Success = makeReg(IceType_i32);
  Variable *TmpReg;
  Variable *TmpHiReg;
  Variable *TmpLoReg;
  Operand *_0 = Ctx->getConstantZero(IceType_i32);
  InstARM32Label *Retry = InstARM32Label::create(Func, this);

  if (DestTy == IceType_i64) {
    Variable64On32 *PtrContentsReg64 = makeI64RegPair();
    PtrContentsHiReg = PtrContentsReg64->getHi();
    PtrContentsLoReg = PtrContentsReg64->getLo();
    PtrContentsReg = PtrContentsReg64;

    llvm::cast<Variable64On32>(Value)->initHiLo(Func);
    Variable64On32 *ValueReg64 = makeI64RegPair();
    ValueHiReg = ValueReg64->getHi();
    ValueLoReg = ValueReg64->getLo();
    ValueReg = ValueReg64;

    Variable64On32 *TmpReg64 = makeI64RegPair();
    TmpHiReg = TmpReg64->getHi();
    TmpLoReg = TmpReg64->getLo();
    TmpReg = TmpReg64;
  } else {
    PtrContentsReg = makeReg(DestTy);
    PtrContentsHiReg = nullptr;
    PtrContentsLoReg = PtrContentsReg;

    ValueReg = makeReg(DestTy);
    ValueHiReg = nullptr;
    ValueLoReg = ValueReg;

    TmpReg = makeReg(DestTy);
    TmpHiReg = nullptr;
    TmpLoReg = TmpReg;
  }

  if (DestTy == IceType_i64) {
    Context.insert(InstFakeDef::create(Func, Value));
  }
  lowerAssign(InstAssign::create(Func, Value, Val));

  Variable *PtrVar = Func->makeVariable(IceType_i32);
  lowerAssign(InstAssign::create(Func, PtrVar, Ptr));

  _dmb();
  Context.insert(Retry);
  Mem = formMemoryOperand(PtrVar, DestTy);
  if (DestTy == IceType_i64) {
    Context.insert(InstFakeDef::create(Func, ValueReg, Value));
  }
  lowerAssign(InstAssign::create(Func, ValueReg, Value));
  if (DestTy == IceType_i8 || DestTy == IceType_i16) {
    _uxt(ValueReg, ValueReg);
  }
  _ldrex(PtrContentsReg, Mem);

  if (DestTy == IceType_i64) {
    Context.insert(InstFakeDef::create(Func, TmpReg, ValueReg));
  }
  switch (Operation) {
  default:
    Func->setError("Unknown AtomicRMW operation");
    return;
  case Intrinsics::AtomicAdd:
    if (DestTy == IceType_i64) {
      _adds(TmpLoReg, PtrContentsLoReg, ValueLoReg);
      _adc(TmpHiReg, PtrContentsHiReg, ValueHiReg);
    } else {
      _add(TmpLoReg, PtrContentsLoReg, ValueLoReg);
    }
    break;
  case Intrinsics::AtomicSub:
    if (DestTy == IceType_i64) {
      _subs(TmpLoReg, PtrContentsLoReg, ValueLoReg);
      _sbc(TmpHiReg, PtrContentsHiReg, ValueHiReg);
    } else {
      _sub(TmpLoReg, PtrContentsLoReg, ValueLoReg);
    }
    break;
  case Intrinsics::AtomicOr:
    _orr(TmpLoReg, PtrContentsLoReg, ValueLoReg);
    if (DestTy == IceType_i64) {
      _orr(TmpHiReg, PtrContentsHiReg, ValueHiReg);
    }
    break;
  case Intrinsics::AtomicAnd:
    _and(TmpLoReg, PtrContentsLoReg, ValueLoReg);
    if (DestTy == IceType_i64) {
      _and(TmpHiReg, PtrContentsHiReg, ValueHiReg);
    }
    break;
  case Intrinsics::AtomicXor:
    _eor(TmpLoReg, PtrContentsLoReg, ValueLoReg);
    if (DestTy == IceType_i64) {
      _eor(TmpHiReg, PtrContentsHiReg, ValueHiReg);
    }
    break;
  case Intrinsics::AtomicExchange:
    _mov(TmpLoReg, ValueLoReg);
    if (DestTy == IceType_i64) {
      _mov(TmpHiReg, ValueHiReg);
    }
    break;
  }
  _strex(Success, TmpReg, Mem);
  _cmp(Success, _0);
  _br(Retry, CondARM32::NE);

  // The following fake-uses ensure that Subzero will not clobber them in the
  // load-linked/store-conditional loop above. We might have to spill them, but
  // spilling is preferable over incorrect behavior.
  Context.insert(InstFakeUse::create(Func, PtrVar));
  if (auto *Value64 = llvm::dyn_cast<Variable64On32>(Value)) {
    Context.insert(InstFakeUse::create(Func, Value64->getHi()));
    Context.insert(InstFakeUse::create(Func, Value64->getLo()));
  } else {
    Context.insert(InstFakeUse::create(Func, Value));
  }
  _dmb();
  if (DestTy == IceType_i8 || DestTy == IceType_i16) {
    _uxt(PtrContentsReg, PtrContentsReg);
  }

  if (DestTy == IceType_i64) {
    Context.insert(InstFakeUse::create(Func, PtrContentsReg));
  }
  lowerAssign(InstAssign::create(Func, Dest, PtrContentsReg));
  if (auto *Dest64 = llvm::dyn_cast<Variable64On32>(Dest)) {
    Context.insert(InstFakeUse::create(Func, Dest64->getLo()));
    Context.insert(InstFakeUse::create(Func, Dest64->getHi()));
  } else {
    Context.insert(InstFakeUse::create(Func, Dest));
  }
}

void TargetARM32::lowerIntrinsicCall(const InstIntrinsicCall *Instr) {
  Variable *Dest = Instr->getDest();
  Type DestTy = (Dest != nullptr) ? Dest->getType() : IceType_void;
  Intrinsics::IntrinsicID ID = Instr->getIntrinsicInfo().ID;
  switch (ID) {
  case Intrinsics::AtomicFence:
  case Intrinsics::AtomicFenceAll:
    assert(Dest == nullptr);
    _dmb();
    return;
  case Intrinsics::AtomicIsLockFree: {
    Operand *ByteSize = Instr->getArg(0);
    auto *CI = llvm::dyn_cast<ConstantInteger32>(ByteSize);
    if (CI == nullptr) {
      // The PNaCl ABI requires the byte size to be a compile-time constant.
      Func->setError("AtomicIsLockFree byte size should be compile-time const");
      return;
    }
    static constexpr int32_t NotLockFree = 0;
    static constexpr int32_t LockFree = 1;
    int32_t Result = NotLockFree;
    switch (CI->getValue()) {
    case 1:
    case 2:
    case 4:
    case 8:
      Result = LockFree;
      break;
    }
    _mov(Dest, legalizeToReg(Ctx->getConstantInt32(Result)));
    return;
  }
  case Intrinsics::AtomicLoad: {
    assert(isScalarIntegerType(DestTy));
    // We require the memory address to be naturally aligned. Given that is the
    // case, then normal loads are atomic.
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(1)))) {
      Func->setError("Unexpected memory ordering for AtomicLoad");
      return;
    }
    Variable *T;

    if (DestTy == IceType_i64) {
      // ldrex is the only arm instruction that is guaranteed to load a 64-bit
      // integer atomically. Everything else works with a regular ldr.
      T = makeI64RegPair();
      _ldrex(T, formMemoryOperand(Instr->getArg(0), IceType_i64));
    } else {
      T = makeReg(DestTy);
      _ldr(T, formMemoryOperand(Instr->getArg(0), DestTy));
    }
    _dmb();
    lowerAssign(InstAssign::create(Func, Dest, T));
    // Make sure the atomic load isn't elided when unused, by adding a FakeUse.
    // Since lowerLoad may fuse the load w/ an arithmetic instruction, insert
    // the FakeUse on the last-inserted instruction's dest.
    Context.insert(
        InstFakeUse::create(Func, Context.getLastInserted()->getDest()));
    return;
  }
  case Intrinsics::AtomicStore: {
    // We require the memory address to be naturally aligned. Given that is the
    // case, then normal loads are atomic.
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(2)))) {
      Func->setError("Unexpected memory ordering for AtomicStore");
      return;
    }
    Operand *Value = Instr->getArg(0);
    Type ValueTy = Value->getType();
    assert(isScalarIntegerType(ValueTy));
    Operand *Addr = Instr->getArg(1);

    if (ValueTy == IceType_i64) {
      // Atomic 64-bit stores require a load-locked/store-conditional loop using
      // ldrexd, and strexd. The lowered code is:
      //
      // retry:
      //     ldrexd t.lo, t.hi, [addr]
      //     strexd success, value.lo, value.hi, [addr]
      //     cmp success, #0
      //     bne retry
      //     fake-use(addr, value.lo, value.hi)
      //
      // The fake-use is needed to prevent those variables from being clobbered
      // in the loop (which will happen under register pressure.)
      Variable64On32 *Tmp = makeI64RegPair();
      Variable64On32 *ValueVar =
          llvm::cast<Variable64On32>(Func->makeVariable(IceType_i64));
      Variable *AddrVar = makeReg(IceType_i32);
      Variable *Success = makeReg(IceType_i32);
      OperandARM32Mem *Mem;
      Operand *_0 = Ctx->getConstantZero(IceType_i32);
      InstARM32Label *Retry = InstARM32Label::create(Func, this);
      Variable64On32 *NewReg = makeI64RegPair();
      ValueVar->initHiLo(Func);
      ValueVar->mustNotHaveReg();

      _dmb();
      lowerAssign(InstAssign::create(Func, ValueVar, Value));
      lowerAssign(InstAssign::create(Func, AddrVar, Addr));

      Context.insert(Retry);
      Context.insert(InstFakeDef::create(Func, NewReg));
      lowerAssign(InstAssign::create(Func, NewReg, ValueVar));
      Mem = formMemoryOperand(AddrVar, IceType_i64);
      _ldrex(Tmp, Mem);
      // This fake-use both prevents the ldrex from being dead-code eliminated,
      // while also keeping liveness happy about all defs being used.
      Context.insert(
          InstFakeUse::create(Func, Context.getLastInserted()->getDest()));
      _strex(Success, NewReg, Mem);
      _cmp(Success, _0);
      _br(Retry, CondARM32::NE);

      Context.insert(InstFakeUse::create(Func, ValueVar->getLo()));
      Context.insert(InstFakeUse::create(Func, ValueVar->getHi()));
      Context.insert(InstFakeUse::create(Func, AddrVar));
      _dmb();
      return;
    }
    // non-64-bit stores are atomically as long as the address is aligned. This
    // is PNaCl, so addresses are aligned.
    Variable *T = makeReg(ValueTy);

    _dmb();
    lowerAssign(InstAssign::create(Func, T, Value));
    _str(T, formMemoryOperand(Addr, ValueTy));
    _dmb();
    return;
  }
  case Intrinsics::AtomicCmpxchg: {
    // The initial lowering for cmpxchg was:
    //
    // retry:
    //     ldrex tmp, [addr]
    //     cmp tmp, expected
    //     mov expected, tmp
    //     jne retry
    //     strex success, new, [addr]
    //     cmp success, #0
    //     bne retry
    //     mov dest, expected
    //
    // Besides requiring two branches, that lowering could also potentially
    // write to memory (in mov expected, tmp) unless we were OK with increasing
    // the register pressure and requiring expected to be an infinite-weight
    // variable (spoiler alert: that was a problem for i64 cmpxchg.) Through
    // careful rewritting, and thanks to predication, we now implement the
    // lowering as:
    //
    // retry:
    //     ldrex tmp, [addr]
    //     cmp tmp, expected
    //     strexeq success, new, [addr]
    //     movne expected, tmp
    //     cmpeq success, #0
    //     bne retry
    //     mov dest, expected
    //
    // Predication lets us move the strex ahead of the mov expected, tmp, which
    // allows tmp to be a non-infinite weight temporary. We wanted to avoid
    // writing to memory between ldrex and strex because, even though most times
    // that would cause no issues, if any interleaving memory write aliased
    // [addr] than we would have undefined behavior. Undefined behavior isn't
    // cool, so we try to avoid it. See the "Synchronization and semaphores"
    // section of the "ARM Architecture Reference Manual."

    assert(isScalarIntegerType(DestTy));
    // We require the memory address to be naturally aligned. Given that is the
    // case, then normal loads are atomic.
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(3)),
            getConstantMemoryOrder(Instr->getArg(4)))) {
      Func->setError("Unexpected memory ordering for AtomicCmpxchg");
      return;
    }

    OperandARM32Mem *Mem;
    Variable *TmpReg;
    Variable *Expected, *ExpectedReg;
    Variable *New, *NewReg;
    Variable *Success = makeReg(IceType_i32);
    Operand *_0 = Ctx->getConstantZero(IceType_i32);
    InstARM32Label *Retry = InstARM32Label::create(Func, this);

    if (DestTy == IceType_i64) {
      Variable64On32 *TmpReg64 = makeI64RegPair();
      Variable64On32 *New64 =
          llvm::cast<Variable64On32>(Func->makeVariable(IceType_i64));
      Variable64On32 *NewReg64 = makeI64RegPair();
      Variable64On32 *Expected64 =
          llvm::cast<Variable64On32>(Func->makeVariable(IceType_i64));
      Variable64On32 *ExpectedReg64 = makeI64RegPair();

      New64->initHiLo(Func);
      New64->mustNotHaveReg();
      Expected64->initHiLo(Func);
      Expected64->mustNotHaveReg();

      TmpReg = TmpReg64;
      New = New64;
      NewReg = NewReg64;
      Expected = Expected64;
      ExpectedReg = ExpectedReg64;
    } else {
      TmpReg = makeReg(DestTy);
      New = Func->makeVariable(DestTy);
      NewReg = makeReg(DestTy);
      Expected = Func->makeVariable(DestTy);
      ExpectedReg = makeReg(DestTy);
    }

    Mem = formMemoryOperand(Instr->getArg(0), DestTy);
    if (DestTy == IceType_i64) {
      Context.insert(InstFakeDef::create(Func, Expected));
    }
    lowerAssign(InstAssign::create(Func, Expected, Instr->getArg(1)));
    if (DestTy == IceType_i64) {
      Context.insert(InstFakeDef::create(Func, New));
    }
    lowerAssign(InstAssign::create(Func, New, Instr->getArg(2)));
    _dmb();

    Context.insert(Retry);
    if (DestTy == IceType_i64) {
      Context.insert(InstFakeDef::create(Func, ExpectedReg, Expected));
    }
    lowerAssign(InstAssign::create(Func, ExpectedReg, Expected));
    if (DestTy == IceType_i64) {
      Context.insert(InstFakeDef::create(Func, NewReg, New));
    }
    lowerAssign(InstAssign::create(Func, NewReg, New));

    _ldrex(TmpReg, Mem);
    Context.insert(
        InstFakeUse::create(Func, Context.getLastInserted()->getDest()));
    if (DestTy == IceType_i64) {
      auto *TmpReg64 = llvm::cast<Variable64On32>(TmpReg);
      auto *ExpectedReg64 = llvm::cast<Variable64On32>(ExpectedReg);
      // lowerAssign above has added fake-defs for TmpReg and ExpectedReg. Let's
      // keep liveness happy, shall we?
      Context.insert(InstFakeUse::create(Func, TmpReg));
      Context.insert(InstFakeUse::create(Func, ExpectedReg));
      _cmp(TmpReg64->getHi(), ExpectedReg64->getHi());
      _cmp(TmpReg64->getLo(), ExpectedReg64->getLo(), CondARM32::EQ);
    } else {
      _cmp(TmpReg, ExpectedReg);
    }
    _strex(Success, NewReg, Mem, CondARM32::EQ);
    if (DestTy == IceType_i64) {
      auto *TmpReg64 = llvm::cast<Variable64On32>(TmpReg);
      auto *Expected64 = llvm::cast<Variable64On32>(Expected);
      _mov_redefined(Expected64->getHi(), TmpReg64->getHi(), CondARM32::NE);
      _mov_redefined(Expected64->getLo(), TmpReg64->getLo(), CondARM32::NE);
      auto *FakeDef = InstFakeDef::create(Func, Expected, TmpReg);
      Context.insert(FakeDef);
      FakeDef->setDestRedefined();
    } else {
      _mov_redefined(Expected, TmpReg, CondARM32::NE);
    }
    _cmp(Success, _0, CondARM32::EQ);
    _br(Retry, CondARM32::NE);
    _dmb();
    lowerAssign(InstAssign::create(Func, Dest, Expected));
    Context.insert(InstFakeUse::create(Func, Expected));
    if (auto *New64 = llvm::dyn_cast<Variable64On32>(New)) {
      Context.insert(InstFakeUse::create(Func, New64->getLo()));
      Context.insert(InstFakeUse::create(Func, New64->getHi()));
    } else {
      Context.insert(InstFakeUse::create(Func, New));
    }
    return;
  }
  case Intrinsics::AtomicRMW: {
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(3)))) {
      Func->setError("Unexpected memory ordering for AtomicRMW");
      return;
    }
    lowerAtomicRMW(
        Dest, static_cast<uint32_t>(
                  llvm::cast<ConstantInteger32>(Instr->getArg(0))->getValue()),
        Instr->getArg(1), Instr->getArg(2));
    return;
  }
  case Intrinsics::Bswap: {
    Operand *Val = Instr->getArg(0);
    Type Ty = Val->getType();
    if (Ty == IceType_i64) {
      Val = legalizeUndef(Val);
      Variable *Val_Lo = legalizeToReg(loOperand(Val));
      Variable *Val_Hi = legalizeToReg(hiOperand(Val));
      Variable *T_Lo = makeReg(IceType_i32);
      Variable *T_Hi = makeReg(IceType_i32);
      Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
      Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
      _rev(T_Lo, Val_Lo);
      _rev(T_Hi, Val_Hi);
      _mov(DestLo, T_Hi);
      _mov(DestHi, T_Lo);
    } else {
      assert(Ty == IceType_i32 || Ty == IceType_i16);
      Variable *ValR = legalizeToReg(Val);
      Variable *T = makeReg(Ty);
      _rev(T, ValR);
      if (Val->getType() == IceType_i16) {
        Operand *_16 = shAmtImm(16);
        _lsr(T, T, _16);
      }
      _mov(Dest, T);
    }
    return;
  }
  case Intrinsics::Ctpop: {
    Operand *Val = Instr->getArg(0);
    InstCall *Call = makeHelperCall(isInt32Asserting32Or64(Val->getType())
                                        ? H_call_ctpop_i32
                                        : H_call_ctpop_i64,
                                    Dest, 1);
    Call->addArg(Val);
    lowerCall(Call);
    // The popcount helpers always return 32-bit values, while the intrinsic's
    // signature matches some 64-bit platform's native instructions and expect
    // to fill a 64-bit reg. Thus, clear the upper bits of the dest just in
    // case the user doesn't do that in the IR or doesn't toss the bits via
    // truncate.
    if (Val->getType() == IceType_i64) {
      Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
      Constant *Zero = Ctx->getConstantZero(IceType_i32);
      Variable *T = makeReg(Zero->getType());
      _mov(T, Zero);
      _mov(DestHi, T);
    }
    return;
  }
  case Intrinsics::Ctlz: {
    // The "is zero undef" parameter is ignored and we always return a
    // well-defined value.
    Operand *Val = Instr->getArg(0);
    Variable *ValLoR;
    Variable *ValHiR = nullptr;
    if (Val->getType() == IceType_i64) {
      Val = legalizeUndef(Val);
      ValLoR = legalizeToReg(loOperand(Val));
      ValHiR = legalizeToReg(hiOperand(Val));
    } else {
      ValLoR = legalizeToReg(Val);
    }
    lowerCLZ(Dest, ValLoR, ValHiR);
    return;
  }
  case Intrinsics::Cttz: {
    // Essentially like Clz, but reverse the bits first.
    Operand *Val = Instr->getArg(0);
    Variable *ValLoR;
    Variable *ValHiR = nullptr;
    if (Val->getType() == IceType_i64) {
      Val = legalizeUndef(Val);
      ValLoR = legalizeToReg(loOperand(Val));
      ValHiR = legalizeToReg(hiOperand(Val));
      Variable *TLo = makeReg(IceType_i32);
      Variable *THi = makeReg(IceType_i32);
      _rbit(TLo, ValLoR);
      _rbit(THi, ValHiR);
      ValLoR = THi;
      ValHiR = TLo;
    } else {
      ValLoR = legalizeToReg(Val);
      Variable *T = makeReg(IceType_i32);
      _rbit(T, ValLoR);
      ValLoR = T;
    }
    lowerCLZ(Dest, ValLoR, ValHiR);
    return;
  }
  case Intrinsics::Fabs: {
    Type DestTy = Dest->getType();
    Variable *T = makeReg(DestTy);
    if (isVectorType(DestTy)) {
      // Add a fake def to keep liveness consistent in the meantime.
      Context.insert(InstFakeDef::create(Func, T));
      _mov(Dest, T);
      UnimplementedError(Func->getContext()->getFlags());
      return;
    }
    _vabs(T, legalizeToReg(Instr->getArg(0)));
    _mov(Dest, T);
    return;
  }
  case Intrinsics::Longjmp: {
    InstCall *Call = makeHelperCall(H_call_longjmp, nullptr, 2);
    Call->addArg(Instr->getArg(0));
    Call->addArg(Instr->getArg(1));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Memcpy: {
    // In the future, we could potentially emit an inline memcpy/memset, etc.
    // for intrinsic calls w/ a known length.
    InstCall *Call = makeHelperCall(H_call_memcpy, nullptr, 3);
    Call->addArg(Instr->getArg(0));
    Call->addArg(Instr->getArg(1));
    Call->addArg(Instr->getArg(2));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Memmove: {
    InstCall *Call = makeHelperCall(H_call_memmove, nullptr, 3);
    Call->addArg(Instr->getArg(0));
    Call->addArg(Instr->getArg(1));
    Call->addArg(Instr->getArg(2));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Memset: {
    // The value operand needs to be extended to a stack slot size because the
    // PNaCl ABI requires arguments to be at least 32 bits wide.
    Operand *ValOp = Instr->getArg(1);
    assert(ValOp->getType() == IceType_i8);
    Variable *ValExt = Func->makeVariable(stackSlotType());
    lowerCast(InstCast::create(Func, InstCast::Zext, ValExt, ValOp));
    // Technically, ARM has their own __aeabi_memset, but we can use plain
    // memset too. The value and size argument need to be flipped if we ever
    // decide to use __aeabi_memset.
    InstCall *Call = makeHelperCall(H_call_memset, nullptr, 3);
    Call->addArg(Instr->getArg(0));
    Call->addArg(ValExt);
    Call->addArg(Instr->getArg(2));
    lowerCall(Call);
    return;
  }
  case Intrinsics::NaClReadTP: {
    if (Ctx->getFlags().getUseSandboxing()) {
      UnimplementedError(Func->getContext()->getFlags());
    } else {
      InstCall *Call = makeHelperCall(H_call_read_tp, Dest, 0);
      lowerCall(Call);
    }
    return;
  }
  case Intrinsics::Setjmp: {
    InstCall *Call = makeHelperCall(H_call_setjmp, Dest, 1);
    Call->addArg(Instr->getArg(0));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Sqrt: {
    Variable *Src = legalizeToReg(Instr->getArg(0));
    Variable *T = makeReg(Dest->getType());
    _vsqrt(T, Src);
    _mov(Dest, T);
    return;
  }
  case Intrinsics::Stacksave: {
    Variable *SP = getPhysicalRegister(RegARM32::Reg_sp);
    _mov(Dest, SP);
    return;
  }
  case Intrinsics::Stackrestore: {
    Variable *SP = getPhysicalRegister(RegARM32::Reg_sp);
    Operand *Val = legalize(Instr->getArg(0), Legal_Reg | Legal_Flex);
    _mov_redefined(SP, Val);
    return;
  }
  case Intrinsics::Trap:
    _trap();
    return;
  case Intrinsics::UnknownIntrinsic:
    Func->setError("Should not be lowering UnknownIntrinsic");
    return;
  }
  return;
}

void TargetARM32::lowerCLZ(Variable *Dest, Variable *ValLoR, Variable *ValHiR) {
  Type Ty = Dest->getType();
  assert(Ty == IceType_i32 || Ty == IceType_i64);
  Variable *T = makeReg(IceType_i32);
  _clz(T, ValLoR);
  if (Ty == IceType_i64) {
    Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
    Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
    Operand *Zero =
        legalize(Ctx->getConstantZero(IceType_i32), Legal_Reg | Legal_Flex);
    Operand *ThirtyTwo =
        legalize(Ctx->getConstantInt32(32), Legal_Reg | Legal_Flex);
    _cmp(ValHiR, Zero);
    Variable *T2 = makeReg(IceType_i32);
    _add(T2, T, ThirtyTwo);
    _clz(T2, ValHiR, CondARM32::NE);
    // T2 is actually a source as well when the predicate is not AL (since it
    // may leave T2 alone). We use _set_dest_redefined to prolong the liveness
    // of T2 as if it was used as a source.
    _set_dest_redefined();
    _mov(DestLo, T2);
    Variable *T3 = makeReg(Zero->getType());
    _mov(T3, Zero);
    _mov(DestHi, T3);
    return;
  }
  _mov(Dest, T);
  return;
}

void TargetARM32::lowerLoad(const InstLoad *Load) {
  // A Load instruction can be treated the same as an Assign instruction, after
  // the source operand is transformed into an OperandARM32Mem operand.
  Type Ty = Load->getDest()->getType();
  Operand *Src0 = formMemoryOperand(Load->getSourceAddress(), Ty);
  Variable *DestLoad = Load->getDest();

  // TODO(jvoung): handled folding opportunities. Sign and zero extension can
  // be folded into a load.
  InstAssign *Assign = InstAssign::create(Func, DestLoad, Src0);
  lowerAssign(Assign);
}

namespace {
void dumpAddressOpt(const Cfg *Func, const Variable *Base, int32_t Offset,
                    const Variable *OffsetReg, int16_t OffsetRegShAmt,
                    const Inst *Reason) {
  if (!BuildDefs::dump())
    return;
  if (!Func->isVerbose(IceV_AddrOpt))
    return;
  OstreamLocker _(Func->getContext());
  Ostream &Str = Func->getContext()->getStrDump();
  Str << "Instruction: ";
  Reason->dumpDecorated(Func);
  Str << "  results in Base=";
  if (Base)
    Base->dump(Func);
  else
    Str << "<null>";
  Str << ", OffsetReg=";
  if (OffsetReg)
    OffsetReg->dump(Func);
  else
    Str << "<null>";
  Str << ", Shift=" << OffsetRegShAmt << ", Offset=" << Offset << "\n";
}

bool matchAssign(const VariablesMetadata *VMetadata, Variable **Var,
                 int32_t *Offset, const Inst **Reason) {
  // Var originates from Var=SrcVar ==> set Var:=SrcVar
  if (*Var == nullptr)
    return false;
  const Inst *VarAssign = VMetadata->getSingleDefinition(*Var);
  if (!VarAssign)
    return false;
  assert(!VMetadata->isMultiDef(*Var));
  if (!llvm::isa<InstAssign>(VarAssign))
    return false;

  Operand *SrcOp = VarAssign->getSrc(0);
  if (auto *SrcVar = llvm::dyn_cast<Variable>(SrcOp)) {
    if (!VMetadata->isMultiDef(SrcVar) ||
        // TODO: ensure SrcVar stays single-BB
        false) {
      *Var = SrcVar;
    } else if (auto *Const = llvm::dyn_cast<ConstantInteger32>(SrcOp)) {
      int32_t MoreOffset = Const->getValue();
      int32_t NewOffset = MoreOffset + *Offset;
      if (Utils::WouldOverflowAdd(*Offset, MoreOffset))
        return false;
      *Var = nullptr;
      *Offset += NewOffset;
    }

    *Reason = VarAssign;
    return true;
  }

  return false;
}

bool isAddOrSub(const Inst *Inst, InstArithmetic::OpKind *Kind) {
  if (const auto *Arith = llvm::dyn_cast<InstArithmetic>(Inst)) {
    switch (Arith->getOp()) {
    default:
      return false;
    case InstArithmetic::Add:
    case InstArithmetic::Sub:
      *Kind = Arith->getOp();
      return true;
    }
  }
  return false;
}

bool matchCombinedBaseIndex(const VariablesMetadata *VMetadata, Variable **Base,
                            Variable **OffsetReg, int32_t OffsetRegShamt,
                            const Inst **Reason) {
  // OffsetReg==nullptr && Base is Base=Var1+Var2 ==>
  //   set Base=Var1, OffsetReg=Var2, Shift=0
  if (*Base == nullptr)
    return false;
  if (*OffsetReg != nullptr)
    return false;
  (void)OffsetRegShamt;
  assert(OffsetRegShamt == 0);
  const Inst *BaseInst = VMetadata->getSingleDefinition(*Base);
  if (BaseInst == nullptr)
    return false;
  assert(!VMetadata->isMultiDef(*Base));
  if (BaseInst->getSrcSize() < 2)
    return false;
  auto *Var1 = llvm::dyn_cast<Variable>(BaseInst->getSrc(0));
  if (!Var1)
    return false;
  if (VMetadata->isMultiDef(Var1))
    return false;
  auto *Var2 = llvm::dyn_cast<Variable>(BaseInst->getSrc(1));
  if (!Var2)
    return false;
  if (VMetadata->isMultiDef(Var2))
    return false;
  InstArithmetic::OpKind _;
  if (!isAddOrSub(BaseInst, &_) ||
      // TODO: ensure Var1 and Var2 stay single-BB
      false)
    return false;
  *Base = Var1;
  *OffsetReg = Var2;
  // OffsetRegShamt is already 0.
  *Reason = BaseInst;
  return true;
}

bool matchShiftedOffsetReg(const VariablesMetadata *VMetadata,
                           Variable **OffsetReg, OperandARM32::ShiftKind *Kind,
                           int32_t *OffsetRegShamt, const Inst **Reason) {
  // OffsetReg is OffsetReg=Var*Const && log2(Const)+Shift<=32 ==>
  //   OffsetReg=Var, Shift+=log2(Const)
  // OffsetReg is OffsetReg=Var<<Const && Const+Shift<=32 ==>
  //   OffsetReg=Var, Shift+=Const
  // OffsetReg is OffsetReg=Var>>Const && Const-Shift>=-32 ==>
  //   OffsetReg=Var, Shift-=Const
  OperandARM32::ShiftKind NewShiftKind = OperandARM32::kNoShift;
  if (*OffsetReg == nullptr)
    return false;
  auto *IndexInst = VMetadata->getSingleDefinition(*OffsetReg);
  if (IndexInst == nullptr)
    return false;
  assert(!VMetadata->isMultiDef(*OffsetReg));
  if (IndexInst->getSrcSize() < 2)
    return false;
  auto *ArithInst = llvm::dyn_cast<InstArithmetic>(IndexInst);
  if (ArithInst == nullptr)
    return false;
  auto *Var = llvm::dyn_cast<Variable>(ArithInst->getSrc(0));
  if (Var == nullptr)
    return false;
  auto *Const = llvm::dyn_cast<ConstantInteger32>(ArithInst->getSrc(1));
  if (Const == nullptr) {
    assert(!llvm::isa<ConstantInteger32>(ArithInst->getSrc(0)));
    return false;
  }
  if (VMetadata->isMultiDef(Var) || Const->getType() != IceType_i32)
    return false;

  uint32_t NewShamt = -1;
  switch (ArithInst->getOp()) {
  default:
    return false;
  case InstArithmetic::Shl: {
    NewShiftKind = OperandARM32::LSL;
    NewShamt = Const->getValue();
    if (NewShamt > 31)
      return false;
  } break;
  case InstArithmetic::Lshr: {
    NewShiftKind = OperandARM32::LSR;
    NewShamt = Const->getValue();
    if (NewShamt > 31)
      return false;
  } break;
  case InstArithmetic::Ashr: {
    NewShiftKind = OperandARM32::ASR;
    NewShamt = Const->getValue();
    if (NewShamt > 31)
      return false;
  } break;
  case InstArithmetic::Udiv:
  case InstArithmetic::Mul: {
    const uint32_t UnsignedConst = Const->getValue();
    NewShamt = llvm::findFirstSet(UnsignedConst);
    if (NewShamt != llvm::findLastSet(UnsignedConst)) {
      // First bit set is not the same as the last bit set, so Const is not
      // a power of 2.
      return false;
    }
    NewShiftKind = ArithInst->getOp() == InstArithmetic::Udiv
                       ? OperandARM32::LSR
                       : OperandARM32::LSL;
  } break;
  }
  // Allowed "transitions":
  //   kNoShift -> * iff NewShamt < 31
  //   LSL -> LSL    iff NewShamt + OffsetRegShamt < 31
  //   LSR -> LSR    iff NewShamt + OffsetRegShamt < 31
  //   ASR -> ASR    iff NewShamt + OffsetRegShamt < 31
  if (*Kind != OperandARM32::kNoShift && *Kind != NewShiftKind) {
    return false;
  }
  const int32_t NewOffsetRegShamt = *OffsetRegShamt + NewShamt;
  if (NewOffsetRegShamt > 31)
    return false;
  *OffsetReg = Var;
  *OffsetRegShamt = NewOffsetRegShamt;
  *Kind = NewShiftKind;
  *Reason = IndexInst;
  return true;
}

bool matchOffsetBase(const VariablesMetadata *VMetadata, Variable **Base,
                     int32_t *Offset, const Inst **Reason) {
  // Base is Base=Var+Const || Base is Base=Const+Var ==>
  //   set Base=Var, Offset+=Const
  // Base is Base=Var-Const ==>
  //   set Base=Var, Offset-=Const
  if (*Base == nullptr)
    return false;
  const Inst *BaseInst = VMetadata->getSingleDefinition(*Base);
  if (BaseInst == nullptr) {
    return false;
  }
  assert(!VMetadata->isMultiDef(*Base));

  auto *ArithInst = llvm::dyn_cast<const InstArithmetic>(BaseInst);
  if (ArithInst == nullptr)
    return false;
  InstArithmetic::OpKind Kind;
  if (!isAddOrSub(ArithInst, &Kind))
    return false;
  bool IsAdd = Kind == InstArithmetic::Add;
  Operand *Src0 = ArithInst->getSrc(0);
  Operand *Src1 = ArithInst->getSrc(1);
  auto *Var0 = llvm::dyn_cast<Variable>(Src0);
  auto *Var1 = llvm::dyn_cast<Variable>(Src1);
  auto *Const0 = llvm::dyn_cast<ConstantInteger32>(Src0);
  auto *Const1 = llvm::dyn_cast<ConstantInteger32>(Src1);
  Variable *NewBase = nullptr;
  int32_t NewOffset = *Offset;

  if (Var0 == nullptr && Const0 == nullptr) {
    assert(llvm::isa<ConstantRelocatable>(Src0));
    return false;
  }

  if (Var1 == nullptr && Const1 == nullptr) {
    assert(llvm::isa<ConstantRelocatable>(Src1));
    return false;
  }

  if (Var0 && Var1)
    // TODO(jpp): merge base/index splitting into here.
    return false;
  if (!IsAdd && Var1)
    return false;
  if (Var0)
    NewBase = Var0;
  else if (Var1)
    NewBase = Var1;
  // Compute the updated constant offset.
  if (Const0) {
    int32_t MoreOffset = IsAdd ? Const0->getValue() : -Const0->getValue();
    if (Utils::WouldOverflowAdd(NewOffset, MoreOffset))
      return false;
    NewOffset += MoreOffset;
  }
  if (Const1) {
    int32_t MoreOffset = IsAdd ? Const1->getValue() : -Const1->getValue();
    if (Utils::WouldOverflowAdd(NewOffset, MoreOffset))
      return false;
    NewOffset += MoreOffset;
  }

  // Update the computed address parameters once we are sure optimization
  // is valid.
  *Base = NewBase;
  *Offset = NewOffset;
  *Reason = BaseInst;
  return true;
}
} // end of anonymous namespace

// ARM32 address modes:
//  ld/st i[8|16|32]: [reg], [reg +/- imm12], [pc +/- imm12],
//                    [reg +/- reg << shamt5]
//  ld/st f[32|64]  : [reg], [reg +/- imm8] , [pc +/- imm8]
//  ld/st vectors   : [reg]
//
// For now, we don't handle address modes with Relocatables.
namespace {
// MemTraits contains per-type valid address mode information.
#define X(tag, elementty, int_width, vec_width, sbits, ubits, rraddr, shaddr)  \
  static_assert(!(shaddr) || rraddr, "Check ICETYPEARM32_TABLE::" #tag);
ICETYPEARM32_TABLE
#undef X

static const struct {
  int32_t ValidImmMask;
  bool CanHaveImm;
  bool CanHaveIndex;
  bool CanHaveShiftedIndex;
} MemTraits[] = {
#define X(tag, elementty, int_width, vec_width, sbits, ubits, rraddr, shaddr)  \
  { (1 << ubits) - 1, (ubits) > 0, rraddr, shaddr, }                           \
  ,
    ICETYPEARM32_TABLE
#undef X
};
static constexpr SizeT MemTraitsSize = llvm::array_lengthof(MemTraits);
} // end of anonymous namespace

OperandARM32Mem *TargetARM32::formAddressingMode(Type Ty, Cfg *Func,
                                                 const Inst *LdSt,
                                                 Operand *Base) {
  assert(Base != nullptr);
  int32_t OffsetImm = 0;
  Variable *OffsetReg = nullptr;
  int32_t OffsetRegShamt = 0;
  OperandARM32::ShiftKind ShiftKind = OperandARM32::kNoShift;

  Func->resetCurrentNode();
  if (Func->isVerbose(IceV_AddrOpt)) {
    OstreamLocker _(Func->getContext());
    Ostream &Str = Func->getContext()->getStrDump();
    Str << "\nAddress mode formation:\t";
    LdSt->dumpDecorated(Func);
  }

  if (isVectorType(Ty))
    // vector loads and stores do not allow offsets, and only support the
    // "[reg]" addressing mode (the other supported modes are write back.)
    return nullptr;

  auto *BaseVar = llvm::dyn_cast<Variable>(Base);
  if (BaseVar == nullptr)
    return nullptr;

  (void)MemTraitsSize;
  assert(Ty < MemTraitsSize);
  auto *TypeTraits = &MemTraits[Ty];
  const bool CanHaveIndex = TypeTraits->CanHaveIndex;
  const bool CanHaveShiftedIndex = TypeTraits->CanHaveShiftedIndex;
  const bool CanHaveImm = TypeTraits->CanHaveImm;
  const int32_t ValidImmMask = TypeTraits->ValidImmMask;
  (void)ValidImmMask;
  assert(!CanHaveImm || ValidImmMask >= 0);

  const VariablesMetadata *VMetadata = Func->getVMetadata();
  const Inst *Reason = nullptr;

  do {
    if (Reason != nullptr) {
      dumpAddressOpt(Func, BaseVar, OffsetImm, OffsetReg, OffsetRegShamt,
                     Reason);
      Reason = nullptr;
    }

    if (matchAssign(VMetadata, &BaseVar, &OffsetImm, &Reason)) {
      continue;
    }

    if (CanHaveIndex &&
        matchAssign(VMetadata, &OffsetReg, &OffsetImm, &Reason)) {
      continue;
    }

    if (CanHaveIndex && matchCombinedBaseIndex(VMetadata, &BaseVar, &OffsetReg,
                                               OffsetRegShamt, &Reason)) {
      continue;
    }

    if (CanHaveShiftedIndex) {
      if (matchShiftedOffsetReg(VMetadata, &OffsetReg, &ShiftKind,
                                &OffsetRegShamt, &Reason)) {
        continue;
      }

      if ((OffsetRegShamt == 0) &&
          matchShiftedOffsetReg(VMetadata, &BaseVar, &ShiftKind,
                                &OffsetRegShamt, &Reason)) {
        std::swap(BaseVar, OffsetReg);
        continue;
      }
    }

    if (matchOffsetBase(VMetadata, &BaseVar, &OffsetImm, &Reason)) {
      continue;
    }
  } while (Reason);

  if (BaseVar == nullptr) {
    // [OffsetReg{, LSL Shamt}{, #OffsetImm}] is not legal in ARM, so we have to
    // legalize the addressing mode to [BaseReg, OffsetReg{, LSL Shamt}].
    // Instead of a zeroed BaseReg, we initialize it with OffsetImm:
    //
    // [OffsetReg{, LSL Shamt}{, #OffsetImm}] ->
    //     mov BaseReg, #OffsetImm
    //     use of [BaseReg, OffsetReg{, LSL Shamt}]
    //
    const Type PointerType = getPointerType();
    BaseVar = makeReg(PointerType);
    Context.insert(
        InstAssign::create(Func, BaseVar, Ctx->getConstantInt32(OffsetImm)));
    OffsetImm = 0;
  } else if (OffsetImm != 0) {
    // ARM Ldr/Str instructions have limited range immediates. The formation
    // loop above materialized an Immediate carelessly, so we ensure the
    // generated offset is sane.
    const int32_t PositiveOffset = OffsetImm > 0 ? OffsetImm : -OffsetImm;
    const InstArithmetic::OpKind Op =
        OffsetImm > 0 ? InstArithmetic::Add : InstArithmetic::Sub;

    if (!CanHaveImm || !isLegalMemOffset(Ty, OffsetImm) ||
        OffsetReg != nullptr) {
      if (OffsetReg == nullptr) {
        // We formed a [Base, #const] addressing mode which is not encodable in
        // ARM. There is little point in forming an address mode now if we don't
        // have an offset. Effectively, we would end up with something like
        //
        // [Base, #const] -> add T, Base, #const
        //                   use of [T]
        //
        // Which is exactly what we already have. So we just bite the bullet
        // here and don't form any address mode.
        return nullptr;
      }
      // We formed [Base, Offset {, LSL Amnt}, #const]. Oops. Legalize it to
      //
      // [Base, Offset, {LSL amount}, #const] ->
      //      add T, Base, #const
      //      use of [T, Offset {, LSL amount}]
      const Type PointerType = getPointerType();
      Variable *T = makeReg(PointerType);
      Context.insert(InstArithmetic::create(
          Func, Op, T, BaseVar, Ctx->getConstantInt32(PositiveOffset)));
      BaseVar = T;
      OffsetImm = 0;
    }
  }

  assert(BaseVar != nullptr);
  assert(OffsetImm == 0 || OffsetReg == nullptr);
  assert(OffsetReg == nullptr || CanHaveIndex);
  assert(OffsetImm < 0 ? (ValidImmMask & -OffsetImm) == -OffsetImm
                       : (ValidImmMask & OffsetImm) == OffsetImm);

  Variable *BaseR = makeReg(getPointerType());
  Context.insert(InstAssign::create(Func, BaseR, BaseVar));
  if (OffsetReg != nullptr) {
    Variable *OffsetR = makeReg(getPointerType());
    Context.insert(InstAssign::create(Func, OffsetR, OffsetReg));
    return OperandARM32Mem::create(Func, Ty, BaseR, OffsetR, ShiftKind,
                                   OffsetRegShamt);
  }

  return OperandARM32Mem::create(
      Func, Ty, BaseR,
      llvm::cast<ConstantInteger32>(Ctx->getConstantInt32(OffsetImm)));
}

void TargetARM32::doAddressOptLoad() {
  Inst *Instr = Context.getCur();
  assert(llvm::isa<InstLoad>(Instr));
  Variable *Dest = Instr->getDest();
  Operand *Addr = Instr->getSrc(0);
  if (OperandARM32Mem *Mem =
          formAddressingMode(Dest->getType(), Func, Instr, Addr)) {
    Instr->setDeleted();
    Context.insert(InstLoad::create(Func, Dest, Mem));
  }
}

void TargetARM32::randomlyInsertNop(float Probability,
                                    RandomNumberGenerator &RNG) {
  RandomNumberGeneratorWrapper RNGW(RNG);
  if (RNGW.getTrueWithProbability(Probability)) {
    UnimplementedError(Func->getContext()->getFlags());
  }
}

void TargetARM32::lowerPhi(const InstPhi * /*Inst*/) {
  Func->setError("Phi found in regular instruction list");
}

void TargetARM32::lowerRet(const InstRet *Inst) {
  Variable *Reg = nullptr;
  if (Inst->hasRetValue()) {
    Operand *Src0 = Inst->getRetValue();
    Type Ty = Src0->getType();
    if (Ty == IceType_i64) {
      Src0 = legalizeUndef(Src0);
      Variable *R0 = legalizeToReg(loOperand(Src0), RegARM32::Reg_r0);
      Variable *R1 = legalizeToReg(hiOperand(Src0), RegARM32::Reg_r1);
      Reg = R0;
      Context.insert(InstFakeUse::create(Func, R1));
    } else if (Ty == IceType_f32) {
      Variable *S0 = legalizeToReg(Src0, RegARM32::Reg_s0);
      Reg = S0;
    } else if (Ty == IceType_f64) {
      Variable *D0 = legalizeToReg(Src0, RegARM32::Reg_d0);
      Reg = D0;
    } else if (isVectorType(Src0->getType())) {
      Variable *Q0 = legalizeToReg(Src0, RegARM32::Reg_q0);
      Reg = Q0;
    } else {
      Operand *Src0F = legalize(Src0, Legal_Reg | Legal_Flex);
      Reg = makeReg(Src0F->getType(), RegARM32::Reg_r0);
      _mov(Reg, Src0F, CondARM32::AL);
    }
  }
  // Add a ret instruction even if sandboxing is enabled, because addEpilog
  // explicitly looks for a ret instruction as a marker for where to insert the
  // frame removal instructions. addEpilog is responsible for restoring the
  // "lr" register as needed prior to this ret instruction.
  _ret(getPhysicalRegister(RegARM32::Reg_lr), Reg);
  // Add a fake use of sp to make sure sp stays alive for the entire function.
  // Otherwise post-call sp adjustments get dead-code eliminated.
  // TODO: Are there more places where the fake use should be inserted? E.g.
  // "void f(int n){while(1) g(n);}" may not have a ret instruction.
  Variable *SP = getPhysicalRegister(RegARM32::Reg_sp);
  Context.insert(InstFakeUse::create(Func, SP));
}

void TargetARM32::lowerSelect(const InstSelect *Inst) {
  Variable *Dest = Inst->getDest();
  Type DestTy = Dest->getType();
  Operand *SrcT = Inst->getTrueOperand();
  Operand *SrcF = Inst->getFalseOperand();
  Operand *Condition = Inst->getCondition();

  if (isVectorType(DestTy)) {
    Variable *T = makeReg(DestTy);
    Context.insert(InstFakeDef::create(Func, T));
    _mov(Dest, T);
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }

  lowerInt1ForSelect(Dest, Condition, legalizeUndef(SrcT), legalizeUndef(SrcF));
}

void TargetARM32::lowerStore(const InstStore *Inst) {
  Operand *Value = Inst->getData();
  Operand *Addr = Inst->getAddr();
  OperandARM32Mem *NewAddr = formMemoryOperand(Addr, Value->getType());
  Type Ty = NewAddr->getType();

  if (Ty == IceType_i64) {
    Value = legalizeUndef(Value);
    Variable *ValueHi = legalizeToReg(hiOperand(Value));
    Variable *ValueLo = legalizeToReg(loOperand(Value));
    _str(ValueHi, llvm::cast<OperandARM32Mem>(hiOperand(NewAddr)));
    _str(ValueLo, llvm::cast<OperandARM32Mem>(loOperand(NewAddr)));
  } else {
    Variable *ValueR = legalizeToReg(Value);
    _str(ValueR, NewAddr);
  }
}

void TargetARM32::doAddressOptStore() {
  Inst *Instr = Context.getCur();
  assert(llvm::isa<InstStore>(Instr));
  Operand *Src = Instr->getSrc(0);
  Operand *Addr = Instr->getSrc(1);
  if (OperandARM32Mem *Mem =
          formAddressingMode(Src->getType(), Func, Instr, Addr)) {
    Instr->setDeleted();
    Context.insert(InstStore::create(Func, Src, Mem));
  }
}

void TargetARM32::lowerSwitch(const InstSwitch *Inst) {
  // This implements the most naive possible lowering.
  // cmp a,val[0]; jeq label[0]; cmp a,val[1]; jeq label[1]; ... jmp default
  Operand *Src0 = Inst->getComparison();
  SizeT NumCases = Inst->getNumCases();
  if (Src0->getType() == IceType_i64) {
    Src0 = legalizeUndef(Src0);
    Variable *Src0Lo = legalizeToReg(loOperand(Src0));
    Variable *Src0Hi = legalizeToReg(hiOperand(Src0));
    for (SizeT I = 0; I < NumCases; ++I) {
      Operand *ValueLo = Ctx->getConstantInt32(Inst->getValue(I));
      Operand *ValueHi = Ctx->getConstantInt32(Inst->getValue(I) >> 32);
      ValueLo = legalize(ValueLo, Legal_Reg | Legal_Flex);
      ValueHi = legalize(ValueHi, Legal_Reg | Legal_Flex);
      _cmp(Src0Lo, ValueLo);
      _cmp(Src0Hi, ValueHi, CondARM32::EQ);
      _br(Inst->getLabel(I), CondARM32::EQ);
    }
    _br(Inst->getLabelDefault());
    return;
  }

  Variable *Src0Var = legalizeToReg(Src0);
  // If Src0 is not an i32, we left shift it -- see the icmp lowering for the
  // reason.
  assert(Src0Var->mustHaveReg());
  const size_t ShiftAmt = 32 - getScalarIntBitWidth(Src0->getType());
  assert(ShiftAmt < 32);
  if (ShiftAmt > 0) {
    Operand *ShAmtImm = shAmtImm(ShiftAmt);
    Variable *T = makeReg(IceType_i32);
    _lsl(T, Src0Var, ShAmtImm);
    Src0Var = T;
  }

  for (SizeT I = 0; I < NumCases; ++I) {
    Operand *Value = Ctx->getConstantInt32(Inst->getValue(I) << ShiftAmt);
    Value = legalize(Value, Legal_Reg | Legal_Flex);
    _cmp(Src0Var, Value);
    _br(Inst->getLabel(I), CondARM32::EQ);
  }
  _br(Inst->getLabelDefault());
}

void TargetARM32::lowerUnreachable(const InstUnreachable * /*Inst*/) {
  _trap();
}

void TargetARM32::prelowerPhis() {
  PhiLowering::prelowerPhis32Bit<TargetARM32>(this, Context.getNode(), Func);
}

Variable *TargetARM32::makeVectorOfZeros(Type Ty, int32_t RegNum) {
  Variable *Reg = makeReg(Ty, RegNum);
  Context.insert(InstFakeDef::create(Func, Reg));
  UnimplementedError(Func->getContext()->getFlags());
  return Reg;
}

// Helper for legalize() to emit the right code to lower an operand to a
// register of the appropriate type.
Variable *TargetARM32::copyToReg(Operand *Src, int32_t RegNum) {
  Type Ty = Src->getType();
  Variable *Reg = makeReg(Ty, RegNum);
  if (auto *Mem = llvm::dyn_cast<OperandARM32Mem>(Src)) {
    _ldr(Reg, Mem);
  } else {
    _mov(Reg, Src);
  }
  return Reg;
}

Operand *TargetARM32::legalize(Operand *From, LegalMask Allowed,
                               int32_t RegNum) {
  Type Ty = From->getType();
  // Assert that a physical register is allowed. To date, all calls to
  // legalize() allow a physical register. Legal_Flex converts registers to the
  // right type OperandARM32FlexReg as needed.
  assert(Allowed & Legal_Reg);

  // Copied ipsis literis from TargetX86Base<Machine>.
  if (RegNum == Variable::NoRegister) {
    if (Variable *Subst = getContext().availabilityGet(From)) {
      // At this point we know there is a potential substitution available.
      if (!Subst->isRematerializable() && Subst->mustHaveReg() &&
          !Subst->hasReg()) {
        // At this point we know the substitution will have a register.
        if (From->getType() == Subst->getType()) {
          // At this point we know the substitution's register is compatible.
          return Subst;
        }
      }
    }
  }

  // Go through the various types of operands: OperandARM32Mem,
  // OperandARM32Flex, Constant, and Variable. Given the above assertion, if
  // type of operand is not legal (e.g., OperandARM32Mem and !Legal_Mem), we
  // can always copy to a register.
  if (auto *Mem = llvm::dyn_cast<OperandARM32Mem>(From)) {
    // Before doing anything with a Mem operand, we need to ensure that the
    // Base and Index components are in physical registers.
    Variable *Base = Mem->getBase();
    Variable *Index = Mem->getIndex();
    ConstantInteger32 *Offset = Mem->getOffset();
    assert(Index == nullptr || Offset == nullptr);
    Variable *RegBase = nullptr;
    Variable *RegIndex = nullptr;
    assert(Base);
    RegBase = legalizeToReg(Base);
    assert(Ty < MemTraitsSize);
    if (Index) {
      assert(Offset == nullptr);
      assert(MemTraits[Ty].CanHaveIndex);
      RegIndex = legalizeToReg(Index);
    }
    if (Offset && Offset->getValue() != 0) {
      assert(Index == nullptr);
      static constexpr bool ZeroExt = false;
      assert(MemTraits[Ty].CanHaveImm);
      if (!OperandARM32Mem::canHoldOffset(Ty, ZeroExt, Offset->getValue())) {
        llvm::report_fatal_error("Invalid memory offset.");
      }
    }

    // Create a new operand if there was a change.
    if (Base != RegBase || Index != RegIndex) {
      // There is only a reg +/- reg or reg + imm form.
      // Figure out which to re-create.
      if (RegIndex) {
        Mem = OperandARM32Mem::create(Func, Ty, RegBase, RegIndex,
                                      Mem->getShiftOp(), Mem->getShiftAmt(),
                                      Mem->getAddrMode());
      } else {
        Mem = OperandARM32Mem::create(Func, Ty, RegBase, Offset,
                                      Mem->getAddrMode());
      }
    }
    if (Allowed & Legal_Mem) {
      From = Mem;
    } else {
      Variable *Reg = makeReg(Ty, RegNum);
      _ldr(Reg, Mem);
      From = Reg;
    }
    return From;
  }

  if (auto *Flex = llvm::dyn_cast<OperandARM32Flex>(From)) {
    if (!(Allowed & Legal_Flex)) {
      if (auto *FlexReg = llvm::dyn_cast<OperandARM32FlexReg>(Flex)) {
        if (FlexReg->getShiftOp() == OperandARM32::kNoShift) {
          From = FlexReg->getReg();
          // Fall through and let From be checked as a Variable below, where it
          // may or may not need a register.
        } else {
          return copyToReg(Flex, RegNum);
        }
      } else {
        return copyToReg(Flex, RegNum);
      }
    } else {
      return From;
    }
  }

  if (llvm::isa<Constant>(From)) {
    if (llvm::isa<ConstantUndef>(From)) {
      From = legalizeUndef(From, RegNum);
      if (isVectorType(Ty))
        return From;
    }
    // There should be no constants of vector type (other than undef).
    assert(!isVectorType(Ty));
    bool CanBeFlex = Allowed & Legal_Flex;
    if (auto *C32 = llvm::dyn_cast<ConstantInteger32>(From)) {
      uint32_t RotateAmt;
      uint32_t Immed_8;
      uint32_t Value = static_cast<uint32_t>(C32->getValue());
      // Check if the immediate will fit in a Flexible second operand, if a
      // Flexible second operand is allowed. We need to know the exact value,
      // so that rules out relocatable constants. Also try the inverse and use
      // MVN if possible.
      if (CanBeFlex &&
          OperandARM32FlexImm::canHoldImm(Value, &RotateAmt, &Immed_8)) {
        return OperandARM32FlexImm::create(Func, Ty, Immed_8, RotateAmt);
      } else if (CanBeFlex && OperandARM32FlexImm::canHoldImm(
                                  ~Value, &RotateAmt, &Immed_8)) {
        auto InvertedFlex =
            OperandARM32FlexImm::create(Func, Ty, Immed_8, RotateAmt);
        Variable *Reg = makeReg(Ty, RegNum);
        _mvn(Reg, InvertedFlex);
        return Reg;
      } else {
        // Do a movw/movt to a register.
        Variable *Reg = makeReg(Ty, RegNum);
        uint32_t UpperBits = (Value >> 16) & 0xFFFF;
        _movw(Reg,
              UpperBits != 0 ? Ctx->getConstantInt32(Value & 0xFFFF) : C32);
        if (UpperBits != 0) {
          _movt(Reg, Ctx->getConstantInt32(UpperBits));
        }
        return Reg;
      }
    } else if (auto *C = llvm::dyn_cast<ConstantRelocatable>(From)) {
      Variable *Reg = makeReg(Ty, RegNum);
      _movw(Reg, C);
      _movt(Reg, C);
      return Reg;
    } else {
      assert(isScalarFloatingType(Ty));
      uint32_t ModifiedImm;
      if (OperandARM32FlexFpImm::canHoldImm(From, &ModifiedImm)) {
        Variable *T = makeReg(Ty, RegNum);
        _mov(T,
             OperandARM32FlexFpImm::create(Func, From->getType(), ModifiedImm));
        return T;
      }

      if (Ty == IceType_f64 && isFloatingPointZero(From)) {
        // Use T = T ^ T to load a 64-bit fp zero. This does not work for f32
        // because ARM does not have a veor instruction with S registers.
        Variable *T = makeReg(IceType_f64, RegNum);
        Context.insert(InstFakeDef::create(Func, T));
        _veor(T, T, T);
        return T;
      }

      // Load floats/doubles from literal pool.
      std::string Buffer;
      llvm::raw_string_ostream StrBuf(Buffer);
      llvm::cast<Constant>(From)->emitPoolLabel(StrBuf, Ctx);
      llvm::cast<Constant>(From)->setShouldBePooled(true);
      Constant *Offset = Ctx->getConstantSym(0, StrBuf.str(), true);
      Variable *BaseReg = makeReg(getPointerType());
      _movw(BaseReg, Offset);
      _movt(BaseReg, Offset);
      From = formMemoryOperand(BaseReg, Ty);
      return copyToReg(From, RegNum);
    }
  }

  if (auto *Var = llvm::dyn_cast<Variable>(From)) {
    if (Var->isRematerializable()) {
      // TODO(jpp): We don't need to rematerialize Var if legalize() was invoked
      // for a Variable in a Mem operand.
      Variable *T = makeReg(Var->getType(), RegNum);
      _mov(T, Var);
      return T;
    }
    // Check if the variable is guaranteed a physical register. This can happen
    // either when the variable is pre-colored or when it is assigned infinite
    // weight.
    bool MustHaveRegister = (Var->hasReg() || Var->mustHaveReg());
    // We need a new physical register for the operand if:
    //   Mem is not allowed and Var isn't guaranteed a physical
    //   register, or
    //   RegNum is required and Var->getRegNum() doesn't match.
    if ((!(Allowed & Legal_Mem) && !MustHaveRegister) ||
        (RegNum != Variable::NoRegister && RegNum != Var->getRegNum())) {
      From = copyToReg(From, RegNum);
    }
    return From;
  }
  llvm_unreachable("Unhandled operand kind in legalize()");

  return From;
}

/// Provide a trivial wrapper to legalize() for this common usage.
Variable *TargetARM32::legalizeToReg(Operand *From, int32_t RegNum) {
  return llvm::cast<Variable>(legalize(From, Legal_Reg, RegNum));
}

/// Legalize undef values to concrete values.
Operand *TargetARM32::legalizeUndef(Operand *From, int32_t RegNum) {
  Type Ty = From->getType();
  if (llvm::isa<ConstantUndef>(From)) {
    // Lower undefs to zero. Another option is to lower undefs to an
    // uninitialized register; however, using an uninitialized register results
    // in less predictable code.
    //
    // If in the future the implementation is changed to lower undef values to
    // uninitialized registers, a FakeDef will be needed:
    // Context.insert(InstFakeDef::create(Func, Reg)); This is in order to
    // ensure that the live range of Reg is not overestimated. If the constant
    // being lowered is a 64 bit value, then the result should be split and the
    // lo and hi components will need to go in uninitialized registers.
    if (isVectorType(Ty))
      return makeVectorOfZeros(Ty, RegNum);
    return Ctx->getConstantZero(Ty);
  }
  return From;
}

OperandARM32Mem *TargetARM32::formMemoryOperand(Operand *Operand, Type Ty) {
  OperandARM32Mem *Mem = llvm::dyn_cast<OperandARM32Mem>(Operand);
  // It may be the case that address mode optimization already creates an
  // OperandARM32Mem, so in that case it wouldn't need another level of
  // transformation.
  if (Mem) {
    return llvm::cast<OperandARM32Mem>(legalize(Mem));
  }
  // If we didn't do address mode optimization, then we only have a
  // base/offset to work with. ARM always requires a base register, so
  // just use that to hold the operand.
  Variable *BaseR = legalizeToReg(Operand);
  return OperandARM32Mem::create(
      Func, Ty, BaseR,
      llvm::cast<ConstantInteger32>(Ctx->getConstantZero(IceType_i32)));
}

Variable64On32 *TargetARM32::makeI64RegPair() {
  Variable64On32 *Reg =
      llvm::cast<Variable64On32>(Func->makeVariable(IceType_i64));
  Reg->setMustHaveReg();
  Reg->initHiLo(Func);
  Reg->getLo()->setMustNotHaveReg();
  Reg->getHi()->setMustNotHaveReg();
  return Reg;
}

Variable *TargetARM32::makeReg(Type Type, int32_t RegNum) {
  // There aren't any 64-bit integer registers for ARM32.
  assert(Type != IceType_i64);
  assert(AllowTemporaryWithNoReg || RegNum != Variable::NoRegister);
  Variable *Reg = Func->makeVariable(Type);
  if (RegNum == Variable::NoRegister)
    Reg->setMustHaveReg();
  else
    Reg->setRegNum(RegNum);
  return Reg;
}

void TargetARM32::alignRegisterPow2(Variable *Reg, uint32_t Align,
                                    int32_t TmpRegNum) {
  assert(llvm::isPowerOf2_32(Align));
  uint32_t RotateAmt;
  uint32_t Immed_8;
  Operand *Mask;
  // Use AND or BIC to mask off the bits, depending on which immediate fits (if
  // it fits at all). Assume Align is usually small, in which case BIC works
  // better. Thus, this rounds down to the alignment.
  if (OperandARM32FlexImm::canHoldImm(Align - 1, &RotateAmt, &Immed_8)) {
    Mask = legalize(Ctx->getConstantInt32(Align - 1), Legal_Reg | Legal_Flex,
                    TmpRegNum);
    _bic(Reg, Reg, Mask);
  } else {
    Mask = legalize(Ctx->getConstantInt32(-Align), Legal_Reg | Legal_Flex,
                    TmpRegNum);
    _and(Reg, Reg, Mask);
  }
}

void TargetARM32::postLower() {
  if (Ctx->getFlags().getOptLevel() == Opt_m1)
    return;
  markRedefinitions();
  Context.availabilityUpdate();
}

void TargetARM32::makeRandomRegisterPermutation(
    llvm::SmallVectorImpl<int32_t> &Permutation,
    const llvm::SmallBitVector &ExcludeRegisters, uint64_t Salt) const {
  (void)Permutation;
  (void)ExcludeRegisters;
  (void)Salt;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::emit(const ConstantInteger32 *C) const {
  if (!BuildDefs::dump())
    return;
  Ostream &Str = Ctx->getStrEmit();
  Str << getConstantPrefix() << C->getValue();
}

void TargetARM32::emit(const ConstantInteger64 *) const {
  llvm::report_fatal_error("Not expecting to emit 64-bit integers");
}

void TargetARM32::emit(const ConstantFloat *C) const {
  (void)C;
  UnimplementedError(Ctx->getFlags());
}

void TargetARM32::emit(const ConstantDouble *C) const {
  (void)C;
  UnimplementedError(Ctx->getFlags());
}

void TargetARM32::emit(const ConstantUndef *) const {
  llvm::report_fatal_error("undef value encountered by emitter.");
}

void TargetARM32::lowerInt1ForSelect(Variable *Dest, Operand *Boolean,
                                     Operand *TrueValue, Operand *FalseValue) {
  Operand *_1 = legalize(Ctx->getConstantInt1(1), Legal_Reg | Legal_Flex);

  assert(Boolean->getType() == IceType_i1);

  bool NeedsAnd1 = false;
  if (TrueValue->getType() == IceType_i1) {
    assert(FalseValue->getType() == IceType_i1);

    Variable *TrueValueV = Func->makeVariable(IceType_i1);
    SafeBoolChain Src0Safe = lowerInt1(TrueValueV, TrueValue);
    TrueValue = TrueValueV;

    Variable *FalseValueV = Func->makeVariable(IceType_i1);
    SafeBoolChain Src1Safe = lowerInt1(FalseValueV, FalseValue);
    FalseValue = FalseValueV;

    NeedsAnd1 = Src0Safe == SBC_No || Src1Safe == SBC_No;
  }

  Variable *DestLo = (Dest->getType() == IceType_i64)
                         ? llvm::cast<Variable>(loOperand(Dest))
                         : Dest;
  Variable *DestHi = (Dest->getType() == IceType_i64)
                         ? llvm::cast<Variable>(hiOperand(Dest))
                         : nullptr;
  Operand *FalseValueLo = (FalseValue->getType() == IceType_i64)
                              ? loOperand(FalseValue)
                              : FalseValue;
  Operand *FalseValueHi =
      (FalseValue->getType() == IceType_i64) ? hiOperand(FalseValue) : nullptr;

  Operand *TrueValueLo =
      (TrueValue->getType() == IceType_i64) ? loOperand(TrueValue) : TrueValue;
  Operand *TrueValueHi =
      (TrueValue->getType() == IceType_i64) ? hiOperand(TrueValue) : nullptr;

  Variable *T_Lo = makeReg(DestLo->getType());
  Variable *T_Hi = (DestHi == nullptr) ? nullptr : makeReg(DestHi->getType());

  _mov(T_Lo, legalize(FalseValueLo, Legal_Reg | Legal_Flex));
  if (DestHi) {
    _mov(T_Hi, legalize(FalseValueHi, Legal_Reg | Legal_Flex));
  }

  CondWhenTrue Cond(CondARM32::kNone);
  // FlagsWereSet is used to determine wether Boolean was folded or not. If not,
  // add an explicit _tst instruction below.
  bool FlagsWereSet = false;
  if (const Inst *Producer = BoolComputations.getProducerOf(Boolean)) {
    switch (Producer->getKind()) {
    default:
      llvm_unreachable("Unexpected producer.");
    case Inst::Icmp: {
      Cond = lowerIcmpCond(llvm::cast<InstIcmp>(Producer));
      FlagsWereSet = true;
    } break;
    case Inst::Fcmp: {
      Cond = lowerFcmpCond(llvm::cast<InstFcmp>(Producer));
      FlagsWereSet = true;
    } break;
    case Inst::Cast: {
      const auto *CastProducer = llvm::cast<InstCast>(Producer);
      assert(CastProducer->getCastKind() == InstCast::Trunc);
      Boolean = CastProducer->getSrc(0);
      // No flags were set, so a _tst(Src, 1) will be emitted below. Don't
      // bother legalizing Src to a Reg because it will be legalized before
      // emitting the tst instruction.
      FlagsWereSet = false;
    } break;
    case Inst::Arithmetic: {
      // This is a special case: we eagerly assumed Producer could be folded,
      // but in reality, it can't. No reason to panic: we just lower it using
      // the regular lowerArithmetic helper.
      const auto *ArithProducer = llvm::cast<InstArithmetic>(Producer);
      lowerArithmetic(ArithProducer);
      Boolean = ArithProducer->getDest();
      // No flags were set, so a _tst(Dest, 1) will be emitted below. Don't
      // bother legalizing Dest to a Reg because it will be legalized before
      // emitting  the tst instruction.
      FlagsWereSet = false;
    } break;
    }
  }

  if (!FlagsWereSet) {
    // No flags have been set, so emit a tst Boolean, 1.
    Variable *Src = legalizeToReg(Boolean);
    _tst(Src, _1);
    Cond = CondWhenTrue(CondARM32::NE); // i.e., CondARM32::NotZero.
  }

  if (Cond.WhenTrue0 == CondARM32::kNone) {
    assert(Cond.WhenTrue1 == CondARM32::kNone);
  } else {
    _mov_redefined(T_Lo, legalize(TrueValueLo, Legal_Reg | Legal_Flex),
                   Cond.WhenTrue0);
    if (DestHi) {
      _mov_redefined(T_Hi, legalize(TrueValueHi, Legal_Reg | Legal_Flex),
                     Cond.WhenTrue0);
    }
  }

  if (Cond.WhenTrue1 != CondARM32::kNone) {
    _mov_redefined(T_Lo, legalize(TrueValueLo, Legal_Reg | Legal_Flex),
                   Cond.WhenTrue1);
    if (DestHi) {
      _mov_redefined(T_Hi, legalize(TrueValueHi, Legal_Reg | Legal_Flex),
                     Cond.WhenTrue1);
    }
  }

  if (NeedsAnd1) {
    // We lowered something that is unsafe (i.e., can't provably be zero or
    // one). Truncate the result.
    _and(T_Lo, T_Lo, _1);
  }

  _mov(DestLo, T_Lo);
  if (DestHi) {
    _mov(DestHi, T_Hi);
  }
}

TargetARM32::SafeBoolChain TargetARM32::lowerInt1(Variable *Dest,
                                                  Operand *Boolean) {
  assert(Boolean->getType() == IceType_i1);
  Variable *T = makeReg(IceType_i1);
  Operand *_0 =
      legalize(Ctx->getConstantZero(IceType_i1), Legal_Reg | Legal_Flex);
  Operand *_1 = legalize(Ctx->getConstantInt1(1), Legal_Reg | Legal_Flex);

  SafeBoolChain Safe = SBC_Yes;
  if (const Inst *Producer = BoolComputations.getProducerOf(Boolean)) {
    switch (Producer->getKind()) {
    default:
      llvm_unreachable("Unexpected producer.");
    case Inst::Icmp: {
      _mov(T, _0);
      CondWhenTrue Cond = lowerIcmpCond(llvm::cast<InstIcmp>(Producer));
      assert(Cond.WhenTrue0 != CondARM32::AL);
      assert(Cond.WhenTrue0 != CondARM32::kNone);
      assert(Cond.WhenTrue1 == CondARM32::kNone);
      _mov_redefined(T, _1, Cond.WhenTrue0);
    } break;
    case Inst::Fcmp: {
      _mov(T, _0);
      Inst *MovZero = Context.getLastInserted();
      CondWhenTrue Cond = lowerFcmpCond(llvm::cast<InstFcmp>(Producer));
      if (Cond.WhenTrue0 == CondARM32::AL) {
        assert(Cond.WhenTrue1 == CondARM32::kNone);
        MovZero->setDeleted();
        _mov(T, _1);
      } else if (Cond.WhenTrue0 != CondARM32::kNone) {
        _mov_redefined(T, _1, Cond.WhenTrue0);
      }
      if (Cond.WhenTrue1 != CondARM32::kNone) {
        assert(Cond.WhenTrue0 != CondARM32::kNone);
        assert(Cond.WhenTrue0 != CondARM32::AL);
        _mov_redefined(T, _1, Cond.WhenTrue1);
      }
    } break;
    case Inst::Cast: {
      const auto *CastProducer = llvm::cast<InstCast>(Producer);
      assert(CastProducer->getCastKind() == InstCast::Trunc);
      Operand *Src = CastProducer->getSrc(0);
      if (Src->getType() == IceType_i64)
        Src = loOperand(Src);
      _mov(T, legalize(Src, Legal_Reg | Legal_Flex));
      Safe = SBC_No;
    } break;
    case Inst::Arithmetic: {
      const auto *ArithProducer = llvm::cast<InstArithmetic>(Producer);
      Safe = lowerInt1Arithmetic(ArithProducer);
      _mov(T, ArithProducer->getDest());
    } break;
    }
  } else {
    _mov(T, legalize(Boolean, Legal_Reg | Legal_Flex));
  }

  _mov(Dest, T);
  return Safe;
}

namespace {
namespace BoolFolding {
bool shouldTrackProducer(const Inst &Instr) {
  switch (Instr.getKind()) {
  default:
    return false;
  case Inst::Icmp:
  case Inst::Fcmp:
    return true;
  case Inst::Cast: {
    switch (llvm::cast<InstCast>(&Instr)->getCastKind()) {
    default:
      return false;
    case InstCast::Trunc:
      return true;
    }
  }
  case Inst::Arithmetic: {
    switch (llvm::cast<InstArithmetic>(&Instr)->getOp()) {
    default:
      return false;
    case InstArithmetic::And:
    case InstArithmetic::Or:
      return true;
    }
  }
  }
}

bool isValidConsumer(const Inst &Instr) {
  switch (Instr.getKind()) {
  default:
    return false;
  case Inst::Br:
    return true;
  case Inst::Select:
    return !isVectorType(Instr.getDest()->getType());
  case Inst::Cast: {
    switch (llvm::cast<InstCast>(&Instr)->getCastKind()) {
    default:
      return false;
    case InstCast::Sext:
      return !isVectorType(Instr.getDest()->getType());
    case InstCast::Zext:
      return !isVectorType(Instr.getDest()->getType());
    }
  }
  case Inst::Arithmetic: {
    switch (llvm::cast<InstArithmetic>(&Instr)->getOp()) {
    default:
      return false;
    case InstArithmetic::And:
      return !isVectorType(Instr.getDest()->getType());
    case InstArithmetic::Or:
      return !isVectorType(Instr.getDest()->getType());
    }
  }
  }
}
} // end of namespace BoolFolding
} // end of anonymous namespace

void TargetARM32::BoolComputationTracker::recordProducers(CfgNode *Node) {
  for (Inst &Instr : Node->getInsts()) {
    // Check whether Instr is a valid producer.
    Variable *Dest = Instr.getDest();
    if (!Instr.isDeleted() // only consider non-deleted instructions; and
        && Dest            // only instructions with an actual dest var; and
        && Dest->getType() == IceType_i1 // only bool-type dest vars; and
        && BoolFolding::shouldTrackProducer(Instr)) { // white-listed instr.
      KnownComputations.emplace(Dest->getIndex(), BoolComputationEntry(&Instr));
    }
    // Check each src variable against the map.
    FOREACH_VAR_IN_INST(Var, Instr) {
      SizeT VarNum = Var->getIndex();
      auto ComputationIter = KnownComputations.find(VarNum);
      if (ComputationIter == KnownComputations.end()) {
        continue;
      }

      ++ComputationIter->second.NumUses;
      if (!BoolFolding::isValidConsumer(Instr)) {
        KnownComputations.erase(VarNum);
        continue;
      }

      if (Instr.isLastUse(Var)) {
        ComputationIter->second.IsLiveOut = false;
      }
    }
  }

  for (auto Iter = KnownComputations.begin(), End = KnownComputations.end();
       Iter != End;) {
    // Disable the folding if its dest may be live beyond this block.
    if (Iter->second.IsLiveOut || Iter->second.NumUses > 1) {
      Iter = KnownComputations.erase(Iter);
      continue;
    }

    // Mark as "dead" rather than outright deleting. This is so that other
    // peephole style optimizations during or before lowering have access to
    // this instruction in undeleted form. See for example
    // tryOptimizedCmpxchgCmpBr().
    Iter->second.Instr->setDead();
    ++Iter;
  }
}

TargetDataARM32::TargetDataARM32(GlobalContext *Ctx)
    : TargetDataLowering(Ctx) {}

void TargetDataARM32::lowerGlobals(const VariableDeclarationList &Vars,
                                   const IceString &SectionSuffix) {
  switch (Ctx->getFlags().getOutFileType()) {
  case FT_Elf: {
    ELFObjectWriter *Writer = Ctx->getObjectWriter();
    Writer->writeDataSection(Vars, llvm::ELF::R_ARM_ABS32, SectionSuffix);
  } break;
  case FT_Asm:
  case FT_Iasm: {
    const IceString &TranslateOnly = Ctx->getFlags().getTranslateOnly();
    OstreamLocker _(Ctx);
    for (const VariableDeclaration *Var : Vars) {
      if (GlobalContext::matchSymbolName(Var->getName(), TranslateOnly)) {
        emitGlobal(*Var, SectionSuffix);
      }
    }
  } break;
  }
}

namespace {
template <typename T> struct ConstantPoolEmitterTraits;

static_assert(sizeof(uint64_t) == 8,
              "uint64_t is supposed to be 8 bytes wide.");

// TODO(jpp): implement the following when implementing constant randomization:
//  * template <> struct ConstantPoolEmitterTraits<uint8_t>
//  * template <> struct ConstantPoolEmitterTraits<uint16_t>
//  * template <> struct ConstantPoolEmitterTraits<uint32_t>
template <> struct ConstantPoolEmitterTraits<float> {
  using ConstantType = ConstantFloat;
  static constexpr Type IceType = IceType_f32;
  // AsmTag and TypeName can't be constexpr because llvm::StringRef is unhappy
  // about them being constexpr.
  static const char AsmTag[];
  static const char TypeName[];
  static uint64_t bitcastToUint64(float Value) {
    static_assert(sizeof(Value) == sizeof(uint32_t),
                  "Float should be 4 bytes.");
    uint32_t IntValue = *reinterpret_cast<uint32_t *>(&Value);
    return static_cast<uint64_t>(IntValue);
  }
};
const char ConstantPoolEmitterTraits<float>::AsmTag[] = ".long";
const char ConstantPoolEmitterTraits<float>::TypeName[] = "f32";

template <> struct ConstantPoolEmitterTraits<double> {
  using ConstantType = ConstantDouble;
  static constexpr Type IceType = IceType_f64;
  static const char AsmTag[];
  static const char TypeName[];
  static uint64_t bitcastToUint64(double Value) {
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "Double should be 8 bytes.");
    return *reinterpret_cast<uint64_t *>(&Value);
  }
};
const char ConstantPoolEmitterTraits<double>::AsmTag[] = ".quad";
const char ConstantPoolEmitterTraits<double>::TypeName[] = "f64";

template <typename T>
void emitConstant(
    Ostream &Str, const GlobalContext *Ctx,
    const typename ConstantPoolEmitterTraits<T>::ConstantType *Const) {
  using Traits = ConstantPoolEmitterTraits<T>;
  Const->emitPoolLabel(Str, Ctx);
  Str << ":\n\t" << Traits::AsmTag << "\t0x";
  T Value = Const->getValue();
  Str.write_hex(Traits::bitcastToUint64(Value));
  Str << "\t@" << Traits::TypeName << " " << Value << "\n";
}

template <typename T> void emitConstantPool(GlobalContext *Ctx) {
  if (!BuildDefs::dump()) {
    return;
  }

  using Traits = ConstantPoolEmitterTraits<T>;
  static constexpr size_t MinimumAlignment = 4;
  SizeT Align = std::max(MinimumAlignment, typeAlignInBytes(Traits::IceType));
  assert((Align % 4) == 0 && "Constants should be aligned");
  Ostream &Str = Ctx->getStrEmit();
  ConstantList Pool = Ctx->getConstantPool(Traits::IceType);

  Str << "\t.section\t.rodata.cst" << Align << ",\"aM\",%progbits," << Align
      << "\n"
      << "\t.align\t" << Align << "\n";

  if (Ctx->getFlags().shouldReorderPooledConstants()) {
    // TODO(jpp): add constant pooling.
    UnimplementedError(Ctx->getFlags());
  }

  for (Constant *C : Pool) {
    if (!C->getShouldBePooled()) {
      continue;
    }

    emitConstant<T>(Str, Ctx, llvm::dyn_cast<typename Traits::ConstantType>(C));
  }
}
} // end of anonymous namespace

void TargetDataARM32::lowerConstants() {
  if (Ctx->getFlags().getDisableTranslation())
    return;
  switch (Ctx->getFlags().getOutFileType()) {
  case FT_Elf:
    UnimplementedError(Ctx->getFlags());
    break;
  case FT_Asm:
  case FT_Iasm: {
    OstreamLocker _(Ctx);
    emitConstantPool<float>(Ctx);
    emitConstantPool<double>(Ctx);
    break;
  }
  }
}

void TargetDataARM32::lowerJumpTables() {
  if (Ctx->getFlags().getDisableTranslation())
    return;
  switch (Ctx->getFlags().getOutFileType()) {
  case FT_Elf:
    UnimplementedError(Ctx->getFlags());
    break;
  case FT_Asm:
    // Already emitted from Cfg
    break;
  case FT_Iasm: {
    // TODO(kschimpf): Fill this in when we get more information.
    break;
  }
  }
}

TargetHeaderARM32::TargetHeaderARM32(GlobalContext *Ctx)
    : TargetHeaderLowering(Ctx), CPUFeatures(Ctx->getFlags()) {}

void TargetHeaderARM32::lower() {
  OstreamLocker _(Ctx);
  Ostream &Str = Ctx->getStrEmit();
  Str << ".syntax unified\n";
  // Emit build attributes in format: .eabi_attribute TAG, VALUE. See Sec. 2 of
  // "Addenda to, and Errata in the ABI for the ARM architecture"
  // http://infocenter.arm.com
  //                  /help/topic/com.arm.doc.ihi0045d/IHI0045D_ABI_addenda.pdf
  //
  // Tag_conformance should be be emitted first in a file-scope sub-subsection
  // of the first public subsection of the attributes.
  Str << ".eabi_attribute 67, \"2.09\"      @ Tag_conformance\n";
  // Chromebooks are at least A15, but do A9 for higher compat. For some
  // reason, the LLVM ARM asm parser has the .cpu directive override the mattr
  // specified on the commandline. So to test hwdiv, we need to set the .cpu
  // directive higher (can't just rely on --mattr=...).
  if (CPUFeatures.hasFeature(TargetARM32Features::HWDivArm)) {
    Str << ".cpu    cortex-a15\n";
  } else {
    Str << ".cpu    cortex-a9\n";
  }
  Str << ".eabi_attribute 6, 10   @ Tag_CPU_arch: ARMv7\n"
      << ".eabi_attribute 7, 65   @ Tag_CPU_arch_profile: App profile\n";
  Str << ".eabi_attribute 8, 1    @ Tag_ARM_ISA_use: Yes\n"
      << ".eabi_attribute 9, 2    @ Tag_THUMB_ISA_use: Thumb-2\n";
  Str << ".fpu    neon\n"
      << ".eabi_attribute 17, 1   @ Tag_ABI_PCS_GOT_use: permit directly\n"
      << ".eabi_attribute 20, 1   @ Tag_ABI_FP_denormal\n"
      << ".eabi_attribute 21, 1   @ Tag_ABI_FP_exceptions\n"
      << ".eabi_attribute 23, 3   @ Tag_ABI_FP_number_model: IEEE 754\n"
      << ".eabi_attribute 34, 1   @ Tag_CPU_unaligned_access\n"
      << ".eabi_attribute 24, 1   @ Tag_ABI_align_needed: 8-byte\n"
      << ".eabi_attribute 25, 1   @ Tag_ABI_align_preserved: 8-byte\n"
      << ".eabi_attribute 28, 1   @ Tag_ABI_VFP_args\n"
      << ".eabi_attribute 36, 1   @ Tag_FP_HP_extension\n"
      << ".eabi_attribute 38, 1   @ Tag_ABI_FP_16bit_format\n"
      << ".eabi_attribute 42, 1   @ Tag_MPextension_use\n"
      << ".eabi_attribute 68, 1   @ Tag_Virtualization_use\n";
  if (CPUFeatures.hasFeature(TargetARM32Features::HWDivArm)) {
    Str << ".eabi_attribute 44, 2   @ Tag_DIV_use\n";
  }
  // Technically R9 is used for TLS with Sandboxing, and we reserve it.
  // However, for compatibility with current NaCl LLVM, don't claim that.
  Str << ".eabi_attribute 14, 3   @ Tag_ABI_PCS_R9_use: Not used\n";
}

llvm::SmallBitVector TargetARM32::TypeToRegisterSet[IceType_NUM];
llvm::SmallBitVector TargetARM32::RegisterAliases[RegARM32::Reg_NUM];
llvm::SmallBitVector TargetARM32::ScratchRegs;

} // end of namespace Ice
