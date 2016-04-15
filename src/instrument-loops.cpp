#include <llvm/Pass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
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
#include <set>
#include <utility>
#include <initializer_list>

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
  ArrayType *RunningArrTy;

  LoopInstrumentation() : ModulePass(ID) {
    initializeLoopInstrumentationPass(*PassRegistry::getPassRegistry());
  };

  virtual bool runOnModule(Module &) override; 

  StructType *LoopProfileTy;

  // return a constant `struct loop_profile` initializer for a loop or (a function)
  Constant *getLoopProfileInitializer(Constant *Fn, unsigned Id);

  // declare and initialize data for profiler
  Function* initGlobals(std::vector<Constant *> &);

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
  }


  // insert code in the beginning of an entry block and return address it's matching address
  // in `_prof_loops_running`
  Value *instrumentEntry(BasicBlock *Entry, unsigned Idx, bool Exclusive=true);
  void instrumentExit(BasicBlock *Exit, Value *RunningAddr, bool Exclusive=true);
  // instrument a loop to record loop entrance/exit
  void instrumentLoop(unsigned Idx, Loop *L);

  const char *getPassName() const override { return "LoopInstrumentation pass"; }
};

void LoopInstrumentation::instrumentExit(BasicBlock *Exit, Value *RunningAddr, bool Exclusive)
{
  LLVMContext &Ctx = CurModule->getContext();
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  GlobalVariable *LoopEntryAddr = CurModule->getGlobalVariable("_prof_entry",
						/*AllowInternal*/true);

  // insert in the beginning of the exit block
  auto Builder = Exclusive ? createFrontBuilder(Exit) : IRBuilder<>(Exit, Exit->getTerminator());
  Value *LoopEntry = Builder.CreateLoad(LoopEntryAddr);

  // emit `_prof_loops_running[idx] -= _prof_entry`
  Builder.CreateStore(
    Builder.CreateBinOp(Instruction::Sub, Builder.CreateLoad(RunningAddr), LoopEntry),
    RunningAddr);

  // emit `_prof_entry -= 1`
  Value *DecLoopEntry = Builder.CreateBinOp(Instruction::Sub, LoopEntry,
                                            ConstantInt::get(Int32Ty, 1));
  Builder.CreateStore(DecLoopEntry, LoopEntryAddr);
}

Value *LoopInstrumentation::instrumentEntry(BasicBlock *Entry, unsigned Idx, bool Exclusive)
{
  LLVMContext &Ctx = CurModule->getContext();
  Type *Int32Ty = Type::getInt32Ty(Ctx),
       *Int64Ty = Type::getInt64Ty(Ctx);

  GlobalVariable *Profiles = CurModule->getGlobalVariable("_prof_loops",
						/*AllowInternal*/true);
  GlobalVariable *RunningArr=CurModule->getGlobalVariable("_prof_loops_running",
						/*AllowInternal*/true);
  GlobalVariable *LoopEntryAddr=CurModule->getGlobalVariable("_prof_entry",
						/*AllowInternal*/true);

  auto Builder = Exclusive ?
    IRBuilder<>(Entry, Entry->getTerminator()) :
    createFrontBuilder(Entry);
  Constant *Zero = ConstantInt::get(Int32Ty, 0),
           *RunsIdx = ConstantInt::get(Int32Ty, 2);

  // get Profile from global array
  std::vector<Value *> ProfileIndexes { Zero, ConstantInt::get(Int32Ty, Idx) }; 
  Value *Profile = Builder.Insert(GetElementPtrInst::CreateInBounds(
      ProfileArrTy,
      Profiles,
      ProfileIndexes)); 

  // emit code for `++_prof_entry`
  Value *LoopEntry = Builder.CreateBinOp(Instruction::Add,
    Builder.CreateLoad(LoopEntryAddr), ConstantInt::get(Int32Ty, 1));
  Builder.CreateStore(LoopEntry, LoopEntryAddr); 

  // emit `_prof_loops_running[idx] += ++_prof_entry`
  std::vector<Value *> RunningIndexes { Zero, ConstantInt::get(Int32Ty, Idx) }; 
  Value *RunningAddr = Builder.Insert(GetElementPtrInst::CreateInBounds(
      RunningArrTy,
      RunningArr, 
      RunningIndexes)); 
  Builder.CreateStore(
      Builder.CreateBinOp(Instruction::Add, LoopEntry, Builder.CreateLoad(RunningAddr)),
      RunningAddr);
  
  // emit `_prof_loops[idx].runs++`
  std::vector<Value *> RunsIndexes { Zero, RunsIdx };
  Value *RunsAddr = Builder.CreateInBoundsGEP(
      LoopProfileTy,
      Profile,
      RunsIndexes);
  Value *OldRuns = Builder.CreateLoad(RunsAddr);
  Value *NewRuns = Builder.CreateBinOp(Instruction::Add, OldRuns,
      ConstantInt::get(Int64Ty, 1));
  Builder.CreateStore(NewRuns, RunsAddr);

  return RunningAddr;
}

