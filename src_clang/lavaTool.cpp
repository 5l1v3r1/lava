// This makes sure assertions actually occur.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <fstream>

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <set>
#include <memory>
#include <iterator>

extern "C" {
#include <unistd.h>
#include <libgen.h>
}

#include <json/json.h>
#include <odb/pgsql/database.hxx>

#include "clang/AST/AST.h"
#include "clang/Driver/Options.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/ReplacementsYaml.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/ASTMatchers/ASTMatchersMacros.h"
#include "clang/Lex/Lexer.h"
#include "lavaDB.h"
#include "lava.hxx"
#include "lava-odb.hxx"
#include "lexpr.hxx"
#include "vector_set.hxx"

#define MATCHER (1 << 0)
#define INJECT (1 << 1)
#define FNARG (1 << 2)
#define TIM (1 << 3)
#define PRI (1 << 4)
#define DEBUG_FLAGS (TIM | INJECT | FNARG | PRI) // (MATCHER | INJECT | FNARG)

#define ARG_NAME "data_flow"

using namespace odb::core;
std::unique_ptr<odb::pgsql::database> db;

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::driver;
using namespace llvm;

using clang::tooling::CommonOptionsParser;
using clang::tooling::SourceFileCallbacks;
using clang::tooling::Replacement;
using clang::tooling::TranslationUnitReplacements;
using clang::tooling::ClangTool;
using clang::tooling::getAbsolutePath;

using llvm::yaml::MappingTraits;
using llvm::yaml::IO;
using llvm::yaml::Input;

static cl::OptionCategory
    LavaCategory("LAVA Taint Query and Attack Point Tool Options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp(
    "\nTODO: Add descriptive help message.  "
    "Automatic clang stuff is ok for now.\n\n");
enum action { LavaQueries, LavaInjectBugs, LavaInstrumentMain };
static cl::opt<action> LavaAction("action", cl::desc("LAVA Action"),
    cl::values(
        clEnumValN(LavaQueries, "query", "Add taint queries"),
        clEnumValN(LavaInjectBugs, "inject", "Inject bugs"),
        clEnumValEnd),
    cl::cat(LavaCategory),
    cl::Required);
static cl::opt<std::string> LavaBugList("bug-list",
    cl::desc("Comma-separated list of bug ids (from the postgres db) to inject into this file"),
    cl::cat(LavaCategory),
    cl::init("XXX"));

static cl::opt<std::string> LavaWL("lava-wl",
    cl::desc("Path to whitelist of fns to instrument with bugs and data_flow "),
    cl::cat(LavaCategory),
    cl::init("XXX"));


static cl::opt<std::string> LavaDB("lava-db",
    cl::desc("Path to LAVA database (custom binary file for source info).  "
        "Created in query mode."),
    cl::cat(LavaCategory),
    cl::init("XXX"));
static cl::opt<std::string> ProjectFile("project-file",
    cl::desc("Path to project.json file."),
    cl::cat(LavaCategory),
    cl::init("XXX"));

/*
static cl::opt<std::string> FnInstrFile("fninstr-file",
    cl::desc("Path to function instrumenting file."),
    cl::cat(LavaCategory),
    cl::init("XXX"));
*/

static cl::opt<std::string> SourceDir("src-prefix",
    cl::desc("Path to source directory to remove as prefix."),
    cl::cat(LavaCategory),
    cl::init(""));
static cl::opt<std::string> MainFileList("main-files",
    cl::desc("Main files"),
    cl::cat(LavaCategory),
    cl::init(""));
static cl::opt<bool> KnobTrigger("kt",
    cl::desc("Inject in Knob-Trigger style"),
    cl::cat(LavaCategory),
    cl::init(false));
static cl::opt<bool> ArgDataflow("arg_dataflow",
    cl::desc("Use function args for dataflow instead of lava_[sg]et"),
    cl::cat(LavaCategory),
    cl::init(false));
static cl::opt<bool> ArgCompetition("competition",
    cl::desc("Log before/after bugs when competition is #defined"),
    cl::cat(LavaCategory),
    cl::init(false));



std::string LavaPath;

uint32_t num_taint_queries = 0;
uint32_t num_atp_queries = 0;

static llvm::raw_null_ostream null_ostream;
#define debug(flag) ((DEBUG_FLAGS & (flag)) ? llvm::errs() : null_ostream)

Loc::Loc(const FullSourceLoc &full_loc)
    : line(full_loc.getExpansionLineNumber()),
    column(full_loc.getExpansionColumnNumber()) {}

static std::vector<const Bug*> bugs;
static std::set<std::string> main_files;

static std::map<std::string, uint32_t> StringIDs;

// Map of bugs with attack points at a given loc.
std::map<std::pair<LavaASTLoc, AttackPoint::Type>, std::vector<const Bug *>>
    bugs_with_atp_at;

// white list of function names (and filenames) 
// that can be instrumented 
// with dua and atp queries (which will later mean bugs)
// also with data_flow.
std::set<std::string> whitelist;

// returns true iff this fn name is in whitelist to be instrumented
bool fninstr(std::pair<std::string, std::string> fnname) {
    std::string filename = fnname.first;
    std::string function_name = fnname.second;
    if (whitelist.size()>0) {
        if (whitelist.count(function_name) == 0)
            return false;  // dont instrument
        else
            return true;  // instrument
    }
    return false;
}




//std::map<std::string, int> fninstr;


struct LvalBytes {
    const SourceLval *lval;
    Range selected;

    LvalBytes(const SourceLval *lval, Range selected)
        : lval(lval), selected(selected) {}
    LvalBytes(const DuaBytes *dua_bytes)
        : lval(dua_bytes->dua->lval), selected(dua_bytes->selected) {}

    bool operator<(const LvalBytes &other) const {
        return std::tie(lval->id, selected)
            < std::tie(other.lval->id, other.selected);
    }

    friend std::ostream &operator<<(std::ostream &os, const LvalBytes &lval_bytes) {
        os << "LvalBytes " << lval_bytes.selected << " of " << *lval_bytes.lval;
        return os;
    }
};

// Map of bugs with siphon of a given  lval name at a given loc.
std::map<LavaASTLoc, vector_set<LvalBytes>> siphons_at;
std::map<LvalBytes, uint32_t> data_slots;

#define MAX_STRNLEN 64
///////////////// HELPER FUNCTIONS BEGIN ////////////////////
template<typename K, typename V>
const V &map_get_default(const std::map<K, V> &map, K key) {
    static const V default_val;
    auto it = map.find(key);
    if (it != map.end()) {
        return it->second;
    } else {
        return default_val;
    }
}

std::set<std::string> parse_commas_strings(std::string list) {
    std::istringstream ss(list);
    std::set<std::string> result;
    std::string i;
    while(std::getline(ss, i, ',')) {
        result.insert(i);
    }
    return result;
}

