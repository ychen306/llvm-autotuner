//===- llvmtuner/src/LoopPolicy.cpp: loops to extract -----------*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// class LoopPolicy: A table of ModulePolicyInfo for each LLVM module.
// class ModulePolicyInfo: A loop extraction policy for one LLVM module.
// 
// This file provides classes to describe policies for
// how to extract subsets of code for optimization.  Each subset contains:
//	{loop,func1,...,funcN},
// where loop is a top-level loop described by a "qualified-loop-name" and 
// and each func is a "qualified-func-name".
// 
// To support separate extraction from individual (unlinked) LLVM modules,
// these policies are stored external policy files used to communicate between
// different phases.  The file format is:
//	list(triple(ModuleName,
//	            list(top-level-loop-name),
//	            list(pair(func,
//                            list(qualified-loop-name)))
// This says:
// For each module "ModuleName",
// (a) a list of top-level loops must be extracted
// (b) for each function in the module, which top-level loop(s) in *any*
//     module need a copy of that function.
// The latter is listed for each function, not each loop, so that each function
// in the module ModuleName can be extracted to multiple output files in a
// single pass over the module.
//
// See LoopName.h for descriptions of qualified-loop-name, qualified-func-name.
// 
//===----------------------------------------------------------------------===//

#include <string>
#include <assert.h>
#include <stdint.h>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <set>
#include "LoopName.h"
#include "LoopPolicy.h"

//===----------------------------------------------------------------------===//
// class ModulePolicyInfo
//
// Describes a policy for one module.
//===----------------------------------------------------------------------===//

class ModulePolicyInfo
{
public:
  // Public types
  typedef std::set<LoopName, struct LoopNameComp> LoopNameSet;

  // Ctors and dtors
  ModulePolicyInfo(const std::string &_module) : thisModule(_module) { }
  ~ModulePolicyInfo() {
    for (auto &mapEntry: funcToLoopMap)
      delete mapEntry.second;
  }

  // Insert one top-level loop
  void addLoop(const LoopName& loopName) {
    loops.emplace(loopName);
  }

  // Insert LoopName for a function in the map
  void addLoopForFunc(const LoopName& loopName, const std::string& funcName) {
    std::vector<LoopName>* vectorEntry = getOrInsertLoopsForFunc(funcName);
    vectorEntry->push_back(loopName);
  }
  
  // Query information about the policy
  const std::string& getModule() const	{ return thisModule; }
  const LoopNameSet& getLoops()  const	{ return loops; }
  const std::vector<LoopName>& getLoopsForFunc(const std::string& funcName) {
    std::vector<LoopName>* vectorEntry = getOrInsertLoopsForFunc(funcName);
    return * vectorEntry;
  }

  // Write out and read back one policy
  void print(std::ostream& os) const;

private:
  // Internal state
  const std::string& thisModule;
  LoopNameSet loops;
  std::map<std::string, std::vector<LoopName>* > funcToLoopMap;
  
  // Retrieving and adding a loop policy for a specified function
  std::vector<LoopName>* getOrInsertLoopsForFunc(const std::string& funcName) {
    std::vector<LoopName>*& vectorEntry = funcToLoopMap[funcName];
    if (vectorEntry == NULL)
      vectorEntry = new std::vector<LoopName>;
    assert(funcToLoopMap[funcName] == vectorEntry && "Bad map insertion (1)?");
    return vectorEntry;
  }

};


// Write the per-module policy information to an output stream. Format:
// Line 1: module-name: "loops: " loopid1,...,loopidN "\n"
// Line 2: module-name: func:qualified-loop-id1,...,qualified-loop-idN "\n"
void ModulePolicyInfo::print(std::ostream &os) const
{
  // Write out the descriptors of top-level loops, one per line.
  os << "loops: " << std::endl;
  for (auto &LN: loops)
    os << LN << std::endl;
  
  // Write out the list of qualified-loop-ids for each function, one per line
  os << "functions: " << std::endl;
  for (auto &mapEntry: funcToLoopMap) {
    os << mapEntry.first << ": ";
    for (auto &loopName: *mapEntry.second)
      os << loopName;
    os << std::endl;
  }
}

