#ifndef CLING_INCREMENTAL_SYCL_COMPILER_H
#define CLING_INCREMENTAL_SYCL_COMPILER_H

#include "llvm/ADT/StringRef.h"
#include <deque>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace llvm {
class MemoryBuffer;
}

namespace cling {
class Transaction;
class InputValidator;
class Interpreter;
class CompilationOptions;
class DeclCollector;
} // namespace cling

namespace clang {
class ASTConsumer;
class CodeGenerator;
class CompilerInstance;
class DiagnosticConsumer;
class Decl;
class FileID;
class ModuleFileExtension;
class Parser;
} // namespace clang

namespace cling {

class DumpCodeEntry {
public:
  unsigned int isStatement;
  std::string code;
  Transaction *CurT;
  bool declSuccess;
  DumpCodeEntry(unsigned int isStatement, const std::string &input,
                Transaction *T, bool declSuccuss = false);
};

class Cpptransformer {
private:
  Interpreter *m_Interp;
  std::list<DumpCodeEntry> EntryList;
  std::ofstream DumpOut;
  unsigned int m_ModuleNo = 0;
  DeclCollector *m_Consumer;
  clang::CodeGenerator *m_CodeGen = nullptr;
  std::unique_ptr<clang::Parser> m_Parser;
  std::deque<std::pair<llvm::MemoryBuffer *, clang::FileID>> m_MemoryBuffers;
  using ModuleFileExtensions =
      std::vector<std::shared_ptr<clang::ModuleFileExtension>>;

public:
  std::unique_ptr<clang::CompilerInstance> m_CI;
  Cpptransformer();
  ~Cpptransformer();
  void Initialize();
  void parse(llvm::StringRef input);
};

///\brief The class is responsible for dump cpp code into dump.cpp
/// and then to be compiled by syclcompiler
class IncrementalSYCLDeviceCompiler {
private:
  Interpreter *m_Interpreter;
  std::list<DumpCodeEntry> EntryList;
  std::ofstream DumpOut;
  bool ExtractDeclFlag = false;
  Transaction *CurT;
  std::unique_ptr<InputValidator> m_InputValidator;
  ///\brief Transaction of the SYCL kernel head file
  ///
  Transaction **HeadTransaction = 0;
  bool secureCode;
  bool ClearFlag = false;
  std::unique_ptr<Cpptransformer> m_Ctran;

public:
  IncrementalSYCLDeviceCompiler(Interpreter *interp);
  ~IncrementalSYCLDeviceCompiler();
  void setExtractDeclFlag(const bool flag);
  void setClearFlag(const bool flag);
  bool dump(const std::string &input, Transaction *T, unsigned int isStatement,
            size_t wrap_point, bool declSuccess = false);
  void submit();
  bool compile(const std::string &input);
  void setTransaction(Transaction *T);
  void setDeclSuccess(Transaction *T);
  void removeCodeByTransaction(Transaction *T);
};



} // namespace cling

#endif // CLING_INCREMENTAL_SYCL_COMPILER_H