template<typename Elem>
std::set<Elem> parse_commas(std::string list) {
    std::istringstream ss(list);
    std::set<Elem> result;
    Elem i;
    while (ss.good()) {
        ss >> i;
        result.insert(i);
        assert(ss.eof() || ss.peek() == ',');
        ss.ignore();
    }
    return result;
}

std::string StripPrefix(std::string filename, std::string prefix) {
    size_t prefix_len = prefix.length();
    if (filename.compare(0, prefix_len, prefix) != 0) {
        printf("Not a prefix!\n");
        assert(false);
    }
    while (filename[prefix_len] == '/') prefix_len++;
    return filename.substr(prefix_len);
}

bool QueriableType(const Type *lval_type) {
    if ((lval_type->isIncompleteType())
        || (lval_type->isIncompleteArrayType())
        || (lval_type->isVoidType())
        || (lval_type->isNullPtrType())
        ) {
        return false;
    }
    if (lval_type->isPointerType()) {
        const Type *pt = lval_type->getPointeeType().getTypePtr();
        return QueriableType(pt);
    }
    return true;
}

// an arg is attackable if it is a pointer 
// and so on.
bool IsArgAttackable(const Expr *arg) {
    debug(MATCHER) << "IsArgAttackable \n";
    if (DEBUG_FLAGS & MATCHER) arg->dump();

    const Type *t = arg->IgnoreParenImpCasts()->getType().getTypePtr();
    if (dyn_cast<OpaqueValueExpr>(arg) || t->isStructureType() || t->isEnumeralType() || t->isIncompleteType()) {
        return false;
    }
    if (QueriableType(t)) {
        if (t->isPointerType()) {
            const Type *pt = t->getPointeeType().getTypePtr();
            // its a pointer to a non-void
            if ( ! (pt->isVoidType() ) ) {
                return true;
            }
        }
        if ((t->isIntegerType() || t->isCharType()) && (!t->isEnumeralType())) {
            return true;
        }
    }
    return false;
}

///////////////// HELPER FUNCTIONS END ////////////////////

uint32_t Slot(LvalBytes lval_bytes) {
    return data_slots.at(lval_bytes);
}

LExpr Get(LvalBytes x) {
    return (ArgDataflow ? DataFlowGet(Slot(x)) : LavaGet(Slot(x)));
}
LExpr Get(const Bug *bug) { return Get(bug->trigger); }

LExpr Set(LvalBytes x) {
    return (ArgDataflow ? DataFlowSet : LavaSet)(x.lval, x.selected, Slot(x));
}
LExpr Set(const Bug *bug) { return Set(bug->trigger); }

LExpr Test(const Bug *bug) {
    return MagicTest<Get>(bug);
}

LExpr traditionalAttack(const Bug *bug) {
    return Get(bug) * Test(bug);
}

LExpr knobTriggerAttack(const Bug *bug) {
    LExpr lava_get_lower = Get(bug) & LHex(0x0000ffff);
    //LExpr lava_get_upper = (LavaGet(bug) >> LDecimal(16)) & LHex(0xffff);
    LExpr lava_get_upper = (Get(bug) & LHex(0xffff0000)) >> LDecimal(16);
    // this is the magic value that will trigger the bug
    // we already know that magic_kt returns uint16_t so we don't have
    // to mask it
    uint16_t magic_value = bug->magic_kt();

    return (lava_get_lower * MagicTest<uint16_t>(magic_value, lava_get_upper))
        + (lava_get_upper * MagicTest<uint16_t>(magic_value, lava_get_lower));
}

/*
 * Keeps track of a list of insertions and makes sure conflicts are resolved.
 */
class Insertions {
private:
    // TODO: use map and "beforeness" concept to robustly avoid duplicate
    // insertions.
    std::map<SourceLocation, std::list<std::string>> impl;

public:
    void clear() { impl.clear(); }

    void InsertAfter(SourceLocation loc, std::string str) {
        if (!str.empty()) {
            std::list<std::string> &strs = impl[loc];
            if (strs.empty() || strs.back() != str || str == ")") {
                impl[loc].push_back(str);
            }
        }
    }

    void InsertBefore(SourceLocation loc, std::string str) {
        if (!str.empty()) {
            std::list<std::string> &strs = impl[loc];
            if (strs.empty() || strs.front() != str || str == "(") {
                impl[loc].push_front(str);
            }
        }
    }

    void render(const SourceManager &sm, std::vector<Replacement> &out) {
        out.reserve(impl.size() + out.size());
        for (const auto &keyvalue : impl) {
            std::stringstream ss;
            for (const std::string &s : keyvalue.second) ss << s;
            out.emplace_back(sm, keyvalue.first, 0, ss.str());
        }
    }
};

/*
 * Contains all the machinery necessary to insert and tries to create some
 * high-level constructs around insertion.
 * Fluent interface to make usage easier. Use Modifier::Change to point at a
 * specific clang expression and the insertion methods to make changes there.
 */
class Modifier {
private:
    const Stmt *stmt = nullptr;

public:
    Insertions &Insert;
    const LangOptions *LangOpts = nullptr;
    const SourceManager *sm = nullptr;

    Modifier(Insertions &Insert) : Insert(Insert) {}

    void Reset(const LangOptions *LangOpts_, const SourceManager *sm_) {
        LangOpts = LangOpts_;
        sm = sm_;
    }

    std::pair<SourceLocation, SourceLocation> range() const {
        auto startRange = sm->getExpansionRange(stmt->getLocStart());
        auto endRange = sm->getExpansionRange(stmt->getLocEnd());
        return std::make_pair(startRange.first, endRange.second);
    }

    SourceLocation before() const {
        return range().first;
    }

    SourceLocation after() const {
        // clang stores ranges as start of first token -> start of last token.
        // so to get character range for replacement, we need to add start of
        // last token.
        SourceLocation end = range().second;
        unsigned lastTokenSize = Lexer::MeasureTokenLength(end, *sm, *LangOpts);
        return end.getLocWithOffset(lastTokenSize);
    }

    const Modifier &InsertBefore(std::string str) const {
        Insert.InsertBefore(before(), str);
        return *this;
    }

    const Modifier &InsertAfter(std::string str) const {
        Insert.InsertAfter(after(), str);
        return *this;
    }

    const Modifier &Change(const Stmt *stmt_) {
        stmt = stmt_;
        return *this;
    }

    const Modifier &Parenthesize() const {
        return InsertBefore("(").InsertAfter(")");
    }

    const Modifier &Operate(std::string op, const LExpr &addend, const Stmt *parent) const {
        InsertAfter(" " + op + " " + addend.render());
        if (parent && !isa<ArraySubscriptExpr>(parent)
                && !isa<ParenExpr>(parent)) {
            Parenthesize();
        }
        return *this;
    }

