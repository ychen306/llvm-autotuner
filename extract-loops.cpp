#include <llvm/Pass.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Transforms/Utils/CodeExtractor.h>
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/ToolOutputFile.h"

#include <utility>
#include <set>
#include <string>
#include <cstdlib>

using namespace llvm;

// since basic blocks can be implicitly labelled,
// we will reference them (across program executations) by the
// order of defualt traversal. i.e. the first block encounter
// in `for (auto &BB : F)` has id 1;
struct LoopHeader {
    std::string Function;
    unsigned HeaderId;
};

// TODO terminate program gracefully...
void error(std::string Msg)
{
    errs() << "[Error]: " << Msg << '\n';
    std::abort();
}

struct LoopHeaderParser : public cl::parser<LoopHeader> { 
    LoopHeaderParser(cl::Option &O) : parser(O) {}

    bool parse(cl::Option &O, StringRef ArgName, const std::string &Arg, LoopHeader &LH) {
        size_t sep = Arg.find(',');

        // ill-formated string
        if (sep >= Arg.length() - 1) return true;

        LH.Function = Arg.substr(0, sep);
        int Id = LH.HeaderId = std::atoi(Arg.substr(sep+1).c_str());
        if (Id <= 0) {
            errs() << "Header id must be a positive integer\n";
            return true;
        }

        return false;
    }
};

cl::opt<std::string> InputFilename(
        cl::Positional,
        cl::desc("<input file>"),
        cl::Required);

cl::list<LoopHeader, bool, LoopHeaderParser> LoopsToExtract(
        "l",
        cl::desc("Specify loop(s) to extract. Describe a loop in this format: \"[function],[loop header]\""),
        cl::OneOrMore, cl::Prefix);

struct LoopExtractor : public ModulePass {
    static char ID;

    virtual bool runOnModule(Module &) override;
    virtual void getAnalysisUsage(AnalysisUsage &) const override;
    LoopExtractor() : ModulePass(ID) { initializeLoopExtractorPass(*PassRegistry::getPassRegistry()); }
    const char *getPassName() const override { return "LoopExtractor pass"; }
}; 

void LoopExtractor::getAnalysisUsage(AnalysisUsage &AU) const
{
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
}

char LoopExtractor::ID = 42; 
INITIALIZE_PASS_BEGIN(LoopExtractor, "", "", true, true);
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass);
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass);
INITIALIZE_PASS_END(LoopExtractor, "", "", true, true);

bool LoopExtractor::runOnModule(Module &M)
{ 
    bool Changed = false;
    // mapping function -> ids of basic blocks
    std::map<std::string, std::set<unsigned> > Loops;
    for (LoopHeader &LH : LoopsToExtract)
        Loops[LH.Function].insert(LH.HeaderId);

    for (auto I : Loops) {
        Function *F = M.getFunction(I.first); 
        if (F == nullptr)
            error("input module doesn't contain function " + I.first);

        std::set<unsigned> &Ls = I.second;

        LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(*F)
            .getLoopInfo();
        DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>(*F)
            .getDomTree(); 

        unsigned i = 0;

        // find basic blocks that are loop headers of loops that the user
        // wants to extract
        for (BasicBlock &BB : *F) {
            if (Ls.find(++i) == Ls.end()) continue;

            Loop *L = LI.getLoopFor(&BB);
            if (!L || L->getParentLoop() || &BB != L->getHeader())
                error("basic block " + std::to_string(i) + " is not a loop header of top level loop");

            // TODO
            // extract `L` as a separate function
        }
    }

    return Changed;
}

int main(int argc, char **argv)
{
    // Print a stack trace if we signal out.
    sys::PrintStackTraceOnErrorSignal();
    PrettyStackTraceProgram X(argc, argv);

    cl::ParseCommandLineOptions(argc, argv, "top-level loop extractor");

    LLVMContext &Context = getGlobalContext();
    SMDiagnostic Err; 
    std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);

    legacy::PassManager Passes;
    
    Passes.add(new LoopExtractor());

    Passes.run(*M.get());
}
