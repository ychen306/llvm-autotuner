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
#include <map>
#include <string>

using namespace llvm;

cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input file>"),
                                   cl::Required);

cl::opt<std::string> OutputFilename("o", cl::desc("Specify output file name"),
                                    cl::value_desc("output file"));

cl::opt<std::string> FunctionToRun("f", cl::desc("functions to run"),
                                   cl::value_desc("function"), cl::Required,
                                   cl::Prefix);

cl::list<int> Invos("inv", cl::desc("invocation you want to run"),
                    cl::value_desc("invocation"), cl::OneOrMore, cl::Prefix);

// replace a call instruction with an equivalent call to `_server_spawn_worker`
// return the replaced call
CallInst *replaceCallWithSpawn(CallInst *Call, Value *SpawnFn, Type *FuncTy) {
  Function *F = Call->getCalledFunction();
  if (!F)
    return Call;

  std::string FnName = F->getName();

  Module *M = F->getParent();
  LLVMContext &Ctx = M->getContext();

  // declare global variable that refers to this function's name
  Constant *Str = ConstantDataArray::getString(Ctx, FnName);
  GlobalVariable *GV = new GlobalVariable(
      *M, Str->getType(), true, GlobalValue::PrivateLinkage, Str,
      "server.fn-name", nullptr, GlobalVariable::NotThreadLocal, 0);
  Constant *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  std::vector<Constant *> Idxs = {Zero, Zero};
  Constant *FnNamePtr =
      ConstantExpr::getInBoundsGetElementPtr(Str->getType(), GV, Idxs);

  // replace `call func(args)` with
  // `call _server_spawn_worker(func, func_name, args)
  BitCastInst *Arg = new BitCastInst(Call->getArgOperand(0),
                                     Type::getInt8PtrTy(Ctx), "", Call);
  BitCastInst *FuncPtr = new BitCastInst(F, FuncTy->getPointerTo(), "", Call);
  std::vector<Value *> Args = {FuncPtr, FnNamePtr, Arg};
  CallInst *CallToWorker = CallInst::Create(SpawnFn, Args, "", Call);
  Call->replaceAllUsesWith(CallToWorker);

  return CallToWorker;
}

void create_server(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *I8PtrTy = Type::getInt8PtrTy(Ctx);

  Type *Int32Ty = Type::getInt32Ty(Ctx);

  // declare _server_num_invos
  new GlobalVariable(M, Int32Ty, true, GlobalValue::ExternalLinkage,
                     ConstantInt::get(Int32Ty, Invos.size()),
                     "_server_num_invos", nullptr,
                     GlobalVariable::NotThreadLocal, 0);

  // decalre _server_invos
  std::vector<Constant *> InvosContent;
  InvosContent.resize(Invos.size());
  for (int i = 0, e = Invos.size(); i != e; i++)
    InvosContent[i] = ConstantInt::get(Int32Ty, Invos[i]);
  ArrayType *InvosTy = ArrayType::get(Int32Ty, Invos.size());
  new GlobalVariable(M, InvosTy, true, GlobalValue::ExternalLinkage,
                     ConstantArray::get(InvosTy, InvosContent), "_server_invos",
                     nullptr, GlobalVariable::NotThreadLocal, 0);

  // declare
  // `uint32_t _server_spawn_worker(
  //      void *(*orig_func)(void *),
  //      char *func_name,
  //      void *args,
  //      uint32_t workers_to_spawn)`
  std::vector<Type *> GenericArgs = {I8PtrTy};
  FunctionType *GenericFnTy = FunctionType::get(Int32Ty, GenericArgs, false);
  std::vector<Type *> SpawnArgs = {GenericFnTy->getPointerTo(), I8PtrTy,
                                   I8PtrTy};
  Function *SpawnFn =
      Function::Create(FunctionType::get(Int32Ty, SpawnArgs, false),
                       Function::ExternalLinkage, "_server_spawn_worker", &M);

  for (Function &F : M.functions()) {
    if (F.empty())
      continue;

    for (BasicBlock &BB : F) {
      for (BasicBlock::iterator I = BB.begin(); I != BB.end(); ) {
        CallInst *Call = dyn_cast<CallInst>(&*I);
	++I;	// pre-increment to allow Call to be replaced below
        if (Call && Call->getCalledFunction() &&
            Call->getCalledFunction()->getName() == FunctionToRun) {
          Type *RetTy = Call->getFunctionType()->getReturnType();
          // cast _server_spawn_worker's return type to whatever `Call` returns
          auto *SpawnTy =
              FunctionType::get(RetTy, SpawnFn->getFunctionType()->params(),
                                false)
                  ->getPointerTo();
          replaceCallWithSpawn(
              Call, new BitCastInst(SpawnFn, SpawnTy, "", Call), GenericFnTy);
          Call->eraseFromParent();
        }
      }
    }
  }
}

int main(int argc, char **argv) {
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
