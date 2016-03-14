#include <llvm/Pass.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Transforms/Utils/CodeExtractor.h>
#include <llvm/Transforms/Utils/Cloning.h>
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

#include <fstream>
#include <sstream>
#include <utility>
#include <set>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <cmath>

using namespace llvm;

// because basic blocks can be implicitly labelled,
// we will reference them (across program executations) by the
// order of defualt traversal. i.e. the first block encounter
// in `for (auto &BB : F)` has id 1;
struct LoopHeader {
  std::string Function;
  unsigned HeaderId;

  bool operator==(LoopHeader &other) {
    return Function == other.Function && HeaderId == other.HeaderId;
  }
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

static cl::opt<std::string>
ExtractedListFile(
  "e",
  cl::desc("file where name of extracted functions will be listed"),
  cl::init("extracted.list"));

static cl::opt<std::string>
InputFilename(
  cl::Positional,
  cl::desc("<input file>"),
  cl::Required);

static cl::opt<std::string>
OuputPrefix(
  "p",
  cl::desc("Specify output prefix"),
  cl::value_desc("output prefix"),
  cl::Required);

static cl::list<LoopHeader, bool, LoopHeaderParser>
LoopsToExtract(
  "l",
  cl::desc("Specify loop(s) to extract.\nDescribe a loop in this format:\n\"[function],[loop header]\""),
  cl::OneOrMore, cl::Prefix);


struct LoopExtractor : public ModulePass {
  static char ID;

  virtual bool runOnModule(Module &) override;

  virtual void getAnalysisUsage(AnalysisUsage &) const override;

  LoopExtractor() : ModulePass(ID) {
    initializeLoopExtractorPass(*PassRegistry::getPassRegistry());
  }

  const char *getPassName() const override { return "LoopExtractor pass"; }
};

void LoopExtractor::getAnalysisUsage(AnalysisUsage &AU) const
{
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
}

char LoopExtractor::ID = 42;


// define `initializeLoopExtractorPass()`
INITIALIZE_PASS_BEGIN(LoopExtractor, "", "", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(LoopExtractor, "", "", true, true)


static std::vector<GlobalValue *> ExtractedLoops;
// mapping <extracted loop> -> <names of functions that needs to be copied and extracted together with the map>
static std::map<std::string, std::vector<std::string>> Called;


// read meta data of the call graph node
std::vector<LoopHeader> readGraphNodeMeta()
{
  std::ifstream Fin("loop-prof.flat.csv");
  std::string Line;
  std::vector<LoopHeader> Nodes;

  // skip header
  std::getline(Fin, Line);
  while (std::getline(Fin, Line)) {
    LoopHeader Node;
    std::istringstream Fields(Line);
    std::getline(Fields, Node.Function, ',');
    Fields >> Node.HeaderId;

    Nodes.emplace_back(Node);
  }

  return Nodes;
}

// build an adjacency matrix for the dynamic call graph
// NOTE: this doesn't do any error handling
std::vector<std::vector<float>> loadDynCallGraph(unsigned NumNodes)
{
  std::ifstream Fin("loop-prof.graph.csv");
  std::vector<std::vector<float>> G(NumNodes);
  std::string NumStr;
  for (unsigned i = 0; i < NumNodes; i++) {
    G[i].resize(NumNodes);
    for (unsigned j = 0; j < NumNodes; j++) {
      Fin >> NumStr;
	  G[i][j] = std::stof(NumStr);
    }
  }
  return G;
}

bool LoopExtractor::runOnModule(Module &M)
{
  bool Changed = false;

  std::vector<LoopHeader> CGNodes = readGraphNodeMeta();
  std::vector<std::vector<float>> DynCG = loadDynCallGraph(CGNodes.size());

  // mapping function -> ids of basic blocks
  std::map<std::string, std::set<unsigned> > Loops;
  for (LoopHeader &LH : LoopsToExtract)
    Loops[LH.Function].insert(LH.HeaderId);

  std::vector<std::pair<Loop *, LoopHeader>> ToExtract;

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

      // "remember" this loop and extract it later
      ToExtract.emplace_back(std::pair<Loop *, LoopHeader>(L, {F->getName(), i}));
      Changed = true;
    }

    // actually extract loops
    for (auto &Pair: ToExtract) {
      Loop *L = Pair.first;
      LoopHeader &Header = Pair.second;
      CodeExtractor CE(DT, *L, true);
      Function *Extracted = CE.extractCodeRegion();
      if (!Extracted) continue;

      Extracted->setVisibility(GlobalValue::DefaultVisibility);
      Extracted->setLinkage(GlobalValue::ExternalLinkage);
      ExtractedLoops.push_back(Extracted);

      unsigned CallerIdx;
      for (unsigned i = 0, e = CGNodes.size(); i != e; i++) {
        if (CGNodes[i] == Header) {
          CallerIdx = i;
          break;
        }
      }
      // find out what functions are called by the loops
      for (unsigned CalleeIdx = 0, E = DynCG.size(); CalleeIdx < E; CalleeIdx++) {
        if (CalleeIdx == CallerIdx) continue;
        float TimeSpent = DynCG[CallerIdx][CalleeIdx];
        if (!std::isnan(TimeSpent) && TimeSpent > 0) {
          auto &Node = CGNodes[CalleeIdx];
          if (Node.HeaderId == 0)
            Called[Extracted->getName()].push_back(Node.Function);
        }
      }
    }
    ToExtract.resize(0);

  }