inline std::ostream& operator<<(std::ostream& os,
				const ModulePolicyInfo& policyInfo)
{
  policyInfo.print(os);
  return os;
}

#if 0
// Write the per-module policy information to an output stream
std::istream& operator >>(std::istream& is, ModulePolicyInfo& policyInfo)
{
  // Read data from stream, construct a ModulePolicyInfo, and return it
  return is;
}
#endif


//===----------------------------------------------------------------------===//
// class LoopPolicy
//
// Describes policies for multiple modules; each one is a ModulePolicyInfo.
//===----------------------------------------------------------------------===//

// LoopPolicy dtor
//
LoopPolicy::~LoopPolicy()
{
  for (auto &MapEntries: modulePolicies)
    delete MapEntries.second;
}

// Look up the policy for a module.  Insert an empty one if none exists.
// 
ModulePolicyInfo& LoopPolicy::getOrCreatePolicy(const std::string& moduleName)
{
  ModulePolicyInfo *&policy = modulePolicies[moduleName];
  if (policy == NULL)
    policy = new ModulePolicyInfo(moduleName);
  assert(modulePolicies[moduleName] == policy && "Bad map insertion (2)?");
  return *policy;
}

// Add a top-level loop
//
void LoopPolicy::addLoop(const LoopName& loopName)
{
  std::string moduleName = loopName.getModule();
  assert(moduleName.length() > 0 && "Invalid module name: empty string.");
  if (moduleName.length() > 0)
    getOrCreatePolicy(moduleName).addLoop(loopName);
}

// Add a new entry for the policy for one function in one module
// 
void LoopPolicy::addLoopForFunc(const std::string& moduleName,
				const std::string& funcName,
				const LoopName& loopName)
{
  assert(moduleName.length() > 0 && "Invalid module name: empty string.");
  getOrCreatePolicy(moduleName).addLoopForFunc(loopName, funcName);
}

void LoopPolicy::print(std::ostream& os) const
{
  // Write out the map information as a simple serialization,
  // one line per map entry.  Start with number of entries to simplify alloc.
  os << modulePolicies.size() << " modules for this program.\n";
  for (auto &mapEntry: modulePolicies) {
    os << "Module " << mapEntry.first << ":" << std::endl;
    os << *mapEntry.second << std::endl;
  }
}

// Write the per-module policy information to an output stream
std::ostream& operator<<(std::ostream& os, const LoopPolicy& policy)
{
  policy.print(os);
  return os;
}

#if 0
// Write the per-module policy information to an output stream
std::istream& operator >>(std::istream& is, LoopPolicy& policy)
{
  // Read back the map information from a file.
  // 
  os << modulePolicies.size() << "\n";
  for (auto &mapEntry: modulePolicies) {
    os << mapEntry.first << ":";
    for (auto &modulePolicies: mapEntry.second)
      os << modulePolicies;
    os << "\n";
  }
  return is;
}
#endif


//===----------------------------------------------------------------------===//
// Simple test driver.
// Usage: LoopExtractionPolicy [F1,L1 [, F2,L2, [...]]
//===----------------------------------------------------------------------===//

#if 0
int main(int argc, char** argv)
{
  std::vector<LoopName> loopNameVec;
  LoopPolicy policy;
  
  // Insert one loop name per argument, or "main,1" if no args provided
  if (argc <= 1) 
    loopNameVec.push_back(LoopName("main,1"));
  else
    for (int i=1; i < argc; i++)
      loopNameVec.push_back(LoopName(argv[i]));
  
  // Insert the specified policies
  for (LoopName& loopName: loopNameVec)
    policy.addLoopForFunc(loopName.getModule(), loopName.getFuncName(),
			  loopName);
  
  // Then print them all out as a simple test
  std::cout << policy;
  
  return 0;
}
#endif
