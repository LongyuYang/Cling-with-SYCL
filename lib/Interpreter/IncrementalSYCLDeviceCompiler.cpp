#include "IncrementalSYCLDeviceCompiler.h"
#include "ASTTransformer.h"
#include "AutoSynthesizer.h"
#include "BackendPasses.h"
#include "CheckEmptyTransactionTransformer.h"
#include "ClingPragmas.h"
#include "DeclCollector.h"
#include "DeclExtractor.h"
#include "DefinitionShadower.h"
#include "DynamicLookup.h"
#include "IncrementalExecutor.h"
#include "NullDerefProtectionTransformer.h"
#include "TransactionPool.h"
#include "ValueExtractionSynthesizer.h"
#include "ValuePrinterSynthesizer.h"
#include "cling/Interpreter/CIFactory.h"
#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/MetaProcessor/InputValidator.h"
#include "cling/Utils/AST.h"
#include "cling/Utils/Output.h"
#include "cling/Utils/Platform.h"
#include "cling/Utils/SourceNormalization.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/Tooling.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaDiagnostic.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ASTWriter.h"
#include "llvm/Support/Path.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace clang;
namespace cling {
DumpCodeEntry::DumpCodeEntry(unsigned int isStatement, const std::string &input,
                             Transaction *T, bool declSuccess /* = false*/)
    : isStatement(isStatement), code(input), CurT(T), declSuccess(declSuccess) {
}

IncrementalSYCLDeviceCompiler::IncrementalSYCLDeviceCompiler(
    Interpreter *interp)
    : m_Interpreter(interp) {
  m_InputValidator.reset(new InputValidator());
  m_Ctran.reset(new Cpptransformer());
  HeadTransaction = new Transaction *;
  *HeadTransaction = NULL;
  secureCode = false;
  DumpOut.open("dump.cpp", std::ios::in | std::ios::out | std::ios::trunc);
  DumpOut.close();
}
IncrementalSYCLDeviceCompiler::~IncrementalSYCLDeviceCompiler() {
  delete HeadTransaction;
  m_InputValidator.reset(0);
  m_Ctran.reset(0);
  if (ClearFlag) {
    remove("dump.cpp");
    remove("st.h");
    remove("mk.spv");
  }
}

void IncrementalSYCLDeviceCompiler::setExtractDeclFlag(const bool flag) {
  ExtractDeclFlag = flag;
}

void IncrementalSYCLDeviceCompiler::setClearFlag(const bool flag) {
  ClearFlag = flag;
}

bool IncrementalSYCLDeviceCompiler::dump(const std::string &input,
                                         Transaction *T,
                                         unsigned int isStatement,
                                         size_t wrap_point,
                                         bool declSuccess /* = false*/) {
  if ((isStatement == 0) && (!ExtractDeclFlag))
    return true;
  std::istringstream input_holder(input);
  std::string line;
  std::string complete_input;
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
    if (wrapPoint == std::string::npos)
      EntryList.push_back(DumpCodeEntry(0, complete_input, T));
    else if (wrapPoint == 0) {
      int wheretodump = utils::getSyclWrapPoint(
          complete_input, m_Interpreter->getCI()->getLangOpts());
      EntryList.push_back(DumpCodeEntry(wheretodump, complete_input, T));
    } else {
      EntryList.push_back(
          DumpCodeEntry(0, complete_input.substr(0, wrapPoint), T));
      submit();
      std::string wrappedinput(complete_input.substr(wrapPoint));
      int wheretodump = utils::getSyclWrapPoint(
          wrappedinput, m_Interpreter->getCI()->getLangOpts());
      EntryList.push_back(DumpCodeEntry(wheretodump, wrappedinput, T));
    }
    submit();
  }
  return compile(input);
}

void IncrementalSYCLDeviceCompiler::submit() {
  std::string declCode;
  std::string stmtCode;
  for (auto &CodeEntry : EntryList) {
    std::string input(CodeEntry.code);
    if (CodeEntry.isStatement == 0) {
      declCode = declCode + input;
    } else {
      stmtCode = stmtCode + input;
    }
    for (auto sit = input.rbegin(); sit != input.rend(); sit++) {
      if (*sit != ' ') {
        if (CodeEntry.isStatement == 0) {
          if (*sit == '}')
            declCode += ";\n";
          else
            declCode += "\n";
          break;
        } else {
          if (*sit == ';')
            stmtCode += '\n';
          else
            stmtCode += ";\n";
          break;
        }
      }
    }
  }
  DumpOut.open("dump.cpp", std::ios::in | std::ios::out | std::ios::trunc);
  DumpOut.seekp(0, std::ios::beg);
  DumpOut << declCode << "int main(){\n" + stmtCode + "}";
  DumpOut.close();
  ExtractDeclFlag = false;
}

