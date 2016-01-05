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

  StructType *LoopDataTy;
  Function *BeginFn, *EndFn;

  // insert code to profile a loop
  // return `struct loop_data` declared for the loop
  Constant *instrumentLoop(Constant *Fn, Loop *L, unsigned Id);

  // declare `extern struct loop_data **loops` for profiler
  void declare(std::vector<Constant *> &);

  void init(Module &M);

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    //AU.addRequiredID(LoopSimplifyID); 
  }

  const char *getPassName() const override { return "LoopInstrumentation pass"; }
};

void LoopInstrumentation::declare(std::vector<Constant *> &LoopDataArr)
{ 
  unsigned NumLoops = LoopDataArr.size();
  ArrayType *ArrTy = ArrayType::get(LoopDataTy->getPointerTo(), NumLoops);
  Constant *ArrContent = ConstantArray::get(ArrTy, LoopDataArr);
  // declare `_prof_loops`
  new GlobalVariable(*curM,
      ArrTy,
      false, GlobalValue::ExternalLinkage,
      ArrContent, "_prof_loops", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  // also define `_prof_num_loop`
  Type *IntTy = Type::getInt32Ty(curM->getContext());
  Constant *NumLoop = ConstantInt::get(IntTy, NumLoops, true);
  new GlobalVariable(*curM,
      IntTy,
      true, GlobalValue::ExternalLinkage,
      NumLoop, "_prof_num_loops", nullptr,
      GlobalVariable::NotThreadLocal,
      0); 
}

// to insert static declarations to support profiling
void LoopInstrumentation::init(Module &M)
{
  LLVMContext &Ctx = M.getContext();

  // declare 
  // ```
  // struct loop_data {
  // 	double total_elapsed;
  // 	double cur_begin;
  // 	uint32_t running;
  // 	uint32_t id;
  // 	char *fn_name;
  // };
  // ``` 
  LoopDataTy = StructType::create(Ctx, "LoopData");
  std::vector<Type *> Fields = {
    Type::getDoubleTy(Ctx),
    Type::getDoubleTy(Ctx),
    Type::getInt32Ty(Ctx),
    Type::getInt32Ty(Ctx),
    Type::getInt8PtrTy(Ctx)
  };
  LoopDataTy->setBody(Fields);

  Type *VoidTy = Type::getVoidTy(Ctx);

  // declare `void _prof_begin(struct loop_data *loop)` 
  std::vector<Type *> BeginArgs = { LoopDataTy->getPointerTo() };
  FunctionType *BeginFnTy = FunctionType::get(VoidTy, BeginArgs, false); 
  BeginFn = Function::Create(BeginFnTy, Function::ExternalLinkage, "_prof_begin", &M);

  // declare `void _prof_end(struct loop_data *loop)`,
  // which has the same type as `_prof_begin`
  EndFn = Function::Create(BeginFnTy, Function::ExternalLinkage, "_prof_end", &M);

  // declare `void _prof_dump()`
  std::vector<Type *> NoArg;
  FunctionType *DumpFnTy = FunctionType::get(VoidTy, NoArg, false);
  Function::Create(DumpFnTy, Function::ExternalLinkage,
      "_prof_dump", &M);

}


char LoopInstrumentation::ID = 1;

INITIALIZE_PASS_BEGIN(LoopInstrumentation, "", "", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(LoopInstrumentation, "", "", true, true) 


Constant *LoopInstrumentation::instrumentLoop(Constant *Fn, Loop *L, unsigned Id)
{ 
  BasicBlock* Preheader = L->getLoopPreheader();

  LLVMContext &Ctx = Preheader->getContext();
  Type *DoubleTy = Type::getDoubleTy(Ctx);
  Type *IntTy = Type::getInt32Ty(Ctx);

  // declare instance of `struct loop_data` 
  std::vector<Constant *> Fields = {
    ConstantFP::get(DoubleTy, 0),
    ConstantFP::get(DoubleTy, 0),
    ConstantInt::get(IntTy, 0, true),
    ConstantInt::get(IntTy, Id, true),
    Fn
  };
  Constant *Struct = ConstantStruct::get(LoopDataTy, Fields);
  GlobalVariable *Data = new GlobalVariable(*Preheader->getParent()->getParent(),
      Struct->getType(),
      false, GlobalValue::ExternalLinkage,
      Struct, "prof.data", nullptr,
      GlobalVariable::NotThreadLocal,
      0);

  std::vector<Value *> ProfArg = { Data };

  // insert a call to `_prof_begin` in the end of preheader
  Instruction *Terminator = Preheader->getTerminator();
  CallInst::Create(
      BeginFn,
      ProfArg,
      Twine(""),
      Terminator); 
  
  SmallVector<BasicBlock *, 4> Exits;
  L->getExitBlocks(Exits);
  // insert a call to `_prof_end` in the beginning of every exit blocks
  // TODO conside laddingpad, phi, etc
  for (BasicBlock *BB : Exits) { 
    BasicBlock::iterator I(BB->getFirstNonPHI());
    while (isa<LandingPadInst>(I)) ++I;
    CallInst::Create(
        EndFn,
        ProfArg,
        Twine(""),
        &*I);
  }
  
  return Data;
}

bool LoopInstrumentation::runOnModule(Module &M)
{ 
  curM = &M;
  init(M);

  LLVMContext &Ctx = M.getContext();

  std::vector<Constant *> LoopData;
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

      LoopData.push_back(instrumentLoop(FnName, L, i));
    }
  }

  declare(LoopData);
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
