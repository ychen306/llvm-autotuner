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

using namespace llvm;

cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input file>"),
                                   cl::Required);

cl::opt<std::string> OutputFilename("o", cl::desc("Specify output file name"),
                                    cl::value_desc("output file"));

cl::opt<std::string>
    FunctionInvoked("f",
                    cl::desc("function whose invocations you want to profile"),
                    cl::value_desc("function invoked"), cl::Prefix);

void instrumentInvokedFunction(Function *F) {
  auto *M = F->getParent();
  auto &Ctx = M->getContext();
  auto *VoidTy = Type::getVoidTy(Ctx);
  // type for `void (*)()`
  auto *FnTy = FunctionType::get(VoidTy, std::vector<Type *>{}, false);

  // declare `void _invos_begin()`
  auto *BeginFn =
      Function::Create(FnTy, Function::ExternalLinkage, "_invos_begin", M);

  // declare `void _invos_end()`
  auto *EndFn =
      Function::Create(FnTy, Function::ExternalLinkage, "_invos_end", M);

  // call `_invos_begin()` in the beginning of the entry block
  auto &Entry = F->getEntryBlock();
  auto *First = &*Entry.getFirstInsertionPt();
  CallInst::Create(BeginFn, std::vector<Value *>{}, "", First);

  // call `_invos_end()` in every exit blocks
  for (auto &BB : *F) {
    auto *Last = BB.getTerminator();
    if (isa<ReturnInst>(Last)) {
      CallInst::Create(EndFn, std::vector<Value *>{}, "", Last);
    }
  }
}

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  cl::ParseCommandLineOptions(
      argc, argv, "instrument module to profile invocations of a function");

  LLVMContext &Context = getGlobalContext();
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);

  std::error_code EC;
  tool_output_file Out(OutputFilename, EC, sys::fs::F_None);
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }

  instrumentInvokedFunction(M->getFunction(FunctionInvoked));

  legacy::PassManager Passes;
  Passes.add(createBitcodeWriterPass(Out.os(), true));
  Passes.run(*M.get());

  Out.keep();
}
