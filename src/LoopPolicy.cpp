//===- llvmtuner/src/LoopPolicy.cpp: loops to extract -----------*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file reads in profile information describing the time spent in
// different top-level loops, the functions called in those loops, and the
// other loops executed within them, and produces a "policy" file describing
// how to extract subsets of code for optimization.  Each subset contains:
//	{loop,func1,...,funcN},
// where loop is a top-level loop described by a "qualified-loop-name" and 
// and each func is a "qualified-function-name".
// 
// To support separate extraction from individual (unlinked) LLVM modules,
// the actual policy file has more information.  The file format is:
//	list<ModuleName,
//	     list<local-loop-name>,
//	     map<func,qualified-loop-name>>
// For each module "ModuleName", this says:
// (a) which top-level loops must be extracted into a target file;
// (b) for each function in the module, which top-level loop in *any* module
//     needs a copy of that function.
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

#include "LoopName.h"
#include "LoopPolicy.h"

//===----------------------------------------------------------------------===//
// class ModulePolicyInfo
//
// Describes a policy for one module.
//===----------------------------------------------------------------------===//

class ModulePolicyInfo
{
  // Internal state
  const std::string& thisModule;
  std::vector<uint32_t> loopIds;
  std::map<std::string, std::vector<LoopName>* > funcToLoopMap;
  
  // Retrieving and adding a loop policy for a specified function
  std::vector<LoopName>* getOrInsertLoopsForFunc(const std::string& funcName) {
    std::vector<LoopName>*& vectorEntry = funcToLoopMap[funcName];
    if (vectorEntry == NULL)
      vectorEntry = new std::vector<LoopName>;
    assert(funcToLoopMap[funcName] == vectorEntry && "Bad map insertion (1)?");
    return vectorEntry;
  }

public:
  // Ctors and dtors
  ModulePolicyInfo(const std::string &_module) : thisModule(_module) { }
  ~ModulePolicyInfo() {
    for (auto &mapEntries: funcToLoopMap)
      delete mapEntries.second;
  }

  // Insert LoopName for a function in the map
  void addLoopForFunc(const LoopName& loopName) {
    std::vector<LoopName>* vectorEntry = 
      getOrInsertLoopsForFunc(loopName.getFuncName());
    vectorEntry->push_back(loopName);
  }
  
  // Query information about the policy
  const std::string &getModule() const			{ return thisModule; }
  const std::vector<uint32_t>& getLoops()  const	{ return loopIds; }
  const std::vector<LoopName>& getLoopsForFunc(const std::string& funcName) {
    return * getOrInsertLoopsForFunc(funcName);
  }

  // Write out and read back one policy
  void print(std::ostream &os) const;
};


// Write the per-module policy information to an output stream. Format:
// Line 1: module-name: "loops: " loopid1,...,loopidN "\n"
// Line 2: module-name: func:qualified-loop-id1,...,qualified-loop-idN "\n"
void ModulePolicyInfo::print(std::ostream &os) const
{
  // Write out the loop ids as a simple serialization, in one line.
  os << "loops: ";
  for (auto &id: loopIds)
    os << id;
  os << std::endl;
  
  // Write out the list of qualified-loop-ids for each function, one per line
  for (auto &mapEntries: funcToLoopMap) {
    os << mapEntries.first << ": ";
    for (auto &loopName: *mapEntries.second)
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

// Add a new policy for one loop in one module
void LoopPolicy::addPolicy(const std::string& moduleName,
			   const std::string& funcName,
			   const LoopName& loopName)
{
  getOrCreatePolicy(moduleName).addLoopForFunc(loopName);
}

void LoopPolicy::print(std::ostream& os) const
{
  // Write out the map information as a simple serialization,
  // one line per map entry.  Start with number of entries to simplify alloc.
  os << modulePolicies.size() << "\n";
  for (auto &mapEntries: modulePolicies) {
    os << "Module " << mapEntries.first << ":";
    os << *mapEntries.second << std::endl;
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
  for (auto &mapEntries: modulePolicies) {
    os << mapEntries.first << ":";
    for (auto &modulePolicies: mapEntries.second)
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
    policy.addPolicy(loopName.getModule(), loopName.getFuncName(), loopName);
  
  // Then print them all out as a simple test
  std::cout << policy;
  
  return 0;
}
#endif