    const Modifier &Add(const LExpr &addend, const Stmt *parent) const {
        // If inner stmt has lower precedence than addition, add parens.
        const BinaryOperator *binop = dyn_cast<BinaryOperator>(stmt);
        if (isa<AbstractConditionalOperator>(stmt)
                || (binop && !binop->isMultiplicativeOp()
                    && !binop->isAdditiveOp())) {
            Parenthesize();
        }
        return Operate("+", addend, parent);
    }

    void InsertAt(SourceLocation loc, std::string str) {
        Insert.InsertBefore(loc, str);
    }
    
};

/*******************************
 * Matcher Handlers
 *******************************/
struct LavaMatchHandler : public MatchFinder::MatchCallback {
    LavaMatchHandler(Modifier &Mod) : Mod(Mod) {}

    std::string ExprStr(const Stmt *e) {
        clang::PrintingPolicy Policy(*LangOpts);
        std::string TypeS;
        llvm::raw_string_ostream s(TypeS);
        e->printPretty(s, 0, Policy);
        return s.str();
    }

    LavaASTLoc GetASTLoc(const SourceManager &sm, const Stmt *s) {
        assert(!SourceDir.empty());
        FullSourceLoc fullLocStart(sm.getExpansionLoc(s->getLocStart()), sm);
        FullSourceLoc fullLocEnd(sm.getExpansionLoc(s->getLocEnd()), sm);
        std::string src_filename = StripPrefix(
                getAbsolutePath(sm.getFilename(fullLocStart)), SourceDir);
        return LavaASTLoc(src_filename, fullLocStart, fullLocEnd);
    }

    // A query inserted at a possible attack point. Used, dynamically, just to 
    // tell us when an input gets to the attack point.  
    LExpr LavaAtpQuery(LavaASTLoc ast_loc, AttackPoint::Type atpType) {
        return LBlock({
                LFunc("vm_lava_attack_point2",
                    { LDecimal(GetStringID(StringIDs, ast_loc)), LDecimal(0),
                        LDecimal(atpType) }),
                LDecimal(0) });
    }

    /*
      An attack expression.  That is, this is where we would *like* to
      attack something.  Currently used by FunctionArgHandler and
      MemoryAccessHandler.  So, for 
    */ 
    void AttackExpression(const SourceManager &sm, const Expr *toAttack,
            const Expr *parent, const Expr *rhs, AttackPoint::Type atpType) {
        LavaASTLoc ast_loc = GetASTLoc(sm, toAttack);
        std::vector<LExpr> pointerAddends;
        std::vector<LExpr> valueAddends;
        int this_bug_id = 0;

        debug(INJECT) << "Inserting expression attack (AttackExpression).\n";
        const Bug *this_bug = NULL;

        if (LavaAction == LavaInjectBugs) {
            const std::vector<const Bug*> &injectable_bugs =
                map_get_default(bugs_with_atp_at,
                        std::make_pair(ast_loc, atpType));

            if (injectable_bugs.size() == 0 && ArgCompetition) return;

            // this should be a function bug -> LExpr to add.
            auto pointerAttack = KnobTrigger ? knobTriggerAttack : traditionalAttack;
            for (const Bug *bug : injectable_bugs) {
                assert(bug->atp->type == atpType);
                if (ArgCompetition) {
                    assert(this_bug_id == 0);  // You can't inject multiple bugs into the same line for a competition
                    this_bug_id = bug->id;
                    this_bug = bug;
                }

                if (bug->type == Bug::PTR_ADD) {
                    pointerAddends.push_back(pointerAttack(bug));
                } else if (bug->type == Bug::REL_WRITE) {
                    const DuaBytes *where = db->load<DuaBytes>(bug->extra_duas[0]);
                    const DuaBytes *what = db->load<DuaBytes>(bug->extra_duas[1]);
                    pointerAddends.push_back(Test(bug) * Get(where));
                    valueAddends.push_back(Test(bug) * Get(what));
                }
            }
            bugs_with_atp_at.erase(std::make_pair(ast_loc, atpType));
        } else if (LavaAction == LavaQueries) {
            debug(TIM) << "LavaQueries I'm in this handle thing\n";
            
            // call attack point hypercall and return 0
            pointerAddends.push_back(LavaAtpQuery(ast_loc, atpType));
            num_atp_queries++;
        }


        if (!pointerAddends.empty()) {
            LExpr addToPointer = LBinop("+", std::move(pointerAddends));
            Mod.Change(toAttack).Add(addToPointer, parent);

            // For competitions, wrap pointer value in LAVALOG macro call-
            // it's effectively just a NOP that prints a message when the trigger is true
            // so we can identify when bugs are potentially triggered
            if (ArgCompetition) {
                if (this_bug == NULL) return; // Maybe redundant
                Mod.Change(toAttack).InsertBefore("LAVALOG(");

                std::stringstream end_str;
                end_str << ", " << Test(this_bug) << "," << this_bug_id << ")";
                Mod.Change(toAttack).InsertAfter(end_str.str());
            }
        }

        if (!valueAddends.empty()) {
            assert(rhs);
            LExpr addToValue = LBinop("+", std::move(valueAddends));
            Mod.Change(rhs).Add(addToValue, nullptr);
        }
    }

    virtual void handle(const MatchFinder::MatchResult &Result) = 0;
    virtual ~LavaMatchHandler() = default;

    virtual void run(const MatchFinder::MatchResult &Result) {
        const SourceManager &sm = *Result.SourceManager;
        auto nodesMap = Result.Nodes.getMap();

        debug(MATCHER) << "====== Found Match =====\n";
        for (auto &keyValue : nodesMap) {
            const Stmt *stmt = keyValue.second.get<Stmt>();
            if (stmt) {
                SourceLocation start = stmt->getLocStart();
                if (!sm.getFilename(start).empty() && sm.isInMainFile(start)
                        && !sm.isMacroArgExpansion(start)) {
                    debug(MATCHER) << keyValue.first << ": " << ExprStr(stmt) << " ";
                    stmt->getLocStart().print(debug(MATCHER), sm);
                    debug(MATCHER) << "\n";
                    if (DEBUG_FLAGS & MATCHER) stmt->dump();
                } else return;
            }
        }
        handle(Result);
    }

    const LangOptions *LangOpts = nullptr;

protected:
    Modifier &Mod;
};




std::pair<std::string,std::string> fundecl_fun_name(const MatchFinder::MatchResult &Result, const FunctionDecl *fd) {
    IdentifierInfo *II = fd->getIdentifier();
    if (II) {
        StringRef Name = II->getName();
        std::string funname = Name.str();
        std::string filename = Result.SourceManager->getFilename(fd->getLocation()).str();
        return std::make_pair(filename, funname);
    }
    return std::make_pair(std::string("Meh"),std::string("Unknown"));
}