bool IncrementalSYCLDeviceCompiler::compile(const std::string &input) {
  std::string _hsinput(input);
  // bool head_spv_flg =
  // utils::generate_hppandspv(_hsinput,m_Interpreter->getCI()->getLangOpts());
  secureCode = true;
  if (true) {
    DumpOut.open("st.h", std::ios::in | std::ios::out | std::ios::trunc);
    DumpOut.close();
    int sysReturn =
        std::system("clang++ -w --sycl -fno-sycl-use-bitcode "
                    "-Xclang -fsycl-int-header=st.h -c dump.cpp -o mk.spv");
    if (sysReturn != 0) {
      secureCode = false;
      removeCodeByTransaction(NULL);
      return false;
    }
    if (HeadTransaction && HeadTransaction[0]) {
      m_Interpreter->unload(HeadTransaction[0][0]);
    }
    std::ifstream headFile;
    headFile.open("st.h");
    std::ostringstream tmp;
    tmp << headFile.rdbuf();
    std::string headFileContent = tmp.str();
    headFile.close();
    if (m_Interpreter->declare(headFileContent.c_str(), HeadTransaction) !=
        Interpreter::kSuccess) {
      secureCode = false;
      return false;
    }
  }
  secureCode = false;
  return true;
}

void IncrementalSYCLDeviceCompiler::setTransaction(Transaction *T) {
  for (auto it = EntryList.rbegin(); it != EntryList.rend(); it++) {
    if (it->CurT)
      break;
    else
      it->CurT = T;
  }
}

void IncrementalSYCLDeviceCompiler::setDeclSuccess(Transaction *T) {
  if (!secureCode) {
    if (T) {
      setTransaction(T);
    } else {
      for (auto it = EntryList.rbegin(); it != EntryList.rend(); it++) {
        if (!it->CurT) {
          it->declSuccess = true;
        }
      }
    }
  }
}

void IncrementalSYCLDeviceCompiler::removeCodeByTransaction(Transaction *T) {
  if (!secureCode) {
    for (auto it = EntryList.begin(); it != EntryList.end();) {
      if (!it->declSuccess && (it->CurT == NULL || it->CurT == T)) {
        it = EntryList.erase(it);
      } else {
        it++;
      }
    }
  }
}

class InterpreterConsumer : public ASTConsumer {
  std::string &DumpText;
  const ASTContext &m_context;
  int m_counter = 0;

public:
  explicit InterpreterConsumer(const ASTContext &context, std::string &S)
      : DumpText(S), m_context(context) {}
  virtual ~InterpreterConsumer() {}
  bool HandleTopLevelDecl(clang::DeclGroupRef DGR) {
    // printf("%x\n",m_Sema);
    // DeclExtractor * m_DE = new DeclExtractor(m_Sema);
    printf("yes\n");
    llvm::raw_string_ostream dump(DumpText);
    for (auto it = DGR.begin(); it != DGR.end(); it++) {
      Decl *D = *it;
      if (D->isFunctionOrFunctionTemplate()) {
        FunctionDecl *FD = cast<FunctionDecl>(D);
        if (FD->getNameAsString().compare("main") != 0)
          continue;
        printf("%d\n", utils::Analyze::IsWrapper(FD));
        CompoundStmt *CS = dyn_cast<CompoundStmt>(FD->getBody());
        for (CompoundStmt::body_iterator I = CS->body_begin(),
                                         EI = CS->body_end();
             I != EI; ++I) {
          DeclStmt *DS = dyn_cast<DeclStmt>(*I);
          if (!DS) {
            printf("get stmt!!!\n");
            DumpText.append(std::string("void __cling_wrapper__costom__") +
                            std::to_string(++m_counter) + std::string("(){\n"));
            (*I)->printPretty(dump, NULL, PrintingPolicy(LangOptions()));
            dump.flush();
            DumpText.append(";\n}\n");
            dump.flush();
            continue;
          }
          for (DeclStmt::decl_iterator J = DS->decl_begin();
               J != DS->decl_end(); ++J) {
            printf("get decl!!!!\n");
            (*J)->print(dump);
            dump.flush();
            DumpText.append(";\n");
            dump.flush();
          }
        }
      }
      dump.flush();
      return true;
    }
  }
};

class InterpreterClassAction : public ASTFrontendAction {
  std::string &DumpText;

public:
  explicit InterpreterClassAction(std::string &S) : DumpText(S) {}
  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
    InterpreterConsumer *consumer =
        new InterpreterConsumer(Compiler.getASTContext(), DumpText);
    return std::unique_ptr<clang::ASTConsumer>(consumer);
  }
};

Cpptransformer::Cpptransformer() {
  
  CompilerInstance compiler;
  compiler.createDiagnostics();
  assert(compiler.hasDiagnostics());
  const char *args[] = {"-std=c++14", "a.cpp"};

  clang::CompilerInvocation::CreateFromArgs(
      compiler.getInvocation(), args, args + 2, compiler.getDiagnostics());
  assert(0 == compiler.getDiagnostics().getErrorsAsFatal());
  std::string tmp;
  FrontendAction *action = new InterpreterClassAction(tmp);
  if (compiler.ExecuteAction(*action)) {
    std::cout << "ok:";
  } else {
    std::cout << "error:";
  }
  printf("modified:\n%s\n",tmp.c_str());
  
}

Cpptransformer::~Cpptransformer() {}

} // namespace cling