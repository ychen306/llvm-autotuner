#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
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

#include <iterator>
#include <sstream>
#include <cstdio>

using namespace llvm;

static
cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input file>"),
                                   cl::Required);

static
cl::opt<std::string> OutputFilename("o", cl::desc("Specify output file name"),
                                    cl::value_desc("output file"), cl::init("-"));

static
cl::opt<bool> ListFunctions("list-functions",
                             cl::desc("List functions and functions declarations in the module"),
                             cl::init(false));


struct TransformImpl { 
  virtual bool apply(Module *M) const = 0;
  virtual ~TransformImpl() {}
};

class Transform {
  std::shared_ptr<TransformImpl> Impl; 
public:
  Transform() : Impl(nullptr) {}
  void setImpl(std::shared_ptr<TransformImpl> TheImpl) { Impl = TheImpl; }

  bool apply(Module *M) const {
    return Impl->apply(M);
  }

  ~Transform() {}
};

// move `Src` before `Dest`
template<typename LIST_TYPE, typename ITR_TYPE>
void moveBefore(LIST_TYPE &list, ITR_TYPE Src, ITR_TYPE Dest)
{ 
  Src->removeFromParent();

  if (Dest == list.end())
    list.push_back(Src);
  else
    list.insert(Dest, Src);
}

template <typename LIST_TYPE, typename ITR_TYPE>
bool swap(LIST_TYPE &list, ITR_TYPE I1, ITR_TYPE I2)
{
    if (I1 == list.end() || I2 == list.end() || I1 == I2)
      return false;

    ITR_TYPE NextOf1 = I1, NextOf2 = I2;
    ++NextOf1;
    ++NextOf2;

    if (NextOf1 == I2) {
      moveBefore(list, I2, I1);
    } else if (NextOf2 == I1) {
      moveBefore(list, I1, I2);
    } else { // there are functions between f1 and f2
      moveBefore(list, I1, NextOf2);
      moveBefore(list, I2, NextOf1);
    }

    return true;
}

class SwapBasicBlocks : public TransformImpl {
  const std::string F;
  const unsigned B1, B2;

public:
  SwapBasicBlocks(const std::string Func, const unsigned BB1, const unsigned BB2) :
    F(Func), B1(BB1), B2(BB2) {}

  bool apply(Module *M) const override {
    // can't reorder entry blocks
    if (B1 == 0 || B2 == 0)
      return false;

    Function *Func = M->getFunction(F);

    Function::iterator BB1, BB2, Itr = Func->begin();
    for (unsigned i = 0; i <= std::max(B1, B2); i++) {
      if (i == B1) {
        BB1 = Itr;
      }
      if (i == B2) {
        BB2 = Itr;
      }
      ++Itr;
    }

    return swap(Func->getBasicBlockList(), BB1, BB2);
  }

};

class MoveBasicBlocks : public TransformImpl {
  const std::string F;
  const unsigned B1, B2;

public:
  MoveBasicBlocks(const std::string Func, const unsigned BB1, const unsigned BB2) :
    F(Func), B1(BB1), B2(BB2) {}
  
  // FIXME code duplication
  bool apply(Module *M) const override {
    Function *Func = M->getFunction(F);

    Function::iterator BB1, BB2, Itr = Func->begin();
    for (unsigned i = 0; i <= std::max(B1, B2); i++) {
      if (i == B1) {
        BB1 = Itr;
      }
      if (i == B2) {
        BB2 = Itr;
      }
      ++Itr;
    }

    // we can't reorder entry block
    if (BB2 == Func->begin() ||
        BB1 == Func->begin() ||
        BB1 == Func->end()) return false;

    moveBefore(Func->getBasicBlockList(), BB1, BB2);

    return true;
  }

};

class SwapFunctions : public TransformImpl {
  const std::string Func1, Func2;

public:
  SwapFunctions(const std::string &F1, const std::string &F2) : Func1(F1), Func2(F2) {}

  bool apply(Module *M) const override { 
    Module::iterator F1 = M->getFunction(Func1), F2 = M->getFunction(Func2);
    return swap(M->getFunctionList(), F1, F2);
  }
};

