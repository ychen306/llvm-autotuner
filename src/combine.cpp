/*
 * the opposite of `extract-loops`
 */
#include <llvm/Pass.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
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
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/CallSite.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Transforms/IPO/InlinerPass.h>
#include <llvm/Transforms/IPO.h>

#include <vector>
#include <fstream>
#include <sstream>
#include <string>

using namespace llvm;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));

void markExtractedToInline(Module &M)
{
  std::ifstream ExtractedList("extracted.list");
  std::string Line;
  std::string FnName;
  InlineFunctionInfo IFI;

  std::getline(ExtractedList, Line);
  while (std::getline(ExtractedList, Line)) { 
    std::istringstream Tokens(Line);
    Tokens >> FnName; 
    Function *Fn = M.getFunction(FnName);

    if (Fn) Fn->addFnAttr(Attribute::AlwaysInline);
  }
}

int main(int argc, char **argv)
{
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  cl::ParseCommandLineOptions(argc, argv, "loop combiner...");
  
  LLVMContext &Context = getGlobalContext();
  SMDiagnostic Err; 
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);

  std::error_code EC;
  tool_output_file Out(OutputFilename, EC, sys::fs::F_None);
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }

  markExtractedToInline(*M);
  
  legacy::PassManager PM; 
  PM.add(createAlwaysInlinerPass());
  PM.add(createGlobalDCEPass());
  PM.add(createBitcodeWriterPass(Out.os(), true)); 
  PM.run(*M);
  Out.keep();
}
