#include "Cppdumper.h"

#include "cling/Interpreter/Transaction.h"
#include "cling/Interpreter/Interpreter.h"
#include "cling/MetaProcessor/InputValidator.h"
#include "cling/Utils/SourceNormalization.h"

#include "clang/Frontend/CompilerInstance.h"

namespace cling {
    Dumpcode_entry::Dumpcode_entry(unsigned int dsflag,const std::string& input,Transaction * T, bool declSuc/* = false*/){
        declstmtflag = dsflag;
        code.assign(input);
        CurT = T;
        declSuccess = declSuc;
    }
    Cppdumper::Cppdumper(Interpreter* interp) : m_Interpreter(interp) {
        m_InputValidator.reset(new InputValidator());
        HeadTransaction = new Transaction *; 
        *HeadTransaction = NULL;
        secureCode = false;
        dump_out.open("dump.cpp", std::ios::in | std::ios::out | std::ios::trunc);
        dump_out.close();
    }
    Cppdumper::~Cppdumper(){ 
        delete HeadTransaction;
    }
    bool Cppdumper::set_curt(Transaction* curt){
        CurT = curt;
        return true;
    }
    bool Cppdumper::set_extract_decl_flag(const bool flag){
        extract_decl_flag = flag;
        return true;
    }
    bool Cppdumper::dump(const std::string& input,Transaction* T,unsigned int declstmtflag, bool declSuccess/* = false*/){
        return dump(input,T,declstmtflag,std::string::npos, declSuccess);
    }
    bool Cppdumper::dump(const std::string& input,Transaction* T,unsigned int declstmtflag,size_t wrap_point, bool declSuccess/* = false*/){
        if((declstmtflag == 0)&&(!extract_decl_flag))
            return true;      
        if(declstmtflag == 0){
            myvector.push_back(Dumpcode_entry(declstmtflag,input,T, declSuccess));
            submit();
            return compile(input);
        }
        std::istringstream input_holder(input);
        std::string line; 
        std::string complete_input;
        while(getline(input_holder,line)){
            if (line.empty() || (line.size() == 1 && line.front() == '\n')) {
                continue;
            }
            if (m_InputValidator->validate(line) == InputValidator::kIncomplete) {
                continue;
            }
            complete_input.clear();
            m_InputValidator->reset(&complete_input);
            size_t wrapPoint = std::string::npos;
            wrapPoint = utils::getWrapPoint(complete_input, m_Interpreter->getCI()->getLangOpts());
            if(wrapPoint == std::string::npos)
                myvector.push_back(Dumpcode_entry(0,complete_input,T));
            else if(wrapPoint == 0)
                myvector.push_back(Dumpcode_entry(1,complete_input,T));
            else {
                myvector.push_back(Dumpcode_entry(0,complete_input.substr(0,wrapPoint),T));
                submit();
                myvector.push_back(Dumpcode_entry(1,complete_input.substr(wrapPoint),T));
            }
            submit();
        }
        return compile(input);
    }
    bool Cppdumper::submit(){
        std::string declCode;
        std::string stmtCode;
        for (auto& de : myvector){
            std::string input(de.code);
            if(de.declstmtflag == 0){
                declCode = declCode + input + '\n';
            }
            else{
                stmtCode = stmtCode + input;
                for(auto sit = input.rbegin();sit!=input.rend();sit++){
                    if(*sit!=' '){
                        if(*sit == ';')
                            stmtCode += '\n';
                        else
                            stmtCode += ";\n";
                        break;
                    }
                }
            }
        }
        dump_out.open("dump.cpp", std::ios::in | std::ios::out | std::ios::trunc);
        dump_out.seekp(0,std::ios::beg);
        dump_out<< declCode << "int main(){\n" + stmtCode + "}";
        dump_out.close();
        extract_decl_flag = false;
        return true;
    }

    bool Cppdumper::compile(const std::string& input){
        std::string _hsinput(input);
        //bool head_spv_flg = utils::generate_hppandspv(_hsinput,m_Interpreter->getCI()->getLangOpts());
        secureCode = true;
        if (true) {
            dump_out.open("st.h", std::ios::in | std::ios::out| std::ios::trunc);
            dump_out.close();        
            int sysReturn = std::system("clang++ -Wno-unused-value --sycl -fno-sycl-use-bitcode -Xclang -fsycl-int-header=st.h -c dump.cpp -o mk.spv");
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
            if (m_Interpreter->declare(headFileContent.c_str(), HeadTransaction) != Interpreter::kSuccess) {
                secureCode = false;
                return false;
            } 
        }
        secureCode = false;
        return true;
    }
    void Cppdumper::setTransaction(Transaction* T) {
        for (auto it = myvector.rbegin(); it != myvector.rend(); it++) {
            if (it->CurT != NULL) break;
            else it->CurT = T;
        }
    }
    void Cppdumper::setDeclSuccess(Transaction* T) {
        if (!secureCode) {
            if (T) {
            setTransaction(T);
            }
            else {
                for (auto it = myvector.rbegin(); it != myvector.rend(); it++) {
                    if (!it->CurT) {
                        it->declSuccess = true;
                    }
                }
            }
        }
    }
    void Cppdumper::removeCodeByTransaction(Transaction* T) {
        if (!secureCode) {
            for (auto it = myvector.begin(); it != myvector.end();) {
                if (!it->declSuccess && (it->CurT == NULL || it->CurT == T)) {
                    it = myvector.erase(it);
                }
                else {
                    it++;
                }
            }
        }
    }
}