class MoveFunction : public TransformImpl {
  const std::string Func1, Func2;

public:
  MoveFunction(const std::string &F1, const std::string &F2) : Func1(F1), Func2(F2) {}
  bool apply(Module *M) const override { 
    Module::iterator Src = M->getFunction(Func1),
      Dest = M->getFunction(Func2);

    if (Src == Dest ||
        Src == M->end()) return false;

    moveBefore(M->getFunctionList(), Src, Dest);

    return true;
  }
};

struct TransformParser : public cl::parser<Transform> {
  TransformParser(cl::Option &O) : parser(O) {}

  bool parse(cl::Option &O, StringRef ArgName, const std::string &Arg,
             Transform &LH) {
    std::string Operator, Operand1, Operand2;
    std::stringstream Fields(Arg);
    std::getline(Fields, Operator, ',');
    std::getline(Fields, Operand1, ',');
    std::getline(Fields, Operand2, ',');
    if (Fields.fail()) {
      errs() << "invalid transformation\n";
      return true;
    }

    std::shared_ptr<TransformImpl> Impl;
    if (Operator == "s") { 
      Impl = std::make_shared<SwapFunctions>(Operand1, Operand2);
    } else if (Operator == "m") {
      Impl = std::make_shared<MoveFunction>(Operand1, Operand2);
    } else if (Operator[0] == 's' && Operator.size() > 1) {
      auto Func = Operator.substr(1);
      Impl = std::make_shared<SwapBasicBlocks>(Func,
                                               std::stoi(Operand1),
                                               std::stoi(Operand2));
    } else if (Operator[0] == 'm' && Operator.size() > 1) {
      auto Func = Operator.substr(1);
      Impl = std::make_shared<MoveBasicBlocks>(Func,
                                               std::stoi(Operand1),
                                               std::stoi(Operand2));
    } else {
      errs() << "invalid transformation\n";
      return true;
    }

    LH.setImpl(Impl);
    return false;
  }
};

static cl::list<Transform, bool, TransformParser>
Transforms("t",
           cl::desc("Specify what transformation to apply.\nDescribe a transformation in "
                    "this format:\n\"[type],[func1],[func2]\"\n"
                    "Type can be 's' (swap) or 'm' (move)\n"
                    "\"m,foo,bar\" means moving the `foo` before `bar`(and shift functions between them)\n"
                    "\"s,foo,bar\" means swapping swapping `foo` and `bar`"),
           cl::ZeroOrMore, cl::Prefix);


bool reorder(Module *M)
{
  for (const auto &T : Transforms) {
    bool Success = T.apply(M);
    if (!Success) return false;
  }
  return true;
} 

void scanModule(Module *M, std::vector<std::pair<Function *, unsigned>> &Functions,
                std::vector<Function *> &Declarations)
{
  for (Function &F : *M) {
    if (F.empty())
      Declarations.push_back(&F);
    else {
      unsigned NumBBs = 0;
      for (BasicBlock &BB : F)
        NumBBs++;
      Functions.emplace_back(std::pair<Function *, unsigned>(&F, NumBBs));
    }
  }
}

int main(int argc, char **argv)
{
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  cl::ParseCommandLineOptions(argc, argv, "reorder function");

  LLVMContext &Context = getGlobalContext();
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);

  if (ListFunctions) {
    std::vector<std::pair<Function *, unsigned>> Functions;
    std::vector<Function *>Declarations;
    scanModule(M.get(), Functions, Declarations);

    for (unsigned i = 0; i < Functions.size(); i++) {
      Function *Fn;
      unsigned NumBBs;
      std::tie(Fn, NumBBs) = Functions[i];
      outs() << Fn->getName() << '|' << NumBBs;
      if (i == Functions.size() - 1)
        outs() << "\n";
      else
        outs() << ",";
    }

    for (unsigned i = 0; i < Declarations.size(); i++) {
      if (i == Declarations.size() - 1)
        outs() << Declarations[i]->getName() << "\n";
      else
        outs() << Declarations[i]->getName() << ",";
    }

    return 0;
  }

  std::error_code EC;
  tool_output_file Out(OutputFilename, EC, sys::fs::F_None);
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }

  if (!reorder(M.get())) {
    errs() << "Failed to reorder functions due to invalid transformation(s)\n";
    exit(1);
  }

  legacy::PassManager PM;
  PM.add(createVerifierPass());
  PM.add(createBitcodeWriterPass(Out.os(), true));
  PM.run(*M.get());

  Out.keep();
}
