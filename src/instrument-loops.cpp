#include <llvm/Pass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/Bitcode/BitcodeWriterPass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/SystemUtils.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>

#include <vector>

using namespace llvm;

cl::opt<std::string>
InputFilename(
    cl::Positional,
    cl::desc("<input file>"),
    cl::Required);

cl::opt<std::string>
OutputFilename( 
    "o", 
    cl::desc("Specify output file name"), 
    cl::value_desc("<output file>"));

namespace llvm {
void initializeLoopInstrumentationPass(PassRegistry &);
}; 

// pass to insert `prof_begin` and `prof_end` to profile loops
// 
// implement this as a `ModulePass` so that 
// we can insert a global array of `struct loop_data`
// into the module as well as reference a loop with
// its loop header, which we reference by visiting order
struct LoopInstrumentation : public ModulePass {                       
  static char ID;

  Module *curM;

  LoopInstrumentation() : ModulePass(ID) {
    initializeLoopInstrumentationPass(*PassRegistry::getPassRegistry());
  };

  virtual bool runOnModule(Module &) override; 

  StructType *LoopProfileTy;

  // insert code to profile a loop
  // return `struct loop_data` declared for the loop
  Constant *instrumentLoop(Constant *Fn, Loop *L, unsigned Id);

  // declare and initialize data for profiler
  void declare(std::vector<Constant *> &);

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    //AU.addRequiredID(LoopSimplifyID); 
  }

  const char *getPassName() const override { return "LoopInstrumentation pass"; }
};

