//===- llvmtuner/src/create-policy.cpp: create extraction policy-*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides classes for:
// + constructing a loop extraction policy
// + creating empty output modules for each top-level loop in a policy
//
// The classes for constructing loop extraction policies
// read in profile information describing the time spent in
// different top-level loops, the functions called in those loops, and the
// other loops executed within them, and produce a "policy" file describing
// how to extract subsets of code for optimization.  Each subset contains:
//	{loop,func1,...,funcN},
// where loop is a top-level loop described by a "qualified-loop-name" and 
// and each func is a "qualified-func-name".
// 
//===----------------------------------------------------------------------===//

#include <llvm/Support/CommandLine.h>

#include "LoopName.h"
#include "LoopPolicy.h"
#include "LoopCallProfile.h"

#include <set>
#include <string>
#include <vector>
#include <climits>
#include <iostream>
#include <sstream>

using namespace llvm;

uint32_t TUNING_UPPERBOUND = UINT_MAX;
uint32_t TUNING_LOWERBOUND = 0;


//===----------------------------------------------------------------------===//
// class ExtractPolicyBase:
// Base class policy that provides methods to read in the loop profile,
// create a loop extraction policy, and create empty bitcode modules 
// for each top-level loop chosen by a specific policy.
//===----------------------------------------------------------------------===//

class ExtractPolicyBase {
public:
  // Typedefs available to clients
  typedef std::set<unsigned> loop_set_t;
  typedef std::map<std::string, loop_set_t> FuncToLoopsMap;
  typedef std::multimap<unsigned, unsigned> RawPolicyMap;

  // Default constructor reads policy files immediately
  ExtractPolicyBase();
  
  // Create a policy using the profile information.
  // The returned LoopPolicy object must be deallocated by the caller.
  virtual LoopPolicy* computePolicy() = 0;

  // Convert a policy to the external persistent representation

protected:
  // Get loops
  loop_set_t& getLoopsForFunc(const std::string& funcName) {
    return funcToLoops[funcName];
  }

  // Utility to topologically sort the dynamically observed "nesting graph".
  // Returns a new, sorted nesting graph
  LoopCallProfile topologicalSort(const LoopCallProfile& dynCG);

  // Convert candidateLoops and thePolicy (a map<loop,func>) to a LoopPolicy.
  // The returned object must be deallocated by the caller.
  LoopPolicy* makeFormattedPolicy(const std::vector<LoopHeader>& candidateLoops,
				  const RawPolicyMap& thePolicy);

private:
  // Helper function for topological sort
  void visitNode(unsigned i, std::set<unsigned>& visited);

protected:
  // Local state implementing this class.  Accessible to subclasses.
  FuncToLoopsMap funcToLoops;
  LoopCallProfile DynCG;
};


ExtractPolicyBase::ExtractPolicyBase()
{
  // Read in the two profile files.
  DynCG.readProfiles();
}

// Convert candidateLoops and thePolicy (a map<loop,func>) to a LoopPolicy.
// The returned object must be deallocated by the caller.
LoopPolicy* ExtractPolicyBase::makeFormattedPolicy
    (const std::vector<LoopHeader>& candidateLoops,
     const RawPolicyMap& thePolicy)
{
  LoopPolicy* newPolicy = new LoopPolicy();

  // First, add all the top-level loops
  for (auto& LH: candidateLoops)
    newPolicy->addLoop(LoopName(LH.ModuleName, LH.Function, LH.HeaderId));

  // Then, add the loops (across the whole pgm) for each function
  for (auto& policyPair: thePolicy) {
    unsigned topLevelLoop = policyPair.first;
    unsigned funcToCall   = policyPair.second;
    newPolicy->addLoopForFunc("",
			      DynCG.getLoopNameForId(funcToCall).getFuncName(),
			      DynCG.getLoopNameForId(topLevelLoop));
  }
  return newPolicy;
}

void ExtractPolicyBase::visitNode(unsigned i, std::set<unsigned>& visited)
{
#if 0
  if (visited.find(i) != visited.end())
    return;
  visited.emplace(i);
  for (auto& j: DynCG) {
    
  }
#endif
}

// Utility to topologically sort the dynamically observed "nesting graph"
LoopCallProfile ExtractPolicyBase::topologicalSort(const LoopCallProfile& dynCG)
{
#if 0
  // Copied from Tom's code in tune.py :-).  Is this Tarjan's algo?
#endif
}

#if 0
//===----------------------------------------------------------------------===//
// class CommandLinePolicy:
// Policy for testing, specified externally via command line arguments.
//===----------------------------------------------------------------------===//

class CommandLinePolicy: public ExtractPolicyBase {
public:
  // Initialize a policy
  virtual LoopPolicy* computePolicy() override;
};

const FuncToLoopsMap* CommandLinePolicy::computePolicy()
{
  // Now, compute the mapping from function -> ids of basic blocks
  // for the <function,loop-id> pairs specified on the command line.
  FuncToLoopsMap funcToLoops;
  for (auto& LN : LoopsToExtract) {
    funcToLoops[LN.getFunction()].emplace(LN.getLoopId());
  }

  std::vector<std::pair<Loop *, LoopName>> ToExtract;

  return funcToLoops;
};


