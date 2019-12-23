#include "IncrementalSYCLDeviceCompiler.h"
#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/MetaProcessor/InputValidator.h"
#include "cling/Utils/SourceNormalization.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

using namespace clang;
namespace cling {

  std::string getRawSourceCode(const ASTContext& context, SourceRange SR) {
    SourceLocation decl_begin = SR.getBegin();
    SourceLocation decl_end =
        Lexer::getLocForEndOfToken(SR.getEnd(), 0, context.getSourceManager(),
                                   context.getLangOpts());
    if (decl_begin.isMacroID()) {
      decl_begin =
          context.getSourceManager().getImmediateMacroCallerLoc(decl_begin);
    }
    const char* buf_begin =
        context.getSourceManager().getCharacterData(decl_begin);
    const char* buf_end = context.getSourceManager().getCharacterData(decl_end);
    return std::string(buf_begin, buf_end);
  }

  // Rewrite ASTConsumer to implement Decl Extractor to make the Decl inside
  // the wrap function Global
  class InterpreterConsumer : public ASTConsumer {
  private:
    const ASTContext& m_context;
    IncrementalSYCLDeviceCompiler::MapUnique& m_UniqueToEntry;
    int lastUnique;
    const std::vector<const char*> clearTargets = {"= <null expr>"};
    // Clear Unexpected Output introduced by Decl::print
    void clearPrint(std::string& input) {
      for (auto targetCString : clearTargets) {
        std::string target(targetCString);
        size_t pos = 0;
        while (pos < input.length() &&
               (pos = input.find(target, pos)) != std::string::npos) {
          input.erase(pos, target.length());
        }
      }
    }

