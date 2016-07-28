#define DEBUG_TYPE "devirt"

#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/CallSite.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Bitcode/BitcodeWriterPass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/SystemUtils.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#include "LoopCallProfile.h"

using namespace llvm;

STATISTIC(NumDirectCalls, "Number of direct calls added");
STATISTIC(NumDevirtualized, "Number of indirect calls devirtualized");

cl::opt<unsigned> DevirtThreshold("devirt-threshold", cl::init(20),
                                cl::desc("make insert direct call only if a function is run above `devirt-threshold` the time of its parent"), 
                                cl::value_desc("devirtualization threshold"));

class Devirtualization : public ModulePass {
  std::vector<CallSite> findIndirectCalls(Module&);
  std::vector<Function *> getTargets(const CallSite &IndirectCall,
                                     LoopCallProfile &DynCG);
  bool devirtualize(CallSite &IndirectCall,
                    const std::vector<Function *>& Targets);
public:
  virtual bool runOnModule(Module &) override;
  Devirtualization() : ModulePass(ID) {}
  static char ID;
};

char Devirtualization::ID = 42;

static RegisterPass<Devirtualization>
X("devirt", "profile-guied devirtualization");

/// find all the indirect calls in `M`
std::vector<CallSite> Devirtualization::findIndirectCalls(Module &M)
{
  std::vector<CallSite> IndirectCalls;

  for (auto &F : M) 
    for (auto &BB : F)
      for (auto &I : BB)
        if (isa<CallInst>(&I) || isa<InvokeInst>(&I)) {
          CallSite CS(&I);
          if (!isa<Function>(CS.getCalledValue()->stripPointerCasts()))
            IndirectCalls.emplace_back(std::move(CS));
        }

  return IndirectCalls;
}

/// get a list of "hot" targets
std::vector<Function *>
Devirtualization::getTargets(const CallSite &IndirectCall, LoopCallProfile &DynCG)
{ 
  std::vector<Function *> Targets;
  auto *Caller = IndirectCall.getCaller();
  auto *M = Caller->getParent();
  unsigned CallerId = DynCG.getFuncIdForFuncName(Caller->getName().str());
  for (unsigned CalleeId : DynCG.getNested(CallerId)) {
    auto &CalleeName = DynCG.getLoopNameForId(CalleeId).getFuncName(); 
    auto *Callee = cast<Function>(M->getFunction(CalleeName));
    if (DynCG.getFreq(CallerId, CalleeId) > DevirtThreshold &&
        Callee->hasAddressTaken()) {
      Targets.push_back(Callee);
    }
  }
  return Targets;
}

bool Devirtualization::devirtualize(CallSite &IndirectCall,
                                    const std::vector<Function *>& Targets)
{
  if (Targets.size() == 0) return false;

  auto *Caller = IndirectCall.getCaller();
  auto &Ctx = Caller->getParent()->getContext();
  auto *Invoke = dyn_cast<InvokeInst>(IndirectCall.getInstruction());

  auto *BB = IndirectCall.getParent();

  auto OrigCallsite = IndirectCall.getInstruction()->getIterator();

  // block where we call make the original indirect call if we 
  // fail to make a direct call
  auto *Fallback = BB->splitBasicBlock(OrigCallsite);
  Fallback->setName("devirt.fallback");

  // block where we merge return values
  BasicBlock *Final;
  if (Invoke) {
    // the invoke instruction might "return" to a predecessor like this
    //  [invoke] -\
    //  ~~~ -----[predecessor]
    //  ~~~ ----/
    //
    //  in this case we want to split the edge between [invoke] and [predecessor]
    //  and merge return values in the new block
    Final = SplitCriticalEdge(Fallback, Invoke->getNormalDest());
    if (!Final) Final = Invoke->getNormalDest();
  } else {
    Final = Fallback->splitBasicBlock(++OrigCallsite);
  }
  Final->setName("devirt.final");

  auto *Int64Ty = Type::getInt64Ty(Ctx);
  auto *CalleeTy = IndirectCall.getCalledValue()->getType();
  auto *CalleeAddr = new PtrToIntInst(IndirectCall.getCalledValue(), Int64Ty,
                                      "calleeAddr", BB->getTerminator());
  Type *ReturnTy = IndirectCall.getFunctionType()->getReturnType();

  unsigned NumArgs = IndirectCall.getNumArgOperands();

  // argument we make direct call(s) with
  std::vector<Value *> Args(NumArgs);
  for (unsigned i = 0; i < NumArgs; i++) {
    Args[i] = IndirectCall.getArgOperand(i);
  }

  // vector of return values and which basic blocks they come from
  std::vector<std::pair<Value *, BasicBlock *>> ReturnVals;

  // create a chain of if-else-then-if... to find the right function
  // to make direct call with
  BB->getTerminator()->eraseFromParent();
  auto *Cur = BB;
  for (Function *Target : Targets) {
    // create a block where we make the direct call and jump to `Final`
    auto *TargetBlock = BasicBlock::Create(Ctx, Target->getName()+".directCall",
                                           Caller, Fallback);
    Value* TargetPtr = Target;
    if (Target->getFunctionType() != CalleeTy)
      TargetPtr = new BitCastInst(Target, CalleeTy, "", TargetBlock);

    Value *ReturnVal;
    if (!Invoke) {
      ReturnVal = CallInst::Create(TargetPtr, Args, "", TargetBlock);
      BranchInst::Create(Final, TargetBlock);
    } else {
      ReturnVal = InvokeInst::Create(TargetPtr, Invoke->getNormalDest(),
                                      Invoke->getUnwindDest(),
                                      Args, "", TargetBlock);
    }

    if (!ReturnTy->isVoidTy())
      ReturnVals.push_back({ ReturnVal, TargetBlock });

    // jump to `TargetBlock` if callee's address matches
    auto *TargetAddr = ConstantExpr::getPtrToInt(Target, Int64Ty);
    auto *Cmp = new ICmpInst(*Cur, CmpInst::ICMP_EQ, TargetAddr, CalleeAddr);
    auto *Fail = BasicBlock::Create(Ctx, "devirt.lookupTarget", Caller, Fallback);
    BranchInst::Create(TargetBlock, Fail, Cmp, Cur);

    Cur = Fail;
    ++NumDirectCalls;
  }

  // when everything fails, jump to `Fallback`
  BranchInst::Create(Fallback, Cur);

  // merge return values
  if (IndirectCall->getNumUses() > 0) { 
    auto *Phi = PHINode::Create(ReturnTy, ReturnVals.size(), "devirt.phi", &Final->front());
    IndirectCall->replaceAllUsesWith(Phi);
    Phi->addIncoming(IndirectCall.getInstruction(), Fallback);

    for (auto &Pair : ReturnVals) {
      Phi->addIncoming(Pair.first, Pair.second);
    }
  } 

  ++NumDevirtualized;

  return true;
}

bool Devirtualization::runOnModule(Module &M)
{ 
  auto IndirectCalls = findIndirectCalls(M);
  LoopCallProfile DynCG;
  DynCG.readProfiles();

  bool Changed = false;

  for (CallSite& CS : IndirectCalls) {
    std::vector<Function *> Targets = getTargets(CS, DynCG);
    Changed |= devirtualize(CS, Targets);
  }

  return Changed;
}