std::pair<std::string,std::string> get_containing_function_name(const MatchFinder::MatchResult &Result, const Stmt &stmt) {

    const Stmt *pstmt = &stmt;

    std::pair<std::string,std::string> fail = std::make_pair(std::string("Notinafunction"), std::string("Notinafunction"));        
    while (true) {
        const auto &parents = Result.Context->getParents(*pstmt);
        std::cout << "get_containing_function_name: " << parents.size() << " parents\n";
        for (auto &parent : parents) {
            std::cout << "parent: " << parent.getNodeKind().asStringRef().str() << "\n";
        }
        if (parents.empty()) {
            std::cout << "get_containing_function_name: no parents for stmt? ";
            pstmt->dumpPretty(*Result.Context);
            std::cout << "\n";            
            return fail;       
        }     
        if (parents[0].get<TranslationUnitDecl>()) {
            std::cout << "get_containing_function_name: parents[0].get<TranslationUnitDecl? ";
            pstmt->dumpPretty(*Result.Context);
            std::cout << "\n";                        
            return fail;
        }
        const FunctionDecl *fd = parents[0].get<FunctionDecl>();
        if (fd) return fundecl_fun_name(Result, fd);
        pstmt = parents[0].get<Stmt>();        
        if (!pstmt) {
            std::cout << "get_containing_function_name: !pstmt \n";
            const VarDecl *pvd = parents[0].get<VarDecl>();
            if (pvd) {
                const auto &parents = Result.Context->getParents(*pvd);
                pstmt = parents[0].get<Stmt>();
            }
            if (!pstmt)
                return fail;
        }    
    }
    
}        

/*
  This code is used both to inject 'queries' used during taint analysis but
  also to inject bug parts (mostly DUA siphoning (first half of bug) but also
  stack pivot).  

  First use is to instrument code with vm_lava_pri_query_point calls.
  These get inserted in between stmts in a compound statement.  

  Thus, if code was

  stmt; stmt; stmt

  Then this handler will change it to

  query; stmt; query; stmt; query; stmt; query

  The idea is these act as sentinels in the source.  We know exactly
  where they are, semantically, since we inserted them.  Then, we run
  the program augmented with these under PANDA and record.  Then when
  we replay, under taint analysis.  The calls to
  vm_lava_pri_query_point talk to the PANDA 'hypervisor' to tell it
  exactly where we are in the program at each point in the trace.  At
  each of these query points, PANDA uses PRI (program introspection
  using debug dwarf info) to know what are the local variables, what
  are they named, and where are they in memory or registers.  PANDA
  queries these in-scope items for taint and anything found to be
  tainted is logged along with taint-compute number and other info to
  the pandalog.  The pandalog is consumed by the
  find_bugs_injectable.cpp program to identify DUAs (which
  additionally have liveness constraints).  

  When lavaTool.cpp is used during bug injection, we insert DUA
  'siphoning' code in exactly the same place as the corresponding
  vm_lava_pri_query_points.  We also can add stack-pivot style 
  exploitable bugs, using these locations as attack points.  

*/

struct PriQueryPointHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    // create code that siphons dua bytes into a global
    // for dua x, offset o, generates:
    // lava_set(slot, *(const unsigned int *)(((const unsigned char *)x)+o)
    // Each lval gets an if clause containing one siphon
    std::string SiphonsForLocation(LavaASTLoc ast_loc) {
        std::stringstream result_ss;
        for (const LvalBytes &lval_bytes : map_get_default(siphons_at, ast_loc)) {
            result_ss << LIf(lval_bytes.lval->ast_name, Set(lval_bytes));
        }

        std::string result = result_ss.str();
        if (!result.empty()) {
            debug(PRI) << " Injecting dua siphon at " << ast_loc << "\n";
            debug(PRI) << "    Text: " << result << "\n";
        }
        siphons_at.erase(ast_loc); // Only inject once.
        return result;
    }

    std::string AttackRetBuffer(LavaASTLoc ast_loc) {
        std::stringstream result_ss;
        auto key = std::make_pair(ast_loc, AttackPoint::QUERY_POINT);
        for (const Bug *bug : map_get_default(bugs_with_atp_at, key)) {
            if (bug->type == Bug::RET_BUFFER) {
                const DuaBytes *buffer = db->load<DuaBytes>(bug->extra_duas[0]);
                if (ArgCompetition) {
                    result_ss << LIf(Test(bug).render(), {
                            LBlock({
                                    //It's always safe to call lavalog here since we're in the if
                                LFunc("LAVALOG", {LDecimal(1), LDecimal(1), LDecimal(bug->id)}), 
                                LIfDef("__x86_64__", {
                                    LAsm({ UCharCast(LStr(buffer->dua->lval->ast_name)) +
                                        LDecimal(buffer->selected.low), },
                                        { "movq %0, %%rsp", "ret" }),
                                    LAsm({ UCharCast(LStr(buffer->dua->lval->ast_name)) +
                                        LDecimal(buffer->selected.low), },
                                        { "movl %0, %%esp", "ret" })})})});
                } else{
                    result_ss << LIf(Test(bug).render(), {
                                LIfDef("__x86_64__", {
                                    LAsm({ UCharCast(LStr(buffer->dua->lval->ast_name)) +
                                        LDecimal(buffer->selected.low), },
                                        { "movq %0, %%rsp", "ret" }),
                                    LAsm({ UCharCast(LStr(buffer->dua->lval->ast_name)) +
                                        LDecimal(buffer->selected.low), },
                                        { "movl %0, %%esp", "ret" })})});
                }
            }
        }
        bugs_with_atp_at.erase(key); // Only inject once.
        return result_ss.str();
    }

    virtual void handle(const MatchFinder::MatchResult &Result) override {
        const Stmt *toSiphon = Result.Nodes.getNodeAs<Stmt>("stmt");
        const SourceManager &sm = *Result.SourceManager;

        auto fnname = get_containing_function_name(Result, *toSiphon);

        // only instrument this stmt 
        // if it's in the body of a function that is on our whitelist
        if (fninstr(fnname)) {
            debug(PRI) << "PriQueryPointHandler: Containing function is in whitelist " << fnname.second << " : " << fnname.first << "\n";
        }
        else {
            debug(PRI) << "PriQueryPointHandler: Containing function is NOT in whitelist " << fnname.second << " : " << fnname.first << "\n";
            return;
        }

        printf ("PriQueryPointHandler handle: ok to instrument %s\n", fnname.second.c_str());

        LavaASTLoc ast_loc = GetASTLoc(sm, toSiphon);
        debug(PRI) << "Have a query point @ " << ast_loc << "!\n";

        std::string before;
        if (LavaAction == LavaQueries) {
            // this is used in first pass clang tool, adding queries
            // to be intercepted by panda to query taint on in-scope variables
            before = "; " + LFunc("vm_lava_pri_query_point", {
               LDecimal(GetStringID(StringIDs, ast_loc)),
                LDecimal(ast_loc.begin.line),
                LDecimal(0)}).render() + "; ";

            num_taint_queries += 1;
        } else if (LavaAction == LavaInjectBugs) {
            // This is used in second pass clang tool, injecting bugs.
            // This part is just about inserting DUA siphon, the first half of the bug.
            // Well, not quite.  We are also considering all such code / trace
            // locations as potential inject points for attack point that is
            // stack-pivot-then-return.  Ugh.
            before = SiphonsForLocation(ast_loc) + AttackRetBuffer(ast_loc);
        }
        Mod.Change(toSiphon).InsertBefore(before);
    }
};





    
        