  return Changed;
}

std::string newFileName()
{
  static int ModuleId = 0;
  return OuputPrefix + "." + std::to_string(ModuleId++) + ".bc";
}

// return functions called (including those called indirectly) by `Caller`)
std::vector<GlobalValue *> getCalledFuncs(Module *M, Function *Caller)
{
  std::vector<GlobalValue *> Funcs;
  for (auto &CalleeName : Called[Caller->getName()]) {
    Funcs.push_back(M->getFunction(CalleeName));
  }

  return Funcs;
}

struct GraphNodeMeta {
  std::string Func;
  bool IsFunc; // could be a loop
};

// change internal non-constant globals to external and rename to avoid name-conflict
void externalize(Module &M)
{ 
  char FilePath[PATH_MAX];
  realpath(InputFilename.c_str(), FilePath);
  for (GlobalVariable &G : M.globals()) {
    if (G.isConstant()) continue;

    if (G.hasInternalLinkage() || G.hasPrivateLinkage()) {
      G.setName(std::string("llvm.autotuner.internals.")+FilePath+"."+G.getName());
      G.setVisibility(GlobalValue::DefaultVisibility);
      G.setLinkage(GlobalValue::ExternalLinkage);
    }
  }
}

std::vector<GlobalValue *> getGlobals(Module *M)
{
  std::vector<GlobalValue *> Globals;
  for (GlobalVariable &G : M->globals()) {
    Globals.push_back(&G);
  }
  return Globals;
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

  if (!M.get()) {
    Err.print(argv[0], errs());
    return 1;
  }

  std::error_code EC;

  externalize(*M.get());

  // create a module to hold all the globals (only)
  Module *GlobalsModule = CloneModule(M.get());
  legacy::PassManager GlobalExtraction;
  std::string GlobalsFName = newFileName();
  tool_output_file GlobalsOut(GlobalsFName, EC, sys::fs::F_None);
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }
  auto Globals = getGlobals(GlobalsModule);
  GlobalExtraction.add(createGVExtractionPass(Globals, false)); 
  GlobalExtraction.add(createBitcodeWriterPass(GlobalsOut.os(), true));
  GlobalExtraction.run(*GlobalsModule);
  GlobalsOut.keep();
  delete GlobalsModule;

  std::ofstream ExtractedList(ExtractedListFile);
  ExtractedList << GlobalsFName << '\n';

  legacy::PassManager Extraction;

  // extract loops and remove globals
  Globals = getGlobals(M.get());
  Extraction.add(createGVExtractionPass(Globals, true));
  Extraction.add(new LoopExtractor());
  Extraction.run(*M.get());

  Module *CopiedModule = CloneModule(M.get());

  legacy::PassManager PM;
  std::string MainModuleName = newFileName();
  tool_output_file Out(MainModuleName, EC, sys::fs::F_None);
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }
  // removed extracted loops from the main module
  PM.add(createGVExtractionPass(ExtractedLoops, true));
  PM.add(createBitcodeWriterPass(Out.os(), true));
  PM.run(*M.get());

  ExtractedList << MainModuleName << '\n';

  // now remove everything in a new module except
  // the extracted loop and its callees (which will also be internalized)
  for (GlobalValue *Extracted : ExtractedLoops) {
    Module *NewModule = CloneModule(CopiedModule);
    std::string ExtractedName = Extracted->getName();
    Function *ExtractedF = NewModule->getFunction(ExtractedName);
    std::vector<GlobalValue *> ToPreserve = getCalledFuncs(NewModule, ExtractedF);
    ToPreserve.push_back(ExtractedF);

    std::string BitcodeFName = newFileName();

    // report name of extracted function
    ExtractedList << ExtractedName << '\t' << BitcodeFName << '\n';

    tool_output_file ExtractedOut(BitcodeFName, EC, sys::fs::F_None);
    if (EC) {
      errs() << EC.message() << '\n';
      return 1;
    }

    // GVExtractor turns appending linkage into external linkage
    SmallVector<GlobalVariable *, 4> ToRemove;
    for (GlobalVariable &GV : NewModule->globals())
      if (GV.hasAppendingLinkage())
        ToRemove.push_back(&GV);
    for (GlobalVariable *GV : ToRemove)
      GV->removeFromParent();

    legacy::PassManager PM;
    PM.add(createGVExtractionPass(ToPreserve, false));
    PM.add(createInternalizePass(std::vector<const char *>{ExtractedName.c_str()}));
    PM.add(createBitcodeWriterPass(ExtractedOut.os(), true));
    PM.run(*NewModule);

    ExtractedOut.keep();
    delete NewModule;
  }

  Out.keep();
  ExtractedList.close();
}
