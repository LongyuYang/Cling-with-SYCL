#include "IncrementalSYCLDeviceCompiler.h"
#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/MetaProcessor/InputValidator.h"
#include "cling/Utils/SourceNormalization.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"

using namespace clang;
namespace cling {

class InterpreterConsumer : public ASTConsumer {
private:
  const ASTContext &m_context;
  IncrementalSYCLDeviceCompiler::MapUnique &m_UniqueToEntry;
  int lastUnique;

public:
  explicit InterpreterConsumer(
      const ASTContext &context,
      IncrementalSYCLDeviceCompiler::MapUnique &UniqueToEntry, int lastUnique)
      : m_context(context), m_UniqueToEntry(UniqueToEntry),
        lastUnique(lastUnique) {}
  virtual ~InterpreterConsumer() {}
  bool HandleTopLevelDecl(clang::DeclGroupRef DGR) {
    std::string DumpText;
    llvm::raw_string_ostream dump(DumpText);
    for (auto it = DGR.begin(); it != DGR.end(); it++) {
      Decl *D = *it;
      if (D->isFunctionOrFunctionTemplate()) {
        FunctionDecl *FD = cast<FunctionDecl>(D);
        std::string FunctionName = FD->getNameAsString();
        if (FunctionName.find("__cling_custom_sycl_") != 0)
          continue;
        int unique = std::stoi(FunctionName.substr(20));
        if (unique > lastUnique) {
          CompoundStmt *CS = dyn_cast<CompoundStmt>(FD->getBody());
          for (CompoundStmt::body_iterator I = CS->body_begin(),
                                           EI = CS->body_end();
               I != EI; ++I) {
            DeclStmt *DS = dyn_cast<DeclStmt>(*I);
            if (!DS) {
              // dump << std::string("void __cling_wrapper__costom__") +
              //                 std::to_string(++m_counter) +
              //                 std::string("(){\n");
              // (*I)->printPretty(dump, NULL, PrintingPolicy(LangOptions()));
              // dump << ";\n}\n";
              continue;
            }
            for (DeclStmt::decl_iterator J = DS->decl_begin();
                 J != DS->decl_end(); ++J) {
              (*J)->print(dump);
              dump << ";\n";
            }
          }
          if (m_UniqueToEntry.count(unique)) {
            dump << m_UniqueToEntry[unique]->code;
            dump.flush();
            m_UniqueToEntry[unique]->code = std::move(DumpText);
          }
        }
      }
    }
    dump.flush();
    return true;
  }
};

size_t IncrementalSYCLDeviceCompiler::m_UniqueCounter = 0;
const std::string IncrementalSYCLDeviceCompiler::dumpFile = "DumpFile.cpp";

class InterpreterClassAction : public ASTFrontendAction {
  IncrementalSYCLDeviceCompiler::MapUnique &m_UniqueToEntry;
  int lastUnique;

public:
  explicit InterpreterClassAction(
      IncrementalSYCLDeviceCompiler::MapUnique &UniqueToEntry, int lastUnique)
      : m_UniqueToEntry(UniqueToEntry), lastUnique(lastUnique) {}
  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
    InterpreterConsumer *consumer = new InterpreterConsumer(
        Compiler.getASTContext(), m_UniqueToEntry, lastUnique);
    return std::unique_ptr<clang::ASTConsumer>(consumer);
  }
};

DumpCodeEntry::DumpCodeEntry(unsigned int isStatement, const std::string &input,
                             Transaction *T, bool declSuccess /* = false*/)
    : isStatement(isStatement), CurT(T), declSuccess(declSuccess) {
  code = IncrementalSYCLDeviceCompiler::SyclWrapInput(input, isStatement);
  m_unique = IncrementalSYCLDeviceCompiler::m_UniqueCounter;
}