/*
  This matcher handles arguments to function calls that are 'attackable', which is basically
  pointers or integers to which would could add something.    
*/
     
struct FunctionArgHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) override {
        // this is the argument we might attack
        const Expr *toAttack = Result.Nodes.getNodeAs<Expr>("arg");
        // and this is the fn call
        const CallExpr *call = Result.Nodes.getNodeAs<CallExpr>("call");
        assert (call != nullptr);
        const Decl *func1 = call->getCalleeDecl();
        assert (func1 != nullptr);
        const NamedDecl *nd = dyn_cast<NamedDecl> (func1);
        assert (nd != nullptr);
        std::string calleename = nd->getNameAsString();
        debug(FNARG) << "Callee name is [" << calleename << "]\n";

        const SourceManager &sm = *Result.SourceManager;

        auto sl1 = call->getLocStart();
        auto sl2 = call->getLocEnd();
        debug(FNARG) << "start: " << sl1.printToString(sm) << "\n"; 
        debug(FNARG) << "end:   " << sl2.printToString(sm) << "\n"; 


        printf ("I'm in FunctionArgHandler\n");

        auto fnname = get_containing_function_name(Result, *toAttack);

        // only instrument this function arg 
        // if it's in the body of a function that is on our whitelist
        if (fninstr(fnname)) {
            debug(FNARG) << "FunctionArgHandler: Containing function is in whitelist " << fnname.second << " : " << fnname.first << "\n";
        }
        else {
            debug(FNARG) << "FunctionArgHandler: Containing function is NOT in whitelist " << fnname.second << " : " << fnname.first << "\n";
            return;
        }

/*
        // and if this is a call to a function that is something like "__builtin_..." we dont instr
        // only instrument calls to functions that are themselves on our whitelist. 
        assert (call != nullptr);
        assert (func != nullptr);
        fnname = fundecl_fun_name(Result, func);
        std::string filename = fnname.first;
        std::string functionname = fnname.second;

        if (functionname.find("__builtin_") != std::string::npos) {
*/
        if (calleename.find("__builtin_") != std::string::npos) {
            debug(TIM) << "OMG we found a built-in being called: " << calleename << "\n";
            return;
        }        

        printf ("FunctionArgHandler handle: ok to instrument %s\n", fnname.second.c_str());

        debug(TIM) <<"FunctionArgHandler @ " << GetASTLoc(sm, toAttack) << "\n";
        debug(INJECT) << "FunctionArgHandler @ " << GetASTLoc(sm, toAttack) << "\n";

/*
//        auto fnname = get_containing_function_name(Result, *toAttack);
        std::string filename = fnname.first;
        std::string functionname = fnname.second;
        if (functionname == "Notinafunction") return;

        debug(TIM) << "filename=[" << filename << "] functionname=[" << functionname << "]\n";

        if (functionname.find("__builtin_") != std::string::npos) {
            debug(TIM) << "OMG we found a builtin " << filename << " : " << functionname << "\n";
            return;
        }
*/        
        AttackExpression(sm, toAttack, nullptr, nullptr, AttackPoint::FUNCTION_ARG);
    }
};


struct ReadDisclosureHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) {
        const SourceManager &sm = *Result.SourceManager;
        const CallExpr *callExpr = Result.Nodes.getNodeAs<CallExpr>("call_expression");

        auto fnname = get_containing_function_name(Result, *callExpr);

        // only instrument this printf with a read disclosure 
        // if it's in the body of a function that is on our whitelist
        if (fninstr(fnname)) {
            debug(INJECT) << "ReadDisclosureHandler: Containing function is in whitelist " << fnname.second << " : " << fnname.first << "\n";
        }
        else {
            debug(INJECT) << "ReadDisclosureHandler: Containing function is NOT in whitelist " << fnname.second << " : " << fnname.first << "\n";
            return;
        }

        printf ("ReadDisclosureHandler handle: ok to instrument %s\n", fnname.second.c_str());

        LExpr addend = LDecimal(0);
        // iterate through all the arguments in the call expression
        for (auto it = callExpr->arg_begin(); it != callExpr->arg_end(); ++it) {
            const Expr *arg = dyn_cast<Expr>(*it);
            if (arg) {
                if (arg->IgnoreImpCasts()->isLValue() && arg->getType()->isIntegerType()) {
                    LavaASTLoc ast_loc = GetASTLoc(sm, arg);
                    Mod.Change(arg);
                    if (LavaAction == LavaQueries)  {
                        addend = LavaAtpQuery(GetASTLoc(sm, arg),
                                AttackPoint::PRINTF_LEAK);
                        Mod.Add(addend, nullptr);
                    } else if (LavaAction == LavaInjectBugs) {
                        const std::vector<const Bug*> &injectable_bugs =
                            map_get_default(bugs_with_atp_at,
                                    std::make_pair(ast_loc, AttackPoint::PRINTF_LEAK));
                        for (const Bug *bug : injectable_bugs) {
                            Mod.Parenthesize()
                                .InsertBefore(Test(bug).render() +
                                        " ? &(" + ExprStr(arg) + ") : ");
                        }
                    }

                }
            }
        }
    }
};


/*
  This handler is for AST items of the form
  LHS = RHS 
  where LHS is a write to array element or via pointer.
  i.e. x[i] = ... or *p = ...
  Actually, to be precise, "lhs" binds to the 'i' or 'p'
  in the above example.

  This matcher is used to insert the 2nd half of a bug.
  That is, the use of one or more DUAs to change the array
  index of pointer value to cause a write out of bounds.

*/
struct MemoryAccessHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) override {
        const Expr *toAttack = Result.Nodes.getNodeAs<Expr>("innerExpr");
        const Expr *parent = Result.Nodes.getNodeAs<Expr>("lhs");

        auto fnname = get_containing_function_name(Result, *toAttack);
        if (fninstr(fnname)) {
            debug(INJECT) << "MemoryAccessHandler: Containing function is in whitelist " << fnname.second << " : " << fnname.first << "\n";
        }
        else {
            debug(INJECT) << "MemoryAccessHandler: Containing function is NOT in whitelist " << fnname.second << " : " << fnname.first << "\n";
            return;
        }

        printf ("MemoryAccessHandler: ok to instrument %s\n", fnname.second.c_str());