//===----------------------------------------------------------------------===//
// Command-line flags, arguments, and argument parsing
//===----------------------------------------------------------------------===//

struct LoopNameParser : public cl::parser<LoopName> {
  LoopNameParser(cl::Option &O) : parser(O) {}
  
  bool parse(cl::Option &O, StringRef ArgName, const std::string &Arg,
             LoopName &LH) {
    LH = LoopName(Arg);
    if (LH.getLoopId() <= 0)
      return true;
    return false;
  }
};

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("module name"), cl::Required);

static cl::list<LoopName, bool, LoopNameParser>
    LoopsToExtract("l",
                   cl::desc("Specify loop(s) to extract.\nDescribe a loop in "
                            "this format:\n\"[function],[loop header]\""),
                   cl::OneOrMore, cl::Prefix);


//===----------------------------------------------------------------------===//
// Simple driver to test policy extraction via command line
//===----------------------------------------------------------------------===//

int main(int argc, char** argv)
{
  std::string moduleName;
  std::vector<LoopName> loopNameVec;
  LoopPolicy& policy;

  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  cl::ParseCommandLineOptions(argc, argv, "top-level loop extractor");

  // Insert one loop name per argument, or "main,2" if no args provided
  if (argc <= 1)
    return usage(argc, argv);

  // Module name must be the first argument
  moduleName = argv[1];

  // Any remaining args are one or more optional loop specifiers
  if (argc <= 2)
    loopNameVec.push_back(LoopName("main,2"));
  else
    for (int i=2; i < argc; i++)
      loopNameVec.push_back(LoopName(argv[i]));
  
  // Insert the specified policies
  for (auto& loopName: loopNameVec)
    policy.addToPolicy(moduleName, loopName.getFuncName(), loopName);
  
  // Then print them all out as a simple test
  std::cout << policy;
  
  return 0;
}
#endif // #if 0


//===----------------------------------------------------------------------===//
// class ExtractThresholdPolicy:
// Policy to extract loops based on [min,max] thresholds:
//   min: loops must be at least min% of total time
//   max: loops must be no more than max% of total time
//===----------------------------------------------------------------------===//

class ExtractThresholdPolicy: public ExtractPolicyBase {
public:
  virtual LoopPolicy* computePolicy() override;
};

LoopPolicy* ExtractThresholdPolicy::computePolicy()
{
  // Result policy to create and return;
  std::vector<LoopHeader> candidateLoops;
  RawPolicyMap thePolicy;
  
  // Use base class method to read in the two profile files into:
  // this->CGNodes: Array listing the nodes
  // this->DynCG  : Matrix of profile info per loop and function
  const std::vector<LoopHeader>& CGNodes = DynCG.GraphNodeMeta();
  
  // Sort the nesting graph to help compare thresholds
  LoopCallProfile& sortedCG = DynCG; //topologicalSort(DynCG);
  
  // Sorting above guarantees that the caller appears before the callee
  // (outer loop before inner loop) in the sortedCG.  
  // Visit only outermost loops.
  std::set<unsigned> ignoreInner;
  //for (auto& ??: sortedCG) {	// FIXME: Compute sorted CG
  for (auto& LH: CGNodes) {
    unsigned i = LH.HeaderId;
    if (ignoreInner.find(i) != ignoreInner.end())
      continue;
    float time = (float) sortedCG.getFreq(i,i);
    if (time >= TUNING_LOWERBOUND && time <= TUNING_UPPERBOUND) {
      // If this is a loop, add it to the policy
      if (! DynCG.isFunction(LH.HeaderId))
	candidateLoops.push_back(LH);

      // Now add the loop for all the functions called by the loop
      for (auto j: DynCG.getNested(i)) {
	ignoreInner.emplace(j);		 // Remember nested loops to ignore
	
	// Add the functions called from the loop 'i' to the policy
	if (DynCG.isFunction(j))
	  thePolicy.emplace(i, j);	// Function j is called from loop i

      }//endfor j: DynCG.nested(i)
    }//endif (time is within the required range)
  }//endfor i: sortedCG

  return makeFormattedPolicy(candidateLoops, thePolicy);
}


//===----------------------------------------------------------------------===//
// Simple driver to test policy extraction using thresholds
//===----------------------------------------------------------------------===//

static cl::opt<int> pmin("pmin", cl::init(-1), cl::value_desc("min%"),
			 cl::desc("retain loops with %time >= this value "
				  "(default min: 0%)"));
static cl::opt<int> pmax("pmax", cl::init(-1), cl::value_desc("max%"),
			 cl::desc("retain loops with %time <= this value "
				  "(default max: 100%)"));

int main(int argc, char** argv)
{
  ExtractThresholdPolicy thresholdPolicyObj;

  cl::ParseCommandLineOptions(argc, argv, "Create a policy from profiles");
  if (pmin > 0)
    TUNING_LOWERBOUND = pmin;
  if (pmax < 100)
    TUNING_UPPERBOUND = pmax;
#ifndef NDEBUG
  std::cout<< argv[0]<< " pmin="<< pmin<< "% pmax="<< pmax<< "%" << std::endl;
#endif

  LoopPolicy* policy = thresholdPolicyObj.computePolicy();
  if (policy != nullptr) {
    std::cout << *policy;
    delete policy;
  }
  return 0;
}

