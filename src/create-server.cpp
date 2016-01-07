#include <llvm/Pass.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Constants.h> 
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
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
#include <algorithm>
#include <map>
#include <string>

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
    cl::value_desc("output file"));

cl::opt<std::uint32_t>
MaxWorkers(
    "w",
    cl::desc("maximum number of workers to spawn per function"),
    cl::value_desc("worker count"),
    cl::init(1));

cl::list<std::string>
FunctionsToFune(
    "f",
    cl::desc("functions to tune"),
    cl::value_desc("function"),
    cl::OneOrMore, cl::Prefix);

// replace a call instruction with an equivalent call to `_server_spawn_worker`
// return the replaced call
CallInst *emit_spawn_worker(CallInst *Call, Function *SpawnFn, Type *FuncTy)
{
  Function *F = Call->getCalledFunction();
  if (!F) return Call;

  std::string FnName = F->getName();
  if (std::find(FunctionsToFune.begin(), FunctionsToFune.end(), FnName) ==
      FunctionsToFune.end())
    return Call;

  Module *M = SpawnFn->getParent();
  LLVMContext &Ctx = M->getContext();

  // declare global variable that refers to this function's name
  Constant *Str = ConstantDataArray::getString(Ctx, FnName);
  GlobalVariable *GV = new GlobalVariable(*M, Str->getType(),
      true, GlobalValue::PrivateLinkage,
      Str, "tuning-server.fn", nullptr,
      GlobalVariable::NotThreadLocal,
      0);
  Constant *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  std::vector<Constant *> Idxs = {Zero, Zero};
  Constant *FnNamePtr = ConstantExpr::getInBoundsGetElementPtr(Str->getType(), GV, Idxs); 

  // declare global variable to hold counter for this call site
  static std::map<std::string, GlobalVariable *> Counters;
  GlobalVariable *Counter = Counters[FnName]; 
  if (!Counter) {
    Type *I32Ty = Type::getInt32Ty(Ctx);
    Constant *CounterInit = ConstantInt::get(I32Ty,
        MaxWorkers);
    Counters[FnName] = Counter = new GlobalVariable(*M, I32Ty,
        false, GlobalValue::PrivateLinkage,
        CounterInit, "tuning-server.worker-count", nullptr,
        GlobalVariable::NotThreadLocal,
        0); 
  }

  // replace `call func(args)` with
  // `call _server_spawn_worker(func, func-name, args, counter) 
  BitCastInst *Arg = new BitCastInst(
      Call->getArgOperand(0),
      Type::getInt8PtrTy(Ctx),
      "",
      Call);
  BitCastInst *FuncPtr= new BitCastInst(
      F,
      FuncTy->getPointerTo(),
      "",
      Call);
  std::vector<Value *> Args = {
    FuncPtr,
    FnNamePtr, 
    Arg,
    Counter
  };
  CallInst *CallToWorker = CallInst::Create(
      SpawnFn,
      Args,
      "",
      Call);
  Call->removeFromParent();

  return CallToWorker;
}

void create_server(Module &M)
{
  LLVMContext &Ctx = M.getContext();
  Type *I8PtrTy = Type::getInt8PtrTy(Ctx);
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *I32PtrTy = Type::getInt32PtrTy(Ctx);
  
  // declare
  // `void _server_spawn_worker(
  //      void *(*orig_func)(void *),
  //      char *func_name,
  //      void *args,
  //      uint32_t workers_to_spawn)`
  std::vector<Type *> GenericArgs = { I8PtrTy };
  FunctionType *GenericFnTy = FunctionType::get(VoidTy, GenericArgs, false);
  std::vector<Type *> SpawnArgs = {
    GenericFnTy->getPointerTo(),
    I8PtrTy,
    I8PtrTy,
    I32PtrTy
  };
  Function *SpawnFn = Function::Create(
      FunctionType::get(VoidTy, SpawnArgs, false),
      Function::ExternalLinkage,
      "_server_spawn_worker",
      &M);

  for (Function &F : M.functions()) {
    if (F.empty()) continue;

    for (BasicBlock &BB : F) {
      for (BasicBlock::iterator I = BB.begin(); I != BB.end(); ++I) {
        CallInst *Call = dyn_cast<CallInst>(&*I);
        if (Call) {
          I = emit_spawn_worker(Call, SpawnFn, GenericFnTy);
        }
      }
    }
  }
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

  create_server(*M.get());

  legacy::PassManager PM;
  PM.add(createBitcodeWriterPass(Out.os(), true));
  PM.run(*M.get());

  Out.keep();
}