        const SourceManager &sm = *Result.SourceManager;
        LavaASTLoc ast_loc = GetASTLoc(sm, toAttack);
        debug(INJECT) << "PointerAtpHandler @ " << ast_loc << "\n";

        const Expr *rhs = nullptr;
        AttackPoint::Type atpType = AttackPoint::POINTER_READ;

        // memwrite style attack points will have rhs bound to a node
        auto it = Result.Nodes.getMap().find("rhs");
        if (it != Result.Nodes.getMap().end()){
            atpType = AttackPoint::POINTER_WRITE;
            rhs = it->second.get<Expr>();
            assert(rhs);
        }

        AttackExpression(sm, toAttack, parent, rhs, atpType);
    }
};



// Add data_flow arg to fn definitions and prototypes

struct FuncDeclArgAdditionHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor

    void AddArg(const FunctionDecl *func) {
        SourceLocation l1 = func->getLocStart();
        SourceLocation l2 = func->getLocEnd();
        debug(FNARG) << "func->getLocStart = " << Mod.sm->getFileOffset(l1) << "\n";
        debug(FNARG) << "func->getLocEnd = " << Mod.sm->getFileOffset(l2) << "\n";


        // first '(' in func decl
        SourceLocation loc1 = clang::Lexer::findLocationAfterToken(
                func->getLocation(), tok::l_paren, *Mod.sm, *Mod.LangOpts, true);

        if (func->getNumParams() == 0) {
            Mod.InsertAt(loc1, "int *" ARG_NAME );
/*
            // first ')' in func decl
            SourceLocation loc2 = clang::Lexer::findLocationAfterToken(
                loc1, tok::r_paren, *Mod.sm, *Mod.LangOpts, true);
            unsigned o1 = Mod.sm->getFileOffset(loc1);
            unsigned o2 = Mod.sm->getFileOffset(loc2);
            if (o1+1 == o2 || o1+2 == o2) {
                // () or ( )
                Mod.InsertAt(loc1, "int *" ARG_NAME);
            }
            else {
                if  (!(o1 < o2)) {
                    debug(FNARG) << "file offsets o1 = " << o1 << " o2 = " << o2 << "\n";
                    assert (o1<o2);
                }
                // (void) or something
                Mod.InsertAt(loc1, "int *" ARG_NAME "/ *");
                // this should be the ')'
                SourceLocation loc3 = loc2.getLocWithOffset(-1);
                Mod.InsertAt(loc3, "* /");
            }
*/
        } else {
          Mod.InsertAt(loc1, "int *" ARG_NAME ", ");
        }

    }

    virtual void handle(const MatchFinder::MatchResult &Result) {

        const FunctionDecl *func =
            Result.Nodes.getNodeAs<FunctionDecl>("funcDecl");

        
        debug(TIM) << "FuncDeclArgAdditionHandler: Maybe adding arg to " << func->getNameAsString() << "\n";


        auto fnname = fundecl_fun_name(Result, func);

        if (!func->hasBody()) {
            debug(TIM) << "Has no body: " << fnname.first << " : " << fnname.second << "\n";
//            func->dumpPretty(*Result.Context);
            if (fnname.first == "Meh") {
                // Function pointer? 
                SourceRange sr = func->getSourceRange();
                debug(TIM) << "Meh: function pointer decl";
                debug(TIM)<< "start: " << sr.getBegin().printToString(*Mod.sm) << "\n"; 
                debug(TIM) << "end:   " <<  sr.getEnd().printToString(*Mod.sm) << "\n"; 
            }
        }

        // only instrument if function being decl / def is in whitelist
        if (fninstr(fnname)) {
            debug(FNARG) << "FuncDeclArgAdditionHandler: Function def/decl is in whitelist     " << fnname.second << " : " << fnname.first << "\n";
        }
        else {
            debug(FNARG) << "FuncDeclArgAdditionHandler: Function def/decl is NOT in whitelist " << fnname.second << " : " << fnname.first << "\n";
            return;
        }
    
        if (fnname.second.find("__builtin") != std::string::npos) {
            debug(FNARG) << "FuncDeclArgAdditionHandler: Function def/decl is builtin" << func->getNameAsString() << "\n";        
            return;
        }

        printf ("FuncDeclArgAdditionHandler handle: ok to instrument %s\n", fnname.second.c_str());
        debug(FNARG) << "adding arg to " << func->getNameAsString() << "\n";
        
        if (func->isThisDeclarationADefinition()) debug(FNARG) << "has body\n";
        if (func->getBody()) debug(FNARG) << "can find body\n";

        if (func->getLocation().isInvalid()) return;
        if (func->getNameAsString().find("lava") == 0) return;
        if (Mod.sm->isInSystemHeader(func->getLocation())) return;
        if (Mod.sm->getFilename(func->getLocation()).empty()) return;

        debug(FNARG) << "actually adding arg\n";

        if (func->isMain()) {
            if (func->isThisDeclarationADefinition()) { // no prototype for main.
                CompoundStmt *body = dyn_cast<CompoundStmt>(func->getBody());
                assert(body);
                Stmt *first = *body->body_begin();
                assert(first);
                std::stringstream data_array;
                data_array << "int data[" << data_slots.size() << "] = {0};\n";
                data_array << "int *" ARG_NAME << "= &data;\n";
                Mod.InsertAt(first->getLocStart(), data_array.str());
            }
        } else {
            const FunctionDecl *bodyDecl = nullptr;
            func->hasBody(bodyDecl);
            if (bodyDecl) AddArg(bodyDecl);
            while (func != NULL) {
                AddArg(func);
                func = func->getPreviousDecl();
                if (func) debug(FNARG) << "found a redeclaration\n";
            }
        }
        return;
    }
};


// adding data_flow arg to 
struct FunctionPointerFieldHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) {
        // What the hell is this malarkey?
        // Assume this is dead code until you encounter it
        // triggering in the real world
        assert (1==0);
        const FieldDecl *decl = Result.Nodes.getNodeAs<FieldDecl>("fieldDecl");
        debug(FNARG) << decl->getLocEnd().printToString(*Mod.sm) << "\n";
        Mod.InsertAt(decl->getLocEnd().getLocWithOffset(-14), "int *" ARG_NAME ", ");
    }
};




//  Add data_flow arg to call expression

struct CallExprArgAdditionHandler : public LavaMatchHandler {
    using LavaMatchHandler::LavaMatchHandler; // Inherit constructor.