IncrementalSYCLDeviceCompiler::IncrementalSYCLDeviceCompiler(
    Interpreter *interp, std::string SYCL_BIN_PATH, const char *llvmdir)
    : m_Interpreter(interp), SYCL_BIN_PATH(std::move(SYCL_BIN_PATH)) {
  m_InputValidator.reset(new InputValidator());
  HeadTransaction = new Transaction *;
  *HeadTransaction = NULL;
  secureCode = false;
  DumpOut.open(dumpFile, std::ios::in | std::ios::out | std::ios::trunc);
  DumpOut.close();

  getSYCLCompileOpt(interp, m_Args, llvmdir);
}

IncrementalSYCLDeviceCompiler::~IncrementalSYCLDeviceCompiler() {
  delete HeadTransaction;
  m_InputValidator.reset(0);
  for (auto arg : m_Args) {
    delete[] arg;
  }
  remove(dumpFile.c_str());
  remove("KernelInfo.h");
  remove("DeviceCode.spv");
}

std::string
IncrementalSYCLDeviceCompiler::SyclWrapInput(const std::string &Input,
                                             unsigned int is_statement) {
  std::string Wrapper(Input);
  if (is_statement) {
    std::string Header = std::string("void __cling_custom_sycl_") +
                         std::to_string(m_UniqueCounter) +
                         std::string("() {\n");
    Wrapper.insert(0, Header);
    Wrapper.append("\n;\n}");
    return Wrapper;
  }
  return Wrapper;
}

void IncrementalSYCLDeviceCompiler::insertCodeEntry(unsigned int is_statement,
                                                    const std::string &input,
                                                    Transaction *T) {
  EntryList.push_back(DumpCodeEntry(is_statement, input, T));
  UniqueToEntry[IncrementalSYCLDeviceCompiler::m_UniqueCounter] =
      --EntryList.end();
  m_Uniques.push_back(IncrementalSYCLDeviceCompiler::m_UniqueCounter++);
}

bool IncrementalSYCLDeviceCompiler::compile(const std::string &input,
                                            Transaction *T,
                                            unsigned int isStatement,
                                            size_t wrap_point,
                                            bool declSuccess /* = false*/) {
  if ((isStatement == 0) && (!ExtractDeclFlag))
    return true;
  std::istringstream input_holder(input);
  std::string line;
  std::string complete_input;
  m_Uniques.clear();
  while (getline(input_holder, line)) {
    if (line.empty() || (line.size() == 1 && line.front() == '\n')) {
      continue;
    }
    if (m_InputValidator->validate(line) == InputValidator::kIncomplete) {
      continue;
    }
    complete_input.clear();
    m_InputValidator->reset(&complete_input);
    size_t wrapPoint = std::string::npos;
    wrapPoint = utils::getWrapPoint(complete_input,
                                    m_Interpreter->getCI()->getLangOpts());
    if (wrapPoint == std::string::npos) {
      insertCodeEntry(0, complete_input, T);
    } else if (wrapPoint == 0) {
      insertCodeEntry(1, complete_input, T);
    } else {
      insertCodeEntry(0, complete_input.substr(0, wrapPoint), T);
      std::string wrappedinput(complete_input.substr(wrapPoint));
      insertCodeEntry(1, wrappedinput, T);
    }
    if (!refactorCode()) {
      return false;
    }
    lastUnique = m_Uniques.back();
  }

  return compileImpl(input);
}

void IncrementalSYCLDeviceCompiler::dump(const std::string &target) {
  std::error_code EC;
  llvm::raw_fd_ostream File(target, EC, llvm::sys::fs::F_Text);
  for (auto &CodeEntry : EntryList) {
    File << CodeEntry.code;
    for (auto sit = CodeEntry.code.rbegin(); sit != CodeEntry.code.rend();
         sit++) {
      if (*sit != ' ') {
        if (CodeEntry.isStatement == 0) {
          if (*sit == '}')
            File << ";\n";
          else
            File << "\n";
          break;
        } else {
          if (*sit == ';')
            File << '\n';
          else
            File << ";\n";
          break;
        }
      }
    }
  }
  File.close();
}