  public:
    explicit InterpreterConsumer(
        const ASTContext& context,
        IncrementalSYCLDeviceCompiler::MapUnique& UniqueToEntry, int lastUnique)
        : m_context(context), m_UniqueToEntry(UniqueToEntry),
          lastUnique(lastUnique) {}
    virtual ~InterpreterConsumer() {}
    bool HandleTopLevelDecl(clang::DeclGroupRef DGR) {
      std::string DumpText;
      llvm::raw_string_ostream dump(DumpText);
      for (auto it = DGR.begin(); it != DGR.end(); it++) {
        Decl* D = *it;
        if (D->isFunctionOrFunctionTemplate()) {
          // Extract only the decl inside wrapper function
          FunctionDecl* FD = cast<FunctionDecl>(D);
          std::string FunctionName = FD->getNameAsString();
          if (FunctionName.find("__cling_custom_sycl_") != 0)
            continue;
          int unique = std::stoi(FunctionName.substr(20));
          // Only handle the new wrapper
          if (unique > lastUnique) {
            CompoundStmt* CS = dyn_cast<CompoundStmt>(FD->getBody());
            for (CompoundStmt::body_iterator I = CS->body_begin(),
                                             EI = CS->body_end();
                 I != EI; ++I) {
              DeclStmt* DS = dyn_cast<DeclStmt>(*I);
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
                // Use Decl::print to dump the AST into cpp 
                (*J)->print(dump);
                // dump << getRawSourceCode(m_context, (*J)->getSourceRange());
                dump << ";\n";
              }
            }
            if (m_UniqueToEntry.count(unique)) {
              dump << m_UniqueToEntry[unique]->code;
              dump.flush();
              clearPrint(DumpText);
              m_UniqueToEntry[unique]->code = std::move(DumpText);
            }
          }
        }
      }
      dump.flush();
      return true;
    }
  };

  class InterpreterClassAction : public ASTFrontendAction {
    IncrementalSYCLDeviceCompiler::MapUnique& m_UniqueToEntry;
    int lastUnique;

  public:
    explicit InterpreterClassAction(
        IncrementalSYCLDeviceCompiler::MapUnique& UniqueToEntry, int lastUnique)
        : m_UniqueToEntry(UniqueToEntry), lastUnique(lastUnique) {}
    virtual std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance& Compiler,
                      llvm::StringRef InFile) {
      InterpreterConsumer* consumer =
          new InterpreterConsumer(Compiler.getASTContext(), m_UniqueToEntry,
                                  lastUnique);
      return std::unique_ptr<clang::ASTConsumer>(consumer);
    }
  };

  DumpCodeEntry::DumpCodeEntry(unsigned int isStatement,
                               const std::string& input, Transaction* T,
                               bool declSuccess /* = false*/)
      : isStatement(isStatement), CurT(T), declSuccess(declSuccess) {
    code = IncrementalSYCLDeviceCompiler::SyclWrapInput(input, isStatement);
    m_unique = IncrementalSYCLDeviceCompiler::m_UniqueCounter;
  }

  // Initialize static members of IncrementalSYCLDeviceCompiler
  size_t IncrementalSYCLDeviceCompiler::m_UniqueCounter = 0;
  const std::string IncrementalSYCLDeviceCompiler::dumpFile = "DumpFile.cpp";
  const std::string IncrementalSYCLDeviceCompiler::kernelInfoFile =
      "KernelInfo.h";
  const std::string IncrementalSYCLDeviceCompiler::spvFile = "DeviceCode.spv";

  IncrementalSYCLDeviceCompiler::IncrementalSYCLDeviceCompiler(
      Interpreter* interp, std::string SYCL_BIN_PATH, const char* llvmdir)
      : m_Interpreter(interp), SYCL_BIN_PATH(std::move(SYCL_BIN_PATH)) {
    m_InputValidator.reset(new InputValidator());
    HeadTransaction = new Transaction*;
    *HeadTransaction = NULL;
    secureCode = false;
    std::ofstream File;
    File.open(dumpFile, std::ios::in | std::ios::out | std::ios::trunc);
    File.close();

    getSYCLCompileOpt(interp, m_Args, llvmdir);
  }

  IncrementalSYCLDeviceCompiler::~IncrementalSYCLDeviceCompiler() {
    delete HeadTransaction;
    m_InputValidator.reset(0);
    for (auto arg : m_Args) {
      delete[] arg;
    }
    remove(dumpFile.c_str());
    remove(kernelInfoFile.c_str());
    remove(spvFile.c_str());
  }

  std::string
  IncrementalSYCLDeviceCompiler::SyclWrapInput(const std::string& Input,
                                               unsigned int is_statement) {
    std::string Wrapper(Input);
    if (is_statement) {
      std::string Header = std::string("void __cling_custom_sycl_") +
                           std::to_string(m_UniqueCounter) +
                           std::string("() {\n");
      Wrapper.insert(0, Header);
      Wrapper.append("\n;\n}");
    }
    return Wrapper;
  }

  void IncrementalSYCLDeviceCompiler::insertCodeEntry(unsigned int is_statement,
                                                      const std::string& input,
                                                      Transaction* T) {
    EntryList.push_back(DumpCodeEntry(is_statement, input, T));
    UniqueToEntry[IncrementalSYCLDeviceCompiler::m_UniqueCounter] =
        --EntryList.end();
    m_Uniques.push_back(IncrementalSYCLDeviceCompiler::m_UniqueCounter++);
  }

  bool IncrementalSYCLDeviceCompiler::compile(const std::string& input,
                                              Transaction* T,
                                              unsigned int isStatement,
                                              bool declSuccess /* = false*/) {
    // If ExtractDeclFlag = false, return immediately since the code is not
    // entered by user input
    if ((isStatement == 0) && (!ExtractDeclFlag))
      return true;
    std::istringstream input_holder(input);
    std::string line;
    std::string complete_input;
    m_Uniques.clear();
    // When integrated with jupyter notebook , the jupyter server may send a complete cpp file, 
    // need to use InputValidator to split the original cpp into several closed decl and stmt,
    // then wrap them in wrap_function 
    while (getline(input_holder, line)) {
      if (line.empty() || (line.size() == 1 && line.front() == '\n')) {
        continue;
      }
      // Check whether a decl or stmt is complete
      if (m_InputValidator->validate(line) == InputValidator::kIncomplete) {
        continue;
      }
      // Clear cached input if complete
      complete_input.clear();
      m_InputValidator->reset(&complete_input);
      size_t wrapPoint = std::string::npos;
      // Wrap the complete input and dump them
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
      // Extract declarations out of wrapper functions into the global scope
      if (!refactorCode()) {
        return false;
      }
      lastUnique = m_Uniques.back();
    }
    // Call SYCL compiler to generate kernel info and spv file
    return compileImpl();
  }

  void IncrementalSYCLDeviceCompiler::dump(const std::string& target) {
    std::error_code EC;
    llvm::raw_fd_ostream File(target, EC, llvm::sys::fs::F_Text);
    for (auto& CodeEntry : EntryList) {
      File << CodeEntry.code;
      // Find the last valid character and add ';' if necessary
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
    // Dump every code entry to a file and declarations in last several code
    // entries are not extracted to global scope
    dump(dumpFile);

    // Initialize CompilerInstance
    CompilerInstance CI;
    // Create complie options
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

    // Dump every code entry to a file again and declarations in all
    // code entries are extracted to global scope
    dump(dumpFile);
    return true;
  }

  bool IncrementalSYCLDeviceCompiler::compileImpl() {
    setExtractDeclFlag(false);
    secureCode = true;
    std::ofstream File;
    File.open(kernelInfoFile, std::ios::in | std::ios::out | std::ios::trunc);
    File.close();
    std::string command =
        SYCL_BIN_PATH +
        "/clang++ -w -fsycl-device-only  -fno-sycl-use-bitcode "
        "-Xclang -fsycl-int-header=" +
        kernelInfoFile + " -c " + dumpFile + " -o " + spvFile;

    // Add include paths entered by .I command in cling
    for (auto& arg : m_ICommandInclude) {
      command = command + " " + arg;
    }

    // Use SYCL device compiler to generate Kernel info and Device code
    int sysReturn = std::system(command.c_str());
    if (sysReturn != 0) {
      secureCode = false;
      removeCodeByTransaction(NULL);
      return false;
    }

    // Unload the previous kernel info header
    if (HeadTransaction && *HeadTransaction) {
      m_Interpreter->unload(**HeadTransaction);
      *HeadTransaction = NULL;
    }

    std::ifstream headFile;
    headFile.open(kernelInfoFile);
    std::ostringstream tmp;
    tmp << headFile.rdbuf();
    std::string headFileContent = tmp.str();
    headFile.close();

    // Load the new kernel info header
    if (m_Interpreter->declare(headFileContent.c_str(), HeadTransaction) !=
        Interpreter::kSuccess) {
      secureCode = false;
      removeCodeByTransaction(NULL);
      return false;
    }

    secureCode = false;
    return true;
  }

  void IncrementalSYCLDeviceCompiler::setTransaction(Transaction* T) {
    for (auto u : m_Uniques) {
      UniqueToEntry[u]->CurT = T;
    }
  }

  void IncrementalSYCLDeviceCompiler::setDeclSuccess(Transaction* T) {
    if (!secureCode) {
      if (T) {
        setTransaction(T);
      } else {
        // declSuccess is only set when T is NULL. This flag is used to ensure
        // those successful code entries with NULL transactions not to be
        // removed by removeCodeByTransaction()
        for (auto u : m_Uniques) {
          UniqueToEntry[u]->declSuccess = true;
        }
      }
    }
  }

  void IncrementalSYCLDeviceCompiler::removeCodeByTransaction(Transaction* T) {
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

  void IncrementalSYCLDeviceCompiler::addCompileArg(const std::string& arg1,
                                                    const std::string& arg2) {
    char* tmpArg1 = new char[arg1.length() + 1];
    strcpy(tmpArg1, arg1.c_str());
    m_Args.push_back(tmpArg1);
    m_ICommandInclude.push_back(arg1);
    if (arg2.length() > 0) {
      char* tmpArg2 = new char[arg2.length() + 1];
      strcpy(tmpArg2, arg2.c_str());
      m_Args.push_back(tmpArg2);
      m_ICommandInclude.push_back(arg2);
    }
  }

} // namespace cling