    virtual void handle(const MatchFinder::MatchResult &Result) {
        const CallExpr *call = Result.Nodes.getNodeAs<CallExpr>("callExpr");
        SourceLocation loc = clang::Lexer::findLocationAfterToken(
                call->getLocStart(), tok::l_paren, *Mod.sm, *Mod.LangOpts, true);

        auto fnname = get_containing_function_name(Result, *call);
        // only instrument call if its in the body of a function that is on our whitelist
        if (fninstr(fnname)) {
            debug(FNARG) << "CallExprArgAdditionHandler: Containing function is in whitelist " << fnname.second << " : " << fnname.first << "\n";
        }
        else {
            debug(FNARG) << "CallExprArgAdditionHandler: Containing function is NOT in whitelist " << fnname.second << " : " << fnname.first << "\n";
            return;
        }

        // and if this is a call that is in the body of a function on our whitelist,  
        // only instrument calls to functions that are themselves on our whitelist. 
        const FunctionDecl *func = call->getDirectCallee();
        if (func) {
            fnname = fundecl_fun_name(Result, func);
            if (fninstr(fnname)) {
                debug(FNARG) << "CallExprArgAdditionHandler: Called function is in whitelist " << fnname.second << " : " << fnname.first << "\n";
            }
            else {
                debug(FNARG) << "CallExprArgAdditionHandler: Called function is NOT in whitelist " << fnname.second << " : " << fnname.first << "\n";
                return;
            }
        }


        // If we get here, we are instrumenting a call to a function on our whitelist that is in 
        // the body of a function also on our whitelist. 

        if (func == nullptr || func->getLocation().isInvalid()) {
            // Function Pointer
            debug(FNARG) << "FUNCTION POINTER USE: ";
            call->getLocStart().print(debug(FNARG), *Mod.sm);
            debug(FNARG) << "this many args: " << call->getNumArgs() << "\n";
            loc = call->getArg(0)->getLocStart();
        } else if (Mod.sm->isInSystemHeader(func->getLocation())) {
            return;
        }

        loc.print(debug(FNARG), *Mod.sm);

        if (call->getNumArgs() == 0) {
            Mod.InsertAt(loc, ARG_NAME);
        } else {
            Mod.InsertAt(loc, ARG_NAME ", ");
        }
    }
};

namespace clang {
    namespace ast_matchers {
        AST_MATCHER(Expr, isAttackableMatcher){
            const Expr *ce = &Node;
            return IsArgAttackable(ce);
        }

        AST_MATCHER(VarDecl, isStaticLocalDeclMatcher){
            const VarDecl *vd = &Node;
            return vd->isStaticLocal();
        }

        AST_MATCHER_P(CallExpr, forEachArgMatcher,
                internal::Matcher<Expr>, InnerMatcher) {
            BoundNodesTreeBuilder Result;
            bool Matched = false;
            for ( const auto *I : Node.arguments()) {
                //for (const auto *I : Node.inits()) {
                BoundNodesTreeBuilder InitBuilder(*Builder);
                if (InnerMatcher.matches(*I, Finder, &InitBuilder)) {
                    Matched = true;
                    Result.addMatch(InitBuilder);
                }
            }
            *Builder = std::move(Result);
            return Matched;
        }
    }
}

class LavaMatchFinder : public MatchFinder, public SourceFileCallbacks {
public:
    LavaMatchFinder() : Mod(Insert) {

        // This is a write to array element or pointer
        // i.e. we have *p = ... or x[i] = ...        
        // Really the 'p' or 'i' is what gets matched
        // This is a potential attack point.
        StatementMatcher memoryAccessMatcher =
            allOf(
                expr(anyOf(
                         // "lhs" part matches i in x[i] or p in *p 
                    arraySubscriptExpr(
                        hasIndex(ignoringImpCasts(
                                expr().bind("innerExpr")))),
                    unaryOperator(hasOperatorName("*"),
                        hasUnaryOperand(ignoringImpCasts(
                                expr().bind("innerExpr")))))).bind("lhs"),
                anyOf(
                    // and this means above "lhs" in tree is assignment
                    // where LHS matches our already bound "lhs"
                    // in which case RHS binds to "rhs"
                    expr(hasAncestor(binaryOperator(allOf(
                                    hasOperatorName("="),
                                    hasRHS(ignoringImpCasts(
                                            expr().bind("rhs"))),
                                    hasLHS(ignoringImpCasts(expr(
                                                equalsBoundNode("lhs")))))))),
                    anything()), // this is a "maybe" construction.
                hasAncestor(functionDecl()), // makes sure that we are't in a global variable declaration
                // make sure we aren't in static local variable initializer which must be constant
                unless(hasAncestor(varDecl(isStaticLocalDeclMatcher()))));

        addMatcher(memoryAccessMatcher, makeHandler<MemoryAccessHandler>());

        // This matches every stmt in a compound statement
        // So "stmt" in 
        // stmt; stmt'; stmt''; stmt'''; etc
        // Used to add pri queries (in turn used by PANDA to know where it is
        // in the source whe querying taint).  Also used to insert DUA siphons
        // (first half of a bug) but also stack-pivot second-half of bug.
        addMatcher(
                stmt(hasParent(compoundStmt())).bind("stmt"),
                makeHandler<PriQueryPointHandler>()
                );

        addMatcher(
                callExpr(
                    forEachArgMatcher(expr(isAttackableMatcher()).bind("arg"))).bind("call"),
                makeHandler<FunctionArgHandler>()
                );

            

        // fortenforge's matchers (for data_flow argument addition)
        if (ArgDataflow && LavaAction == LavaInjectBugs) {
            // function declarations & definition.  Decl without body is prototype
            addMatcher(
                    functionDecl().bind("funcDecl"),
                    makeHandler<FuncDeclArgAdditionHandler>());
            // function calls (direct or via fn pointer)
            addMatcher(
                    callExpr().bind("callExpr"),
                    makeHandler<CallExprArgAdditionHandler>());
            // OMG what is this nonsense?
            addMatcher(
                    fieldDecl(anyOf(hasName("as_number"), hasName("as_string"))).bind("fieldDecl"),
                    makeHandler<FunctionPointerFieldHandler>());
        }

        addMatcher(
                callExpr(
                    callee(functionDecl(hasName("::printf"))),
                    unless(argumentCountIs(1))).bind("call_expression"),
                makeHandler<ReadDisclosureHandler>()
                );
    }