void LoopInstrumentation::declare(std::vector<Constant *> &LoopProfileArr)
{ 
  unsigned NumLoops = LoopProfileArr.size();
  ArrayType *ArrTy = ArrayType::get(LoopProfileTy->getPointerTo(), NumLoops);
  Constant *ArrContent = ConstantArray::get(ArrTy, LoopProfileArr);
  // declare `_prof_loops`
  new GlobalVariable(*curM,
      ArrTy,
      false, GlobalValue::ExternalLinkage,
      ArrContent, "_prof_loops", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  // also define `_prof_num_loop`
  Type *Int32Ty = Type::getInt32Ty(curM->getContext());
  Constant *NumLoop = ConstantInt::get(Int32Ty, NumLoops, true);
  new GlobalVariable(*curM,
      Int32Ty,
      true, GlobalValue::ExternalLinkage,
      NumLoop, "_prof_num_loops", nullptr,
      GlobalVariable::NotThreadLocal,
      0); 
}

char LoopInstrumentation::ID = 1;

INITIALIZE_PASS_BEGIN(LoopInstrumentation, "", "", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(LoopInstrumentation, "", "", true, true) 

// create an IRBuilder that inserts instructions in the first
// non-phi and non-pad instructions
IRBuilder<> createFrontInserter(BasicBlock *BB)
{
  BasicBlock::iterator I(BB->getFirstNonPHI());
  while (isa<LandingPadInst>(I)) ++I; 

  return IRBuilder<> (BB, I);
}

Constant *LoopInstrumentation::instrumentLoop(Constant *Fn, Loop *L, unsigned Id)
{ 
  BasicBlock* Preheader = L->getLoopPreheader();

  LLVMContext &Ctx = Preheader->getContext();
  Type *Int32Ty = Type::getInt32Ty(Ctx),
       *Int64Ty = Type::getInt64Ty(Ctx);

  // declare instance of `struct loop_data` 
  std::vector<Constant *> Fields {
    Fn,
    ConstantInt::get(Int32Ty, Id),
    ConstantInt::get(Int32Ty, 0),
    ConstantInt::get(Int64Ty, 0),
    ConstantInt::get(Int64Ty, 0)
  };
  Constant *LoopProfileInit = ConstantStruct::get(LoopProfileTy, Fields);
  GlobalVariable *Profile = new GlobalVariable(*curM,
      LoopProfileInit->getType(),
      false, GlobalValue::PrivateLinkage,
      LoopProfileInit, "prof.loop", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  std::vector<Value *> ProfArg = { Profile };

  IRBuilder<> Builder = createFrontInserter(Preheader);
  Constant *Zero = ConstantInt::get(Int32Ty, 0),
           *RunningIdx = ConstantInt::get(Int32Ty, 2),
           *RunsIdx = ConstantInt::get(Int32Ty, 3),
           *One = ConstantInt::get(Int32Ty, 1);
  std::vector<Value *> RunningIndexes { Zero, RunningIdx },
    RunsIndexes { Zero, RunsIdx };
  Value *RunningAddr = Builder.CreateInBoundsGEP(
      LoopProfileTy,
      Profile, 
      RunningIndexes); 
  // set `running` to 1
  Builder.CreateStore(One, RunningAddr);
  Value *RunsAddr = Builder.CreateInBoundsGEP(
      LoopProfileTy,
      Profile,
      RunsIndexes);
  Value *OldRuns = Builder.CreateLoad(RunsAddr);
  Value *NewRuns = Builder.CreateBinOp(Instruction::Add, OldRuns,
      ConstantInt::get(Int64Ty, 1));
  // increment `runs`
  Builder.CreateStore(NewRuns, RunsAddr);
  
  SmallVector<BasicBlock *, 4> Exits;
  L->getExitBlocks(Exits);
  // set `running` to 0 in exit blocks
  for (BasicBlock *BB : Exits) { 
    IRBuilder<> Builder = createFrontInserter(BB);
    Value *RunningAddr = Builder.CreateInBoundsGEP(
        LoopProfileTy,
        Profile, 
        RunningIndexes); 
    Builder.CreateStore(Zero, RunningAddr);
  }
  
  return Profile;
}

bool LoopInstrumentation::runOnModule(Module &M)
{ 
  curM = &M;
  LLVMContext &Ctx = M.getContext();

  // declare 
  // ```
  // struct loop_profile { 
  //     char *func;
  //     int32_t header_id;
  //     int32_t running;
  //     int64_t runs;
  //     int64_t sampled;
  // };
  // ``` 
  LoopProfileTy = StructType::create(Ctx, "LoopProfile");
  std::vector<Type *> Fields {
    Type::getInt8PtrTy(Ctx),
    Type::getInt32Ty(Ctx),
    Type::getInt32Ty(Ctx),
    Type::getInt64Ty(Ctx),
    Type::getInt64Ty(Ctx),
  };
  LoopProfileTy->setBody(Fields);

  std::vector<Constant *> LoopProfile;
  for (Function &F : M.getFunctionList()) {
    // external function
    if (F.empty()) continue;

    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F)
      .getLoopInfo();

    // declare global variable that refers to this function's name
    Constant *Str = ConstantDataArray::getString(Ctx, F.getName());
    GlobalVariable *GV = new GlobalVariable(M, Str->getType(),
        true, GlobalValue::PrivateLinkage,
        Str, "prof.fn", nullptr,
        GlobalVariable::NotThreadLocal,
        0);
    Constant *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
    std::vector<Constant *> Args = {Zero, Zero};
    Constant *FnName = ConstantExpr::getInBoundsGetElementPtr(Str->getType(), GV, Args); 

    unsigned i = 0;
    for (BasicBlock &BB : F) {
      ++i;
      Loop *L = LI.getLoopFor(&BB);
      if (!L || L->getParentLoop() ||
          !L->isLoopSimplifyForm() || L->getHeader() != &BB) continue;

      LoopProfile.push_back(instrumentLoop(FnName, L, i));
    }
  }

  declare(LoopProfile);
  return true;
} 

int main(int argc, char **argv)
{
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  cl::ParseCommandLineOptions(argc, argv, "instrument loop for profiling");

  LLVMContext &Context = getGlobalContext();
  SMDiagnostic Err; 
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);

  std::error_code EC;
  tool_output_file Out(OutputFilename, EC, sys::fs::F_None);
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  } 

  legacy::PassManager Passes;
  Passes.add(new LoopInstrumentation());
  Passes.add(createBitcodeWriterPass(Out.os(), true));

  Passes.run(*M.get());
  
  Out.keep();
}