bool IncrementalSYCLDeviceCompiler::refactorCode() {
  dump(dumpFile);

  // Initialize CompilerInstance
  CompilerInstance CI;
  clang::CompilerInvocation::CreateFromArgs(CI.getInvocation(), m_Args.data(),
                                            m_Args.data() + m_Args.size(),
                                            CI.getDiagnostics());

  // create Diagnostics after create args to suppress all warnings
  CI.createDiagnostics();
  assert(CI.hasDiagnostics());

  std::unique_ptr<FrontendAction> action(
      new InterpreterClassAction(UniqueToEntry, lastUnique));
  if (!CI.ExecuteAction(*action)) {
    removeCodeByTransaction(NULL);
    return false;
  }
  dump(dumpFile);
  return true;
}

bool IncrementalSYCLDeviceCompiler::compileImpl(const std::string &input) {
  // Dump the code of every CodeEntry
  setExtractDeclFlag(false);
  secureCode = true;
  DumpOut.open("KernelInfo.h", std::ios::in | std::ios::out | std::ios::trunc);
  DumpOut.close();
  std::string command = SYCL_BIN_PATH +
                        "/clang++ -w -fsycl-device-only  -fno-sycl-use-bitcode "
                        "-Xclang -fsycl-int-header=KernelInfo.h -c "
                        "DumpFile.cpp -o DeviceCode.spv";
  for (auto &arg : m_ICommandInclude) {
    command = command + " " + arg;
  }
  int sysReturn = std::system(command.c_str());
  if (sysReturn != 0) {
    secureCode = false;
    removeCodeByTransaction(NULL);
    return false;
  }
  if (HeadTransaction && HeadTransaction[0]) {
    m_Interpreter->unload(HeadTransaction[0][0]);
    HeadTransaction[0] = NULL;
  }
  std::ifstream headFile;
  headFile.open("KernelInfo.h");
  std::ostringstream tmp;
  tmp << headFile.rdbuf();
  std::string headFileContent = tmp.str();
  headFile.close();
  if (m_Interpreter->declare(headFileContent.c_str(), HeadTransaction) !=
      Interpreter::kSuccess) {
    secureCode = false;
    removeCodeByTransaction(NULL);
    return false;
  }
  secureCode = false;
  return true;
}

void IncrementalSYCLDeviceCompiler::setTransaction(Transaction *T) {
  for (auto u : m_Uniques) {
    UniqueToEntry[u]->CurT = T;
  }
}

void IncrementalSYCLDeviceCompiler::setDeclSuccess(Transaction *T) {
  if (!secureCode) {
    if (T) {
      setTransaction(T);
    } else {
      for (auto u : m_Uniques) {
        UniqueToEntry[u]->declSuccess = true;
      }
    }
  }
}

void IncrementalSYCLDeviceCompiler::removeCodeByTransaction(Transaction *T) {
  if (!secureCode) {
    for (auto it = EntryList.begin(); it != EntryList.end();) {
      if (!it->declSuccess && (it->CurT == NULL || it->CurT == T)) {
        UniqueToEntry.erase(it->m_unique);
        it = EntryList.erase(it);
      } else {
        it++;
      }
    }
  }
}

void IncrementalSYCLDeviceCompiler::addCompileArg(const std::string &arg1,
                                                  const std::string &arg2) {
  char *tmpArg1 = new char[arg1.length() + 1];
  strcpy(tmpArg1, arg1.c_str());
  m_Args.push_back(tmpArg1);
  m_ICommandInclude.push_back(arg1);
  if (arg2.length() > 0) {
    char *tmpArg2 = new char[arg2.length() + 1];
    strcpy(tmpArg2, arg2.c_str());
    m_Args.push_back(tmpArg2);
    m_ICommandInclude.push_back(arg2);
  }
}

} // namespace cling