    virtual bool handleBeginSource(CompilerInstance &CI, StringRef Filename) override {
        Insert.clear();
        Mod.Reset(&CI.getLangOpts(), &CI.getSourceManager());
        TUReplace.Replacements.clear();
        TUReplace.MainSourceFile = Filename;
        CurrentCI = &CI;

        debug(INJECT) << "*** handleBeginSource for: " << Filename << "\n";

        std::stringstream competition;
        competition << "#include <stdio.h>\n"
                    << "#ifdef LAVA_LOGGING\n"
                    << "#define LAVALOG(x, trigger, bugid)  ({trigger && printf(\"\\nLAVALOG: %d: %s:%d\\n\", bugid, __FILE__, __LINE__) && fflush(stdout), (x);})\n"
                    << "#else\n"
                    << "#define LAVALOG(x,y,z)  (x)\n"
                    << "#endif\n";

        std::string insert_at_top;
        if (LavaAction == LavaQueries) {
            insert_at_top = "#include \"pirate_mark_lava.h\"\n";
        } else if (LavaAction == LavaInjectBugs) {
            if (ArgCompetition) {
                insert_at_top.append(competition.str());
            }
            if (!ArgDataflow) {
                if (main_files.count(getAbsolutePath(Filename)) > 0) {
                    std::stringstream top;
                    top << "static unsigned int lava_val[" << data_slots.size() << "] = {0};\n"
                        << "void lava_set(unsigned int, unsigned int);\n"
                        << "__attribute__((visibility(\"default\")))\n"
                        << "void lava_set(unsigned int slot, unsigned int val) { lava_val[slot] = val; }\n"
                        << "unsigned int lava_get(unsigned int);\n"
                        << "__attribute__((visibility(\"default\")))\n"
                        << "unsigned int lava_get(unsigned int slot) { return lava_val[slot]; }\n";
                    insert_at_top.append(top.str());
                } else {
                    insert_at_top.append("void lava_set(unsigned int bn, unsigned int val);\n"
                    "extern unsigned int lava_get(unsigned int);\n");
                }
            }
        }

        debug(INJECT) << "Inserting at top of file: \n" << insert_at_top;
        TUReplace.Replacements.emplace_back(Filename, 0, 0, insert_at_top);

        for (auto it = MatchHandlers.begin();
                it != MatchHandlers.end(); it++) {
            (*it)->LangOpts = &CI.getLangOpts();
        }

        return true;
    }

    virtual void handleEndSource() override {
        debug(INJECT) << "*** handleEndSource\n";

        Insert.render(CurrentCI->getSourceManager(), TUReplace.Replacements);
        std::error_code EC;
        llvm::raw_fd_ostream YamlFile(TUReplace.MainSourceFile + ".yaml",
                EC, llvm::sys::fs::F_RW);
        yaml::Output Yaml(YamlFile);
        Yaml << TUReplace;
    }

    template<class Handler>
    LavaMatchHandler *makeHandler() {
        MatchHandlers.emplace_back(new Handler(Mod));
        return MatchHandlers.back().get();
    }

private:
    Insertions Insert;
    Modifier Mod;
    TranslationUnitReplacements TUReplace;
    std::vector<std::unique_ptr<LavaMatchHandler>> MatchHandlers;
    CompilerInstance *CurrentCI = nullptr;
};

void mark_for_siphon(const DuaBytes *dua_bytes) {
    LvalBytes lval_bytes(dua_bytes);
    siphons_at[lval_bytes.lval->loc].insert(lval_bytes);

    // if insert fails do nothing. we already have a slot for this one.
    data_slots.insert(std::make_pair(lval_bytes, data_slots.size()));
}


void parse_whitelist(std::string whitelist_filename) {
    printf ("parsing white list\n");
    FILE *fp = fopen(whitelist_filename.c_str(), "r");
    char *line = NULL;
    size_t len = 0;
    ssize_t read = 0;
    while ((read = getline(&line, &len, fp)) != -1) {
        char *p = line;
        char *np = strtok(p, " ");
        char *npp = strtok(NULL, "\n");
        auto wlp = std::make_pair(std::string(np), std::string(npp));
        whitelist.insert(std::string(npp));
        printf ("white list entry: file = [%s] func = [%s]\n", np, npp);
        
    }
    printf ("whitelist is %d entries\n", whitelist.size());
}



            

int main(int argc, const char **argv) {
    CommonOptionsParser op(argc, argv, LavaCategory);
    ClangTool Tool(op.getCompilations(), op.getSourcePathList());

    LavaPath = std::string(dirname(dirname(dirname(realpath(argv[0], NULL)))));

    std::ifstream json_file(ProjectFile);
    Json::Value root;
    if (ProjectFile == "XXX") {
        if (LavaAction == LavaInjectBugs) {
            errs() << "Error: Specify a json file with \"-project-file\".  Exiting . . .\n";
            exit(1);
        }
    } else {
        json_file >> root;
    }

    if (LavaDB != "XXX") StringIDs = LoadDB(LavaDB);

    odb::transaction *t = nullptr;

    if (LavaWL != "XXX") 
        parse_whitelist(LavaWL);
    else 
        printf ("No whitelist\n");

    if (LavaAction == LavaInjectBugs) {
        db.reset(new odb::pgsql::database("postgres", "postgrespostgres",
                    root["db"].asString()));
        t = new odb::transaction(db->begin());

        main_files = parse_commas_strings(MainFileList);

        // get bug info for the injections we are supposed to be doing.
        debug(INJECT) << "LavaBugList: [" << LavaBugList << "]\n";

        std::set<uint32_t> bug_ids = parse_commas<uint32_t>(LavaBugList);
        // for each bug_id, load that bug from DB and insert into bugs vector.
        std::transform(bug_ids.begin(), bug_ids.end(), std::back_inserter(bugs),
                [&](uint32_t bug_id) { return db->load<Bug>(bug_id); });

        for (const Bug *bug : bugs) {
            LavaASTLoc atp_loc = bug->atp->loc;
            auto key = std::make_pair(atp_loc, bug->atp->type);
            bugs_with_atp_at[key].push_back(bug);

            mark_for_siphon(bug->trigger);

            if (bug->type != Bug::RET_BUFFER) {
                for (uint64_t dua_id : bug->extra_duas) {
                    const DuaBytes *dua_bytes = db->load<DuaBytes>(dua_id);
                    mark_for_siphon(dua_bytes);
                }
            }
        }
    }

    debug(INJECT) << "about to call Tool.run \n";
    LavaMatchFinder Matcher;
    Tool.run(newFrontendActionFactory(&Matcher, &Matcher).get());
    debug(INJECT) << "back from calling Tool.run \n";

    if (LavaAction == LavaQueries) {
        std::cout << "num taint queries added " << num_taint_queries << "\n";
        std::cout << "num atp queries added " << num_atp_queries << "\n";

        if (LavaDB != "XXX") SaveDB(StringIDs, LavaDB);
    } else if (LavaAction == LavaInjectBugs) {
        if (!bugs_with_atp_at.empty()) {
            std::cout << "Warning: Failed to inject attacks for bugs:\n";
            for (const auto &keyvalue : bugs_with_atp_at) {
                std::cout << "    At " << keyvalue.first.first << "\n";
                for (const Bug *bug : keyvalue.second) {
                    std::cout << "        " << *bug << "\n";
                }
            }
        }
        if (!siphons_at.empty()) {
            std::cout << "Warning: Failed to inject siphons:\n";
            for (const auto &keyvalue : siphons_at) {
                std::cout << "    At " << keyvalue.first << "\n";
                for (const LvalBytes &lval_bytes : keyvalue.second) {
                    std::cout << "        " << lval_bytes << "\n";
                }
            }
        }
    }

    if (t) {
        t->commit();
        delete t;
    }

    return 0;
}