void LoopInstrumentation::instrumentLoop(unsigned Idx, Loop *L)
{
  BasicBlock *Preheader = L->getLoopPreheader(); 
  Value *RunningAddr = instrumentEntry(Preheader, Idx);

  SmallVector<BasicBlock *, 4> Exits;
  L->getExitBlocks(Exits);
  std::set<BasicBlock *> UniqExits(Exits.begin(), Exits.end());

  for (BasicBlock *BB : UniqExits) { 
    instrumentExit(BB, RunningAddr);
  }
}

Function* LoopInstrumentation::initGlobals(std::vector<Constant *> &LoopProfiles)
{ 
  LLVMContext &Ctx = CurModule->getContext();
  unsigned NumLoops = LoopProfiles.size();

  Type *Int32Ty = Type::getInt32Ty(Ctx);

  // declare `_prof_loops`
  ProfileArrTy = ArrayType::get(LoopProfileTy, NumLoops);
  Constant *ArrContent = ConstantArray::get(ProfileArrTy, LoopProfiles);
  GlobalVariable* Prof_Loops_GVar =
    new GlobalVariable(*CurModule,
                     ProfileArrTy,
                     false, GlobalValue::PrivateLinkage,
                     ArrContent, "_prof_loops", nullptr,
                     GlobalVariable::NotThreadLocal,
                     0);

  RunningArrTy = ArrayType::get(Int32Ty, NumLoops);
  // declare `_prof_loops_running` 
  GlobalVariable* Prof_Loops_Running_GVar =
    new GlobalVariable(*CurModule,
      RunningArrTy,
      false, GlobalValue::PrivateLinkage,
      ConstantAggregateZero::get(RunningArrTy), "_prof_loops_running", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  // also define `_prof_num_loop`
  Constant *NumLoop = ConstantInt::get(Int32Ty, NumLoops, true);
  GlobalVariable *Prof_Num_Loops_GVar =
    new GlobalVariable(*CurModule,
      Int32Ty,
      true, GlobalValue::PrivateLinkage,
      NumLoop, "_prof_num_loops", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  // Declare external function used to register global variables used
  // per module
  const std::string extFuncName = "add_module_desc";
  assert(CurModule->getNamedValue(extFuncName) == NULL &&
	 "External function `add_module_desc' is in module already?");
  Constant *descFuncDecl = CurModule->getOrInsertFunction(extFuncName,
				Type::getVoidTy(Ctx), /* return type */
				Type::getInt32PtrTy(Ctx),
				PointerType::get(LoopProfileTy, 0),
				Type::getInt32PtrTy(Ctx),
				nullptr);

  // Create a function to call the declaration function
  FunctionType* newFType = FunctionType::get(Type::getVoidTy(Ctx),
					     /*isVarArg*/ false);
  Function* newF = Function::Create(newFType, GlobalValue::InternalLinkage,
				    "callAddModuleDesc", CurModule);
  BasicBlock* entryBlock = BasicBlock::Create(Ctx, "", newF);
  IRBuilder<> IRB(entryBlock);
  ArrayRef<Value*> arrayRefList = { /* std::initializer_list for ArrayRef */
    Prof_Num_Loops_GVar,
    IRB.CreateBitCast(Prof_Loops_GVar, PointerType::get(LoopProfileTy, 0)),
    IRB.CreateBitCast(Prof_Loops_Running_GVar, Type::getInt32PtrTy(Ctx)) };
  IRB.CreateCall(descFuncDecl, arrayRefList);
  IRB.CreateRetVoid();

  // And append the new function to the global ctors list so it gets called
  llvm::appendToGlobalCtors(*CurModule, newF, 65535);

  // Return the newly created function so it isn't instrumented!
  return newF;
}

char LoopInstrumentation::ID = 1;

INITIALIZE_PASS_BEGIN(LoopInstrumentation, "", "", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(LoopInstrumentation, "", "", true, true) 

Constant *LoopInstrumentation::getLoopProfileInitializer(Constant *Fn, unsigned Id)
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
  //     int64_t runs;
  // };
  // ``` 
  LoopProfileTy = StructType::create(Ctx, "LoopProfile");
  std::vector<Type *> Fields {
    Type::getInt8PtrTy(Ctx),
    Type::getInt32Ty(Ctx),
    Type::getInt64Ty(Ctx)
  };
  LoopProfileTy->setBody(Fields);

  Type *Int32Ty = Type::getInt32Ty(Ctx);
  // Declare `_prof_entry`: it is private to each module
  new GlobalVariable(*CurModule, 
      Int32Ty,
      false, GlobalValue::LinkOnceODRLinkage,
      ConstantInt::get(Int32Ty, 0), "_prof_entry", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  std::vector<Constant *> LoopProfiles;
  // index to profile entry
  unsigned Idx = 0;

  // find out how many functions/top-level loops are there and create
  // global variable to hold their profiling data
  for (Function &F : M.getFunctionList()) {
    // external function
    if (F.empty()) continue;

    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();

    // declare global str that refers to this function's name
    Constant *Str = ConstantDataArray::getString(Ctx, F.getName());
    GlobalVariable *GV = new GlobalVariable(M, Str->getType(),
        true, GlobalValue::PrivateLinkage,
        Str, "prof.fn", nullptr,
        GlobalVariable::NotThreadLocal,
        0);
    Constant *Zero = ConstantInt::get(Int32Ty, 0);
    std::vector<Constant *> Args = {Zero, Zero};
    Constant *FnName = ConstantExpr::getInBoundsGetElementPtr(Str->getType(), GV, Args); 

    // a loop's header id has to start from 1, so use 0 for function
    LoopProfiles.push_back(getLoopProfileInitializer(FnName, 0));

    unsigned i = 0;
    for (BasicBlock &BB : F) {
      ++i;
      Loop *L = LI.getLoopFor(&BB);

      if (!L || L->getParentLoop() ||
          !L->isLoopSimplifyForm() || L->getHeader() != &BB) continue;

      LoopProfiles.push_back(getLoopProfileInitializer(FnName, i));
    }
  }

  // Create the global variables and the function to register them.
  // Get the function pointer so we don't instrument it!
  Function* regFunc = initGlobals(LoopProfiles); 

  // insert code in the entry and exit blocks of a function/loop
  for (Function &F : M.getFunctionList()) {
    // skip external functions
    if (F.empty()) continue;

    // skip the registration function
    if (&F == regFunc) continue;

    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();

    Value *FnRunningAddr = instrumentEntry(&F.getEntryBlock(), Idx++, false);

    // instrument returning blocks of `F`
    for (BasicBlock &BB : F) {
      if (BB.getTerminator() != NULL && isa<ReturnInst>(BB.getTerminator())) {
        instrumentExit(&BB, FnRunningAddr, false);
      }
    }

    unsigned i = 0;
    for (BasicBlock &BB : F) {
      ++i;
      Loop *L = LI.getLoopFor(&BB);

      if (!L || L->getParentLoop() ||
          !L->isLoopSimplifyForm() || L->getHeader() != &BB) continue;

      instrumentLoop(Idx++, L);
    }
  }

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
