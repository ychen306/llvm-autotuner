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
#include <utility>

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


// create an IRBuilder that inserts instructions in the first
// non-phi and non-pad instructions
IRBuilder<> createFrontBuilder(BasicBlock *BB)
{
  BasicBlock::iterator I(BB->getFirstNonPHI());
  while (isa<LandingPadInst>(I)) ++I; 

  return IRBuilder<> (BB, I);
}

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

  Module *CurModule;

  ArrayType *ProfileArrTy;
  ArrayType *Int64ArrTy;

  LoopInstrumentation() : ModulePass(ID) {
    initializeLoopInstrumentationPass(*PassRegistry::getPassRegistry());
  };

  virtual bool runOnModule(Module &) override; 

  StructType *LoopProfileTy;

  // insert code to profile a loop
  // return `struct loop_data` declared for the loop
  Constant *getLoopProfileInitializer(Constant *Fn, Loop *L, unsigned Id);

  // declare and initialize data for profiler
  void declareArrs(std::vector<Constant *> &);

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    //AU.addRequiredID(LoopSimplifyID); 
  }

  // instrument a loop to record loop entrance/exit
  void instrumentLoop(unsigned Idx, Loop *L);

  const char *getPassName() const override { return "LoopInstrumentation pass"; }
};

void LoopInstrumentation::instrumentLoop(unsigned Idx, Loop *L)
{
  LLVMContext &Ctx = CurModule->getContext();
  Type *Int32Ty = Type::getInt32Ty(Ctx),
       *Int64Ty = Type::getInt64Ty(Ctx);
  BasicBlock *Preheader = L->getLoopPreheader();

  GlobalVariable *Profiles = CurModule->getGlobalVariable("_prof_loops");
  GlobalVariable *RunningArr = CurModule->getGlobalVariable("_prof_loops_running");

  IRBuilder<> Builder = createFrontBuilder(Preheader);
  Constant *Zero = ConstantInt::get(Int64Ty, 0),
           *One = ConstantInt::get(Int64Ty, 1),
           *RunsIdx = ConstantInt::get(Int32Ty, 2);

  // get Profile from global array
  std::vector<Value *> ProfileIndexes { Zero, ConstantInt::get(Int32Ty, Idx) }; 
  Value *Profile = Builder.CreateInBoundsGEP(
      ProfileArrTy,
      Profiles,
      ProfileIndexes); 

  // set `running` to 1
  std::vector<Value *> RunningIndexes { Zero, ConstantInt::get(Int32Ty, Idx) }; 
  Value *RunningAddr = Builder.CreateInBoundsGEP(
      Int64ArrTy,
      RunningArr, 
      RunningIndexes); 
  Builder.CreateStore(One, RunningAddr);

  // increment `runs`
  std::vector<Value *> RunsIndexes { Zero, RunsIdx };
  Value *RunsAddr = Builder.CreateInBoundsGEP(
      LoopProfileTy,
      Profile,
      RunsIndexes);
  Value *OldRuns = Builder.CreateLoad(RunsAddr);
  Value *NewRuns = Builder.CreateBinOp(Instruction::Add, OldRuns,
      ConstantInt::get(Int64Ty, 1));
  Builder.CreateStore(NewRuns, RunsAddr);
  
  SmallVector<BasicBlock *, 4> Exits;
  L->getExitBlocks(Exits);
  // set `running` to 0 in exit blocks
  for (BasicBlock *BB : Exits) { 
    IRBuilder<> Builder = createFrontBuilder(BB);
    Value *RunningAddr = Builder.CreateInBoundsGEP(
        Int64ArrTy,
        RunningArr, 
        RunningIndexes); 
    Builder.CreateStore(Zero, RunningAddr);
  }
}

void LoopInstrumentation::declareArrs(std::vector<Constant *> &LoopProfiles)
{ 
  LLVMContext &Ctx = CurModule->getContext();
  unsigned NumLoops = LoopProfiles.size();

  ProfileArrTy = ArrayType::get(LoopProfileTy, NumLoops);
  Constant *ArrContent = ConstantArray::get(ProfileArrTy, LoopProfiles);
  // declare `_prof_loops`
  new GlobalVariable(*CurModule,
      ProfileArrTy,
      false, GlobalValue::ExternalLinkage,
      ArrContent, "_prof_loops", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  // declare `_prof_loops_sampled`
  Int64ArrTy = ArrayType::get(
      Type::getInt64Ty(Ctx), NumLoops);
  new GlobalVariable(*CurModule,
      Int64ArrTy,
      false, GlobalValue::ExternalLinkage,
      ConstantAggregateZero::get(Int64ArrTy), "_prof_loops_sampled", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  // declare `_prof_loops_running` 
  new GlobalVariable(*CurModule,
      Int64ArrTy,
      false, GlobalValue::ExternalLinkage,
      ConstantAggregateZero::get(Int64ArrTy), "_prof_loops_running", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  // also define `_prof_num_loop`
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  Constant *NumLoop = ConstantInt::get(Int32Ty, NumLoops, true);
  new GlobalVariable(*CurModule,
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

Constant *LoopInstrumentation::getLoopProfileInitializer(Constant *Fn, Loop *L, unsigned Id)
{ 
  LLVMContext &Ctx = CurModule->getContext();
  Type *Int32Ty = Type::getInt32Ty(Ctx),
       *Int64Ty = Type::getInt64Ty(Ctx);

  // declare instance of `struct loop_data` 
  std::vector<Constant *> Fields {
    Fn,
    ConstantInt::get(Int32Ty, Id),
    ConstantInt::get(Int64Ty, 0)
  };

  return ConstantStruct::get(LoopProfileTy, Fields);
}

bool LoopInstrumentation::runOnModule(Module &M)
{ 
  CurModule = &M;
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
    Type::getInt64Ty(Ctx)
  };
  LoopProfileTy->setBody(Fields);

  std::vector<Constant *> LoopProfiles;
  std::vector<Loop *> LoopsToInstrument;
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

      LoopProfiles.push_back(getLoopProfileInitializer(FnName, L, i));
      LoopsToInstrument.push_back(L);
    }
  }

  declareArrs(LoopProfiles); 

  unsigned Idx = 0;
  for (Function &F : M.getFunctionList()) {
    // external function
    if (F.empty()) continue;

    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F)
      .getLoopInfo();

    unsigned i = 0;
    for (BasicBlock &BB : F) {
      ++i;
      Loop *L = LI.getLoopFor(&BB);
      if (!L || L->getParentLoop() ||
          !L->isLoopSimplifyForm() || L->getHeader() != &BB) continue; 

      instrumentLoop(Idx++, L);
    }
  }

  /*
  for (unsigned i = 0, e = LoopsToInstrument.size(); i != e; i++)
    instrumentLoop(i, LoopsToInstrument[i]);
    */
  
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
