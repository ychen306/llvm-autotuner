//===- llvmtuner/src/LoopPolicy.h: policies to extract loops ----*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// class LoopPolicy:
// Describes policies for multiple modules.  Each one is a ModulePolicyInfo.
//
//===----------------------------------------------------------------------===//

#ifndef LOOP_POLICY_H
#define LOOP_POLICY_H

#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <iostream>

class LoopName;
class ModulePolicyInfo;

//===----------------------------------------------------------------------===//

class LoopPolicy
{
  // Types used within this class
  typedef std::map<const std::string, ModulePolicyInfo*> PolicyMap;
  typedef       PolicyMap::iterator       iterator;
  typedef const PolicyMap::iterator const_iterator;

  // Look up the policy for a module.  Insert an empty one if none exists.
  ModulePolicyInfo& getOrCreatePolicy(const std::string& moduleName);

public:
  // dtor: release memory for all ModulePoicyInfo objects in the map
  ~LoopPolicy();

  // Add a top-level loop to the policy
  void addLoop(const LoopName& loopName);
  
  // Add one loop for a function
  void addLoopForFunc(const std::string& moduleName,
		      const std::string& funcName, const LoopName& loopName);

  // Helper for writing out the policies
  void print(std::ostream &os) const;

private:
  // Internal state implementing this class
  PolicyMap modulePolicies;
};

// Writing out and reading back the policies
extern std::ostream& operator <<(std::ostream&, const LoopPolicy&);
#if 0
  friend std::istream& operator >>(std::istream&, const LoopPolicy&);
#endif


//===----------------------------------------------------------------------===//

#endif // ifndef LOOP_POLICY_H
