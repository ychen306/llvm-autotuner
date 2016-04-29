#include <string>
#include <vector>
#include <iostream>

#include "LoopName.h"
#include "LoopPolicy.h"



static int usage(int argc, char** argv)
{
  std::cout << "Usage: "
	    << argv[0] << " module [F1,L1 [, F2,L2 [, ...]]]"
	    << std::endl;
  return 1;
}

// Extract policies via comamnd-line arguments
// 
int main(int argc, char** argv)
{
  std::string moduleName;
  std::vector<LoopName> loopNameVec;
  LoopPolicy policy;
  
  // Insert one loop name per argument, or "main,1" if no args provided
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
  for (LoopName& loopName: loopNameVec)
    policy.addPolicy(moduleName, loopName.getFuncName(), loopName);
  
  // Then print them all out as a simple test
  std::cout << policy;
  
  return 0;
}
