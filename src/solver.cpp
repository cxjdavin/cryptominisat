/******************************************
Copyright (c) 2016, Mate Soos

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
***********************************************/

#include "solver.h"

#include <fstream>
#include <cmath>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <string>
#include <algorithm>
#include <vector>
#include <complex>
#include <locale>

#include "varreplacer.h"
#include "time_mem.h"
#include "searcher.h"
#include "occsimplifier.h"
#include "prober.h"
#include "distillerallwithall.h"
#include "clausecleaner.h"
#include "solutionextender.h"
#include "varupdatehelper.h"
#include "completedetachreattacher.h"
#include "compfinder.h"
#include "comphandler.h"
#include "subsumestrengthen.h"
#include "watchalgos.h"
#include "clauseallocator.h"
#include "subsumeimplicit.h"
#include "distillerlongwithimpl.h"
#include "str_impl_w_impl_stamp.h"
#include "datasync.h"
#include "reducedb.h"
#include "clausedumper.h"
#include "sccfinder.h"
#include "intree.h"
#include "features_calc.h"
#include "GitSHA1.h"
#include "features_to_reconf.h"
#include "trim.h"
#include "streambuffer.h"
#include "gaussian.h"
#include "matrixfinder.h"
#include "sqlstats.h"

using namespace CMSat;
using std::cout;
using std::endl;

namespace CMSat {
    class TooManyVarsError {};
    class TooLongClauseError {};
}

#ifdef USE_SQLITE3
#include "sqlitestats.h"
#endif

//#define DRAT_DEBUG

//#define DEBUG_RENUMBER

//#define DEBUG_IMPLICIT_PAIRS_TRIPLETS

Solver::Solver(const SolverConf *_conf, std::atomic<bool>* _must_interrupt_inter) :
    Searcher(_conf, this, _must_interrupt_inter)
{
    sqlStats = NULL;

    if (conf.doProbe) {
        prober = new Prober(this);
    }
    intree = new InTree(this);
    if (conf.perform_occur_based_simp) {
        occsimplifier = new OccSimplifier(this);
    }
    distill_all_with_all = new DistillerAllWithAll(this);
    dist_long_with_impl = new DistillerLongWithImpl(this);
    dist_impl_with_impl = new StrImplWImplStamp(this);
    clauseCleaner = new ClauseCleaner(this);
    varReplacer = new VarReplacer(this);
    if (conf.doCompHandler) {
        compHandler = new CompHandler(this);
    }
    if (conf.doStrSubImplicit) {
        subsumeImplicit = new SubsumeImplicit(this);
    }
    datasync = new DataSync(this, NULL);
    Searcher::solver = this;
    reduceDB = new ReduceDB(this);

    set_up_sql_writer();
    next_lev1_reduce = conf.every_lev1_reduce;
    next_lev2_reduce =  conf.every_lev2_reduce;
}

Solver::~Solver()
{
    delete compHandler;
    delete sqlStats;
    delete prober;
    delete intree;
    delete occsimplifier;
    delete distill_all_with_all;
    delete dist_long_with_impl;
    delete dist_impl_with_impl;
    delete clauseCleaner;
    delete varReplacer;
    delete subsumeImplicit;
    delete datasync;
    delete reduceDB;
}

void Solver::set_sqlite(string
    #ifdef USE_SQLITE3
    filename
    #endif
) {
    #ifdef USE_SQLITE3
    sqlStats = new SQLiteStats(filename);
    if (!sqlStats->setup(this)) {
        exit(-1);
    }
    if (conf.verbosity >= 4) {
        cout << "c Connected to SQLite server" << endl;
    }
    #else
    std::cerr << "SQLite support was not compiled in, cannot use it. Exiting."
    << endl;
    std::exit(-1);
    #endif
}

void Solver::set_shared_data(SharedData* shared_data)
{
    delete datasync;
    datasync = new DataSync(this, shared_data);
}

bool Solver::add_xor_clause_inter(
    const vector<Lit>& lits
    , bool rhs
    , const bool attach
    , bool addDrat
) {
    assert(ok);
    assert(!attach || qhead == trail.size());
    assert(decisionLevel() == 0);

    vector<Lit> ps(lits);
    for(Lit& lit: ps) {
        if (lit.sign()) {
            rhs ^= true;
            lit ^= true;
        }
    }
    std::sort(ps.begin(), ps.end());
    Lit p;
    uint32_t i, j;
    for (i = j = 0, p = lit_Undef; i != ps.size(); i++) {
        assert(ps[i].sign() == false);

        if (ps[i].var() == p.var()) {
            //added, but easily removed
            j--;
            p = lit_Undef;

            //Flip rhs if neccessary
            if (value(ps[i]) != l_Undef) {
                rhs ^= value(ps[i]) == l_True;
            }

        } else if (value(ps[i]) == l_Undef) {
            //Add and remember as last one to have been added
            ps[j++] = p = ps[i];

            assert(varData[p.var()].removed != Removed::elimed);
        } else {
            //modify rhs instead of adding
            rhs ^= value(ps[i]) == l_True;
        }
    }
    ps.resize(ps.size() - (i - j));

    if (ps.size() >= (0x01UL << 28)) {
        throw CMSat::TooLongClauseError();
    }
    //cout << "Cleaned ps is: " << ps << endl;

    if (!ps.empty()) {
        if (ps.size() > 2) {
            xorclauses.push_back(Xor(ps, rhs));
        }
        ps[0] ^= rhs;
    } else {
        if (rhs) {
            *drat << fin;
            ok = false;
        }
        return ok;
    }

    //cout << "without rhs is: " << ps << endl;
    add_every_combination_xor(ps, attach, addDrat);

    return ok;
}

void Solver::add_every_combination_xor(
    const vector<Lit>& lits
    , const bool attach
    , const bool addDrat
) {
    //cout << "add_every_combination got: " << lits << endl;

    size_t at = 0;
    size_t num = 0;
    vector<Lit> xorlits;
    Lit lastlit_added = lit_Undef;
    while(at != lits.size()) {
        xorlits.clear();
        size_t last_at = at;
        for(; at < last_at+2 && at < lits.size(); at++) {
            xorlits.push_back(lits[at]);
        }

        //Connect to old cut
        if (lastlit_added != lit_Undef) {
            xorlits.push_back(lastlit_added);
        } else if (at < lits.size()) {
            xorlits.push_back(lits[at]);
            at++;
        }

        if (at + 1 == lits.size()) {
            xorlits.push_back(lits[at]);
            at++;
        }

        //New lit to connect to next cut
        if (at != lits.size()) {
            new_var(true);
            const uint32_t newvar = nVars()-1;
            const Lit toadd = Lit(newvar, false);
            xorlits.push_back(toadd);
            lastlit_added = toadd;
        }

        add_xor_clause_inter_cleaned_cut(xorlits, attach, addDrat);
        if (!ok)
            break;

        num++;
    }
}

void Solver::add_xor_clause_inter_cleaned_cut(
    const vector<Lit>& lits
    , const bool attach
    , const bool addDrat
) {
    //cout << "xor_inter_cleaned_cut got: " << lits << endl;
    vector<Lit> new_lits;
    for(size_t i = 0; i < (1ULL<<lits.size()); i++) {
        unsigned bits_set = num_bits_set(i, lits.size());
        if (bits_set % 2 == 0) {
            continue;
        }

        new_lits.clear();
        for(size_t at = 0; at < lits.size(); at++) {
            bool xorwith = (i >> at)&1;
            new_lits.push_back(lits[at] ^ xorwith);
        }
        //cout << "Added. " << new_lits << endl;
        Clause* cl = add_clause_int(new_lits, false, ClauseStats(), attach, NULL, addDrat);
        if (cl) {
            longIrredCls.push_back(cl_alloc.get_offset(cl));
        }

        if (!ok)
            return;
    }
}

unsigned Solver::num_bits_set(const size_t x, const unsigned max_size) const
{
    unsigned bits_set = 0;
    for(size_t i = 0; i < max_size; i++) {
        if ((x>>i)&1) {
            bits_set++;
        }
    }

    return bits_set;
}


bool Solver::sort_and_clean_clause(
    vector<Lit>& ps
    , const vector<Lit>& origCl
    , const bool red
) {
    std::sort(ps.begin(), ps.end());
    Lit p = lit_Undef;
    uint32_t i, j;
    for (i = j = 0; i != ps.size(); i++) {
        if (value(ps[i]) == l_True) {
            return false;
        } else if (ps[i] == ~p) {
            if (!red) {
                uint32_t var = p.var();
                var = map_inter_to_outer(var);
                if (undef_must_set_vars.size() < var+1) {
                    undef_must_set_vars.resize(var+1, false);
                }
                undef_must_set_vars[var] = true;
            }
            return false;
        } else if (value(ps[i]) != l_False && ps[i] != p) {
            ps[j++] = p = ps[i];

            if (varData[p.var()].removed != Removed::none) {
                cout << "ERROR: clause " << origCl << " contains literal "
                << p << " whose variable has been removed (removal type: "
                << removed_type_to_string(varData[p.var()].removed)
                << " var-updated lit: "
                << varReplacer->get_var_replaced_with(p)
                << ")"
                << endl;
            }

            //Variables that have been eliminated cannot be added internally
            //as part of a clause. That's a bug
            assert(varData[p.var()].removed == Removed::none);
        }
    }
    ps.resize(ps.size() - (i - j));
    return true;
}

/**
@brief Adds a clause to the problem. Should ONLY be called internally

This code is very specific in that it must NOT be called with varibles in
"ps" that have been replaced, eliminated, etc. Also, it must not be called
when the wer are in an UNSAT (!ok) state, for example. Use it carefully,
and only internally
*/
Clause* Solver::add_clause_int(
    const vector<Lit>& lits
    , const bool red
    , ClauseStats stats
    , const bool attach_long
    , vector<Lit>* finalLits
    , bool addDrat
    , const Lit drat_first
) {
    assert(ok);
    assert(decisionLevel() == 0);
    assert(!attach_long || qhead == trail.size());
    #ifdef VERBOSE_DEBUG
    cout << "add_clause_int clause " << lits << endl;
    #endif //VERBOSE_DEBUG

    //Make stats sane
    #ifdef STATS_NEEDED
    stats.introduced_at_conflict = std::min<uint64_t>(Searcher::sumConflicts, stats.introduced_at_conflict);
    #endif

    vector<Lit> ps = lits;
    if (!sort_and_clean_clause(ps, lits, red)) {
        return NULL;
    }

    #ifdef VERBOSE_DEBUG
    cout << "add_clause_int final clause " << ps << endl;
    #endif

    //If caller required final set of lits, return it.
    if (finalLits) {
        *finalLits = ps;
    }

    if (addDrat) {
        size_t i = 0;
        if (drat_first != lit_Undef) {
            for(i = 0; i < ps.size(); i++) {
                if (ps[i] == drat_first) {
                    break;
                }
            }
        }
        std::swap(ps[0], ps[i]);
        *drat << ps << fin;
        std::swap(ps[0], ps[i]);

        if (ps.size() == 2) {
            datasync->signalNewBinClause(ps);
        }
    }

    //Handle special cases
    switch (ps.size()) {
        case 0:
            ok = false;
            if (conf.verbosity >= 6) {
                cout
                << "c solver received clause through addClause(): "
                << lits
                << " that became an empty clause at toplevel --> UNSAT"
                << endl;
            }
            return NULL;
        case 1:
            enqueue(ps[0]);
            #ifdef STATS_NEEDED
            propStats.propsUnit++;
            #endif
            if (attach_long) {
                ok = (propagate<true>().isNULL());
            }

            return NULL;
        case 2:
            attach_bin_clause(ps[0], ps[1], red);
            return NULL;

        default:
            Clause* c = cl_alloc.Clause_new(ps
            , sumConflicts
            #ifdef STATS_NEEDED
            , 1
            #endif
            );
            if (red) {
                c->makeRed(stats.glue);
            }
            c->stats = stats;

            //In class 'OccSimplifier' we don't need to attach normall
            if (attach_long) {
                attachClause(*c);
            } else {
                if (red)
                    litStats.redLits += ps.size();
                else
                    litStats.irredLits += ps.size();
            }

            return c;
    }
}

void Solver::attachClause(
    const Clause& cl
    , const bool checkAttach
) {
    #if defined(DRAT_DEBUG) && defined(DRAT)
    if (drat) {
        for(size_t i = 0; i < cl.size(); i++) {
            *drat << cl[i];
        }
        *drat << fin;
    }
    #endif

    //Update stats
    if (cl.red())
        litStats.redLits += cl.size();
    else
        litStats.irredLits += cl.size();

    //Call Solver's function for heavy-lifting
    PropEngine::attachClause(cl, checkAttach);
}

void Solver::attach_bin_clause(
    const Lit lit1
    , const Lit lit2
    , const bool red
    , const bool checkUnassignedFirst
) {
    #if defined(DRAT_DEBUG)
    *drat << lit1 << lit2 << fin;
    #endif

    //Update stats
    if (red) {
        binTri.redBins++;
    } else {
        binTri.irredBins++;
    }
    binTri.numNewBinsSinceSCC++;

    //Call Solver's function for heavy-lifting
    PropEngine::attach_bin_clause(lit1, lit2, red, checkUnassignedFirst);
}

void Solver::detachClause(const Clause& cl, const bool removeDrat)
{
    if (removeDrat) {
        *drat << del << cl << fin;
    }

    assert(cl.size() > 2);
    detach_modified_clause(cl[0], cl[1], cl.size(), &cl);
}

void Solver::detachClause(const ClOffset offset, const bool removeDrat)
{
    Clause* cl = cl_alloc.ptr(offset);
    detachClause(*cl, removeDrat);
}

void Solver::detach_modified_clause(
    const Lit lit1
    , const Lit lit2
    , const uint32_t origSize
    , const Clause* address
) {
    //Update stats
    if (address->red())
        litStats.redLits -= origSize;
    else
        litStats.irredLits -= origSize;

    //Call heavy-lifter
    PropEngine::detach_modified_clause(lit1, lit2, address);
}

bool Solver::addClauseHelper(vector<Lit>& ps)
{
    //If already UNSAT, just return
    if (!ok)
        return false;

    //Sanity checks
    assert(decisionLevel() == 0);
    assert(qhead == trail.size());

    //Check for too long clauses
    if (ps.size() > (0x01UL << 28)) {
        cout << "Too long clause!" << endl;
        throw CMSat::TooLongClauseError();
    }

    //Check for too large variable number
    for (Lit& lit: ps) {
        if (lit.var() >= nVarsOuter()) {
            std::cerr
            << "ERROR: Variable " << lit.var() + 1
            << " inserted, but max var is "
            << nVarsOuter()
            << endl;
            assert(false);
            std::exit(-1);
        }
        assert(lit.var() < nVarsOuter()
        && "Clause inserted, but variable inside has not been declared with new_var() !");

        //Undo var replacement
        const Lit updated_lit = varReplacer->get_lit_replaced_with_outer(lit);
        if (conf.verbosity >= 12
            && lit != updated_lit
        ) {
            cout
            << "EqLit updating outer lit " << lit
            << " to outer lit " << updated_lit
            << endl;
        }
        lit = updated_lit;

        //Map outer to inter, and add re-variable if need be
        if (map_outer_to_inter(lit).var() >= nVars()) {
            new_var(false, lit.var());
        }
    }

    renumber_outer_to_inter_lits(ps);

    #ifdef SLOW_DEBUG
    //Check renumberer
    for (const Lit lit: ps) {
        const Lit updated_lit = varReplacer->get_lit_replaced_with(lit);
        assert(lit == updated_lit);
    }
    #endif

     //Undo comp handler
    if (compHandler) {
        bool readd = false;
        for (Lit lit: ps) {
            if (varData[lit.var()].removed == Removed::decomposed) {
                readd = true;
                break;
            }
        }

        if (readd) {
            compHandler->readdRemovedClauses();
        }
    }

    //Uneliminate vars
    for (const Lit lit: ps) {
        if (conf.perform_occur_based_simp
            && varData[lit.var()].removed == Removed::elimed
        ) {
            #ifdef VERBOSE_DEBUG_RECONSTRUCT
            cout << "Uneliminating var " << lit.var() + 1 << endl;
            #endif
            if (!occsimplifier->uneliminate(lit.var()))
                return false;
        }
    }

    #ifdef SLOW_DEBUG
    //Check
    for (Lit& lit: ps) {
        const Lit updated_lit = varReplacer->get_lit_replaced_with(lit);
        assert(lit == updated_lit);
    }
    #endif

    return true;
}

bool Solver::addClause(const vector<Lit>& lits, bool red)
{
    if (conf.perform_occur_based_simp && occsimplifier->getAnythingHasBeenBlocked()) {
        std::cerr
        << "ERROR: Cannot add new clauses to the system if blocking was"
        << " enabled. Turn it off from conf.doBlockClauses"
        << endl;
        std::exit(-1);
    }

    #ifdef VERBOSE_DEBUG
    cout << "Adding clause " << lits << endl;
    #endif //VERBOSE_DEBUG
    const size_t origTrailSize = trail.size();

    vector<Lit> ps = lits;

    if (!addClauseHelper(ps)) {
        return false;
    }

    finalCl_tmp.clear();
    std::sort(ps.begin(), ps.end());
    Clause* cl = add_clause_int(
        ps
        , red
        , ClauseStats() //default stats
        , true //yes, attach
        , &finalCl_tmp
        , false //add drat?
    );

    //Drat -- We manipulated the clause, delete
    if (drat->enabled()
        && ps != finalCl_tmp
    ) {
        //Dump only if non-empty (UNSAT handled later)
        if (!finalCl_tmp.empty()) {
            *drat << finalCl_tmp << fin;
        }

        //Empty clause, it's UNSAT
        if (!okay()) {
            *drat << fin;
        }
        *drat << del << ps << fin;
    }

    if (cl != NULL) {
        ClOffset offset = cl_alloc.get_offset(cl);
        if (!red) {
            longIrredCls.push_back(offset);
        } else {
            cl->stats.which_red_array = 2;
            if (cl->stats.glue <= conf.glue_put_lev0_if_below_or_eq) {
                cl->stats.which_red_array = 0;
            } else if (cl->stats.glue <= conf.glue_put_lev1_if_below_or_eq
                && conf.glue_put_lev1_if_below_or_eq != 0
            ) {
                cl->stats.which_red_array = 1;
            }
            longRedCls[cl->stats.which_red_array].push_back(offset);
        }
    }

    zeroLevAssignsByCNF += trail.size() - origTrailSize;

    return ok;
}

void Solver::test_renumbering() const
{
    //Check if we renumbered the varibles in the order such as to make
    //the unknown ones first and the known/eliminated ones second
    bool uninteresting = false;
    bool problem = false;
    for(size_t i = 0; i < nVars(); i++) {
        //cout << "val[" << i << "]: " << value(i);

        if (value(i)  != l_Undef)
            uninteresting = true;

        if (varData[i].removed == Removed::elimed
            || varData[i].removed == Removed::replaced
            || varData[i].removed == Removed::decomposed
        ) {
            uninteresting = true;
            //cout << " removed" << endl;
        } else {
            //cout << " non-removed" << endl;
        }

        if (value(i) == l_Undef
            && varData[i].removed != Removed::elimed
            && varData[i].removed != Removed::replaced
            && varData[i].removed != Removed::decomposed
            && uninteresting
        ) {
            problem = true;
        }
    }
    assert(!problem && "We renumbered the variables in the wrong order!");
}

void Solver::renumber_clauses(const vector<uint32_t>& outerToInter)
{
    //Clauses' abstractions have to be re-calculated
    for(size_t i = 0; i < longIrredCls.size(); i++) {
        Clause* cl = cl_alloc.ptr(longIrredCls[i]);
        updateLitsMap(*cl, outerToInter);
        cl->setStrenghtened();
    }

    for(auto& lredcls: longRedCls) {
        for(ClOffset off: lredcls) {
            Clause* cl = cl_alloc.ptr(off);
            updateLitsMap(*cl, outerToInter);
            cl->setStrenghtened();
        }
    }
}

void Solver::renumber_xor_clauses(const vector<uint32_t>& outerToInter)
{
    //Clauses' abstractions have to be re-calculated
    for(Xor& x: xorclauses) {
        for(uint32_t& v: x) {
            v = getUpdatedVar(v, outerToInter);
        }
    }
}

size_t Solver::calculate_interToOuter_and_outerToInter(
    vector<uint32_t>& outerToInter
    , vector<uint32_t>& interToOuter
) {
    size_t at = 0;
    vector<uint32_t> useless;
    size_t numEffectiveVars = 0;
    for(size_t i = 0; i < nVars(); i++) {
        if (value(i) != l_Undef
            || varData[i].removed == Removed::elimed
            || varData[i].removed == Removed::replaced
            || varData[i].removed == Removed::decomposed
        ) {
            useless.push_back(i);
            continue;
        }

        outerToInter[i] = at;
        interToOuter[at] = i;
        at++;
        numEffectiveVars++;
    }

    //Fill the rest with variables that have been removed/eliminated/set
    for(vector<uint32_t>::const_iterator
        it = useless.begin(), end = useless.end()
        ; it != end
        ; ++it
    ) {
        outerToInter[*it] = at;
        interToOuter[at] = *it;
        at++;
    }
    assert(at == nVars());

    //Extend to nVarsOuter() --> these are just the identity transformation
    for(size_t i = nVars(); i < nVarsOuter(); i++) {
        outerToInter[i] = i;
        interToOuter[i] = i;
    }

    return numEffectiveVars;
}

double Solver::calc_renumber_saving()
{
    uint32_t num_used = 0;
    for(size_t i = 0; i < nVars(); i++) {
        if (value(i) != l_Undef
            || varData[i].removed == Removed::elimed
            || varData[i].removed == Removed::replaced
            || varData[i].removed == Removed::decomposed
        ) {
            continue;
        }
        num_used++;
    }
    double saving = 1.0-(double)num_used/(double)nVars();
    return saving;
}

//Beware. Cannot be called while Searcher is running.
void Solver::renumber_variables(bool must_renumber)
{
    if (nVars() == 0) {
        return;
    }

    if (!must_renumber
        && calc_renumber_saving() < 0.2
    ) {
        return;
    }

    double myTime = cpuTime();
    clauseCleaner->remove_and_clean_all();

    //outerToInter[10] = 0 ---> what was 10 is now 0.
    vector<uint32_t> outerToInter(nVarsOuter());
    vector<uint32_t> interToOuter(nVarsOuter());

    size_t numEffectiveVars =
        calculate_interToOuter_and_outerToInter(outerToInter, interToOuter);

    //Create temporary outerToInter2
    vector<uint32_t> interToOuter2(nVarsOuter()*2);
    for(size_t i = 0; i < nVarsOuter(); i++) {
        interToOuter2[i*2] = interToOuter[i]*2;
        interToOuter2[i*2+1] = interToOuter[i]*2+1;
    }

    renumber_clauses(outerToInter);
    CNF::updateVars(outerToInter, interToOuter);
    PropEngine::updateVars(outerToInter, interToOuter, interToOuter2);
    Searcher::updateVars(outerToInter, interToOuter);

    if (conf.doStamp) {
        stamp.updateVars(outerToInter, interToOuter2, seen);
    }
    renumber_xor_clauses(outerToInter);

    //Update sub-elements' vars
    varReplacer->updateVars(outerToInter, interToOuter);
    if (conf.doCache) {
        implCache.updateVars(seen, outerToInter, interToOuter2, numEffectiveVars);
    }
    datasync->updateVars(outerToInter, interToOuter);

    //Tests
    test_renumbering();
    test_reflectivity_of_renumbering();

    //Print results
    const double time_used = cpuTime() - myTime;
    if (conf.verbosity) {
        cout
        << "c [renumber]"
        << conf.print_times(time_used)
        << endl;
    }
    if (sqlStats) {
        sqlStats->time_passed_min(
            solver
            , "renumber"
            , time_used
        );
    }

    if (conf.doSaveMem) {
        save_on_var_memory(numEffectiveVars);
    }

    //NOTE order heap is now wrong, but that's OK, it will be restored from
    //backed up activities and then rebuilt at the start of Searcher
}

void Solver::check_switchoff_limits_newvar(size_t n)
{
    if (conf.doStamp
        && nVars() + n > 15ULL*1000ULL*1000ULL //~1 GB of RAM
    ) {
        conf.doStamp = false;
        stamp.freeMem();
        if (conf.verbosity) {
            cout
            << "c Switching off stamping due to excessive number of variables"
            << " (it would take too much memory)"
            << endl;
        }
    }

    if (conf.doCache
        && nVars() + n > 5ULL*1000ULL*1000ULL
    ) {
        conf.doCache = false;
        implCache.free();

        if (conf.verbosity) {
            cout
            << "c Switching off caching due to excessive number of variables"
            << " (it would take too much memory)"
            << endl;
        }
    }
}

void Solver::new_vars(size_t n)
{
    if (n == 0) {
        return;
    }

    check_switchoff_limits_newvar(n);
    Searcher::new_vars(n);
    varReplacer->new_vars(n);

    if (conf.perform_occur_based_simp) {
        occsimplifier->new_vars(n);
    }

    if (compHandler) {
        compHandler->new_vars(n);
    }
    datasync->new_vars(n);
}

void Solver::new_var(const bool bva, const uint32_t orig_outer)
{
    check_switchoff_limits_newvar();
    Searcher::new_var(bva, orig_outer);

    varReplacer->new_var(orig_outer);

    if (conf.perform_occur_based_simp) {
        occsimplifier->new_var(orig_outer);
    }

    if (compHandler) {
        compHandler->new_var(orig_outer);
    }
    if (orig_outer == std::numeric_limits<uint32_t>::max()) {
        datasync->new_var(bva);
    }

    if (bva) {
        assumptionsSet.push_back(false);
    }

    //Too expensive
    //test_reflectivity_of_renumbering();
}

void Solver::save_on_var_memory(const uint32_t newNumVars)
{
    //print_mem_stats();

    const double myTime = cpuTime();
    minNumVars = newNumVars;
    Searcher::save_on_var_memory();

    varReplacer->save_on_var_memory();
    if (occsimplifier) {
        occsimplifier->save_on_var_memory();
    }
    if (compHandler) {
        compHandler->save_on_var_memory();
    }
    datasync->save_on_var_memory();
    assumptionsSet.resize(nVars(), false);
    assumptionsSet.shrink_to_fit();

    const double time_used = cpuTime() - myTime;
    if (sqlStats) {
        sqlStats->time_passed_min(
            this
            , "save var mem"
            , time_used
        );
    }
    //print_mem_stats();
}

//Uneliminates, readds components, fills assumptionsSet, all the good stuff
void Solver::set_assumptions()
{
    assert(okay());

    conflict.clear();
    assumptions.clear();

    back_number_from_outside_to_outer(outside_assumptions);
    vector<Lit> inter_assumptions = back_number_from_outside_to_outer_tmp;
    addClauseHelper(inter_assumptions);
    assumptionsSet.resize(nVars(), false);
    if (outside_assumptions.empty()) {
        return;
    }

    assert(inter_assumptions.size() == outside_assumptions.size());
    for(size_t i = 0; i < inter_assumptions.size(); i++) {
        const Lit inter_lit = inter_assumptions[i];
        const Lit outside_lit = outside_assumptions[i];
        assumptions.push_back(AssumptionPair(inter_lit, outside_lit));
    }

    fill_assumptions_set_from(assumptions);
}

void Solver::check_model_for_assumptions() const
{
    for(const AssumptionPair lit_pair: assumptions) {
        const Lit outside_lit = lit_pair.lit_orig_outside;
        assert(outside_lit.var() < model.size());

        if (model_value(outside_lit) == l_Undef) {
            std::cerr
            << "ERROR, lit " << outside_lit
            << " was in the assumptions, but it wasn't set at all!"
            << endl;
        }
        assert(model_value(outside_lit) != l_Undef);

        if (model_value(outside_lit) != l_True) {
            std::cerr
            << "ERROR, lit " << outside_lit
            << " was in the assumptions, but it was set to its opposite value!"
            << endl;
        }
        assert(model_value(outside_lit) == l_True);
    }
}

void Solver::check_recursive_minimization_effectiveness(const lbool status)
{
    const SearchStats& stats = Searcher::get_stats();
    if (status == l_Undef
        && conf.doRecursiveMinim
        && stats.recMinLitRem + stats.litsRedNonMin > 100000
    ) {
        double remPercent =
            float_div(stats.recMinLitRem, stats.litsRedNonMin)*100.0;

        double costPerGained = float_div(stats.recMinimCost, remPercent);
        if (costPerGained > 200ULL*1000ULL*1000ULL) {
            conf.doRecursiveMinim = false;
            if (conf.verbosity) {
                cout
                << "c recursive minimization too costly: "
                << std::fixed << std::setprecision(0) << (costPerGained/1000.0)
                << "Kcost/(% lits removed) --> disabling"
                << std::setprecision(2)
                << endl;
            }
        } else {
            if (conf.verbosity) {
                cout
                << "c recursive minimization cost OK: "
                << std::fixed << std::setprecision(0) << (costPerGained/1000.0)
                << "Kcost/(% lits removed)"
                << std::setprecision(2)
                << endl;
            }
        }
    }
}

void Solver::check_minimization_effectiveness(const lbool status)
{
    const SearchStats& search_stats = Searcher::get_stats();
    if (status == l_Undef
        && conf.doMinimRedMore
        && search_stats.moreMinimLitsStart > 100000
    ) {
        double remPercent =
            (double)(search_stats.moreMinimLitsStart-search_stats.moreMinimLitsEnd)/
                (double)(search_stats.moreMinimLitsStart)*100.0;

        //TODO take into account the limit on the number of first literals, too
        if (remPercent < 1.0) {
            conf.doMinimRedMore = false;
            if (conf.verbosity) {
                cout
                << "c more minimization effectiveness low: "
                << std::fixed << std::setprecision(2) << remPercent
                << " % lits removed --> disabling"
                << endl;
            }
        } else if (remPercent > 7.0) {
            more_red_minim_limit_binary_actual = 3*conf.more_red_minim_limit_binary;
            more_red_minim_limit_cache_actual  = 3*conf.more_red_minim_limit_cache;
            if (conf.verbosity) {
                cout
                << "c more minimization effectiveness good: "
                << std::fixed << std::setprecision(2) << remPercent
                << " % --> increasing limit to 3x"
                << endl;
            }
        } else {
            more_red_minim_limit_binary_actual = conf.more_red_minim_limit_binary;
            more_red_minim_limit_cache_actual  = conf.more_red_minim_limit_cache;
            if (conf.verbosity) {
                cout
                << "c more minimization effectiveness OK: "
                << std::fixed << std::setprecision(2) << remPercent
                << " % --> setting limit to norm"
                << endl;
            }
        }
    }
}

void Solver::extend_solution()
{
    #ifdef DEBUG_IMPLICIT_STATS
    check_stats();
    #endif

    const double myTime = cpuTime();
    model = back_number_solution_from_inter_to_outer(model);
    full_model = back_number_solution_from_inter_to_outer(full_model);

    //Extend solution to stored solution in component handler
    if (compHandler) {
        compHandler->addSavedState(model);
        compHandler->addSavedState(full_model);
    }

    SolutionExtender extender(this, occsimplifier);
    extender.extend();

    model = map_back_to_without_bva(model);
    check_model_for_assumptions();
    if (sqlStats) {
        sqlStats->time_passed_min(
            this
            , "extend solution"
            , cpuTime()-myTime
        );
    }
}

void Solver::set_up_sql_writer()
{
    if (!sqlStats) {
        return;
    }

    bool ret = sqlStats->setup(this);
    if (!ret) {
        std::cerr
        << "c ERROR: SQL was required (with option '--sql 2'), but couldn't connect to SQL server." << endl;
        std::exit(-1);
    }
}

void Solver::check_config_parameters() const
{
    if (conf.maxConfl < 0) {
        std::cerr << "Maximum number conflicts set must be greater or equal to 0" << endl;
        exit(-1);
    }

    if (conf.shortTermHistorySize <= 0) {
        std::cerr << "You MUST give a short term history size (\"--gluehist\")  greater than 0!" << endl;
        exit(-1);
    }
}

lbool Solver::simplify_problem_outside()
{
    #ifdef SLOW_DEBUG
    if (ok) {
        assert(check_order_heap_sanity());
        check_implicit_stats();
        find_all_attach();
        test_all_clause_attached();
    }
    #endif

    conf.global_timeout_multiplier = conf.orig_global_timeout_multiplier;

    if (!ok) {
        return l_False;
    }
    conflict.clear();
    check_config_parameters();
    datasync->rebuild_bva_map();
    set_assumptions();

    lbool status = l_Undef;
    if (nVars() > 0 && conf.do_simplify_problem) {
        status = simplify_problem(false);
    }
    unfill_assumptions_set_from(assumptions);
    return status;
}

lbool Solver::solve()
{
    #ifdef SLOW_DEBUG
    if (ok) {
        assert(check_order_heap_sanity());
        check_implicit_stats();
        find_all_attach();
    }
    #endif

    solveStats.num_solve_calls++;
    conflict.clear();
    check_config_parameters();

    //Parameters for restarts
    max_confl_phase = conf.restart_first;
    max_confl_this_phase = max_confl_phase;
    VSIDS = true;
    params.rest_type = conf.restartType;
    if (params.rest_type == Restart::glue_geom) {
        params.rest_type = Restart::geom;
    }

    if (conf.verbosity >= 6) {
        cout << "c " << __func__ << " called" << endl;
    }
    conf.global_timeout_multiplier = conf.orig_global_timeout_multiplier;

    //Check if adding the clauses caused UNSAT
    lbool status = l_Undef;
    if (!ok) {
        assert(conflict.empty());
        status = l_False;
        if (conf.verbosity >= 6) {
            cout << "c Solver status " << status << " on startup of solve()" << endl;
        }
        goto end;
    }

    //Clean up as a startup
    datasync->rebuild_bva_map();
    set_assumptions();

    if (conf.preprocess == 2) {
        status = load_state(conf.saved_state_file);
        if (status != l_False) {
            model = assigns;
            status = load_solution_from_file(conf.solution_file);
            full_model = model;
        }
    }

    //If still unknown, simplify
    if (status == l_Undef
        && nVars() > 0
        && conf.do_simplify_problem
        && conf.simplify_at_startup
        && (solveStats.numSimplify == 0 || conf.simplify_at_every_startup)
    ) {
        status = simplify_problem(!conf.full_simplify_at_startup);
    }

    if (status == l_Undef
        && conf.preprocess == 0
    ) {
        #ifdef USE_GAUSS
        clear_gauss();
        MatrixFinder finder(this);
        ok = finder.findMatrixes();
        if (!ok) {
            status = l_False;
            goto end;
        }
        #endif
        status = iterate_until_solved();
    }

    end:
    if (sqlStats) {
        sqlStats->finishup(status);
    }

    if (conf.preprocess == 1) {
        cancelUntil(0);
        if (status != l_False) {
            //So no set variables end up in the clauses
            clauseCleaner->remove_and_clean_all();
        }

        if (status == l_True) {
            cout << "WARN: Solution found during preprocessing,"
            "but putting simplified CNF to file" << endl;
        }
        save_state(conf.saved_state_file, status);
        ClauseDumper dumper(this);
        if (status == l_False) {
            dumper.open_file_and_write_unsat(conf.simplified_cnf);
        } else {
            dumper.open_file_and_dump_irred_clauses_preprocessor(conf.simplified_cnf);
        }
        cout << "Wrote solver state to file " << conf.saved_state_file
        << " and simplified CNF to file " << conf.simplified_cnf
        << endl;
    }

    handle_found_solution(status);
    unfill_assumptions_set_from(assumptions);
    conf.maxConfl = std::numeric_limits<long>::max();
    conf.maxTime = std::numeric_limits<double>::max();
    return status;
}

void Solver::dump_memory_stats_to_sql()
{
    if (!sqlStats) {
        return;
    }

    const double my_time = cpuTime();

    sqlStats->mem_used(
        this
        , "solver"
        , my_time
        , mem_used()/(1024*1024)
    );

    sqlStats->mem_used(
        this
        , "vardata"
        , my_time
        , mem_used_vardata()/(1024*1024)
    );

    sqlStats->mem_used(
        this
        , "stamp"
        , my_time
        , Searcher::mem_used_stamp()/(1024*1024)
    );

    sqlStats->mem_used(
        this
        , "cache"
        , my_time
        , implCache.mem_used()/(1024*1024)
    );

    sqlStats->mem_used(
        this
        , "longclauses"
        , my_time
        , CNF::mem_used_longclauses()/(1024*1024)
    );

    sqlStats->mem_used(
        this
        , "watch-alloc"
        , my_time
        , watches.mem_used_alloc()/(1024*1024)
    );

    sqlStats->mem_used(
        this
        , "watch-array"
        , my_time
        , watches.mem_used_array()/(1024*1024)
    );

    sqlStats->mem_used(
        this
        , "renumber"
        , my_time
        , CNF::mem_used_renumberer()/(1024*1024)
    );


    if (compHandler) {
        sqlStats->mem_used(
            this
            , "component"
            , my_time
            , compHandler->mem_used()/(1024*1024)
        );
    }

    if (occsimplifier) {
        sqlStats->mem_used(
            this
            , "occsimplifier"
            , my_time
            , occsimplifier->mem_used()/(1024*1024)
        );

        sqlStats->mem_used(
            this
            , "xor"
            , my_time
            , occsimplifier->mem_used_xor()/(1024*1024)
        );

        sqlStats->mem_used(
            this
            , "bva"
            , my_time
            , occsimplifier->mem_used_bva()/(1024*1024)
        );
    }

    sqlStats->mem_used(
        this
        , "varreplacer"
        , my_time
        , varReplacer->mem_used()/(1024*1024)
    );

    if (prober) {
        sqlStats->mem_used(
            this
            , "prober"
            , my_time
            , prober->mem_used()/(1024*1024)
        );
    }

    double vm_mem_used = 0;
    const uint64_t rss_mem_used = memUsedTotal(vm_mem_used);
    sqlStats->mem_used(
        this
        , "rss"
        , my_time
        , rss_mem_used/(1024*1024)
    );
    sqlStats->mem_used(
        this
        , "vm"
        , my_time
        , vm_mem_used/(1024*1024)
    );
}

long Solver::calc_num_confl_to_do_this_iter(const size_t iteration_num) const
{
    double mult = std::pow(conf.num_conflicts_of_search_inc, (double)iteration_num);
    mult = std::min(mult, conf.num_conflicts_of_search_inc_max);
    long num_conflicts_of_search = (double)conf.num_conflicts_of_search*mult;
    if (conf.never_stop_search) {
        num_conflicts_of_search = 500ULL*1000ULL*1000ULL;
    }
    num_conflicts_of_search = std::min<long>(
        num_conflicts_of_search
        , (long)conf.maxConfl - (long)sumConflicts
    );

    return num_conflicts_of_search;
}

lbool Solver::iterate_until_solved()
{
    uint64_t backup_burst_len = conf.burst_search_len;
    conf.burst_search_len = 0;
    size_t iteration_num = 0;

    lbool status = l_Undef;
    while (status == l_Undef
        && !must_interrupt_asap()
        && cpuTime() < conf.maxTime
        && sumConflicts < (uint64_t)conf.maxConfl
    ) {
        iteration_num++;
        if (conf.verbosity && iteration_num >= 2) {
            print_clause_size_distrib();
        }
        if (iteration_num >= 2) {
            conf.burst_search_len = backup_burst_len;
        }
        dump_memory_stats_to_sql();

        const long num_confl = calc_num_confl_to_do_this_iter(iteration_num);
        if (num_confl <= 0) {
            break;
        }
        status = Searcher::solve(num_confl, iteration_num);
        clear_gauss();

        //Check for effectiveness
        check_recursive_minimization_effectiveness(status);
        check_minimization_effectiveness(status);

        //Update stats
        sumSearchStats += Searcher::get_stats();
        sumPropStats += propStats;
        propStats.clear();
        Searcher::resetStats();
        check_too_many_low_glues();

        //Solution has been found
        if (status != l_Undef) {
            break;
        }

        //If we are over the limit, exit
        if (sumConflicts >= (uint64_t)conf.maxConfl
            || cpuTime() > conf.maxTime
            || must_interrupt_asap()
        ) {
            break;
        }

        if (conf.do_simplify_problem) {
            status = simplify_problem(false);
        }
    }

    clear_gauss();
    conf.burst_search_len = backup_burst_len;
    return status;
}

void Solver::check_too_many_low_glues()
{
    if (conf.glue_put_lev0_if_below_or_eq == 2
        || sumConflicts < conf.min_num_confl_adjust_glue_cutoff
        || adjusted_glue_cutoff_if_too_many
        || conf.adjust_glue_if_too_many_low >= 1.0
    ) {
        return;
    }

    double perc = float_div(sumSearchStats.red_cl_in_which0, sumConflicts);
    if (perc > conf.adjust_glue_if_too_many_low) {
        conf.glue_put_lev0_if_below_or_eq--;
        adjusted_glue_cutoff_if_too_many = true;
        if (conf.verbosity) {
            cout << "c Adjusted glue cutoff to " << conf.glue_put_lev0_if_below_or_eq
            << " due to too many low glues: " << perc*100.0 << " %" << endl;
        }
    }
}

void Solver::handle_found_solution(const lbool status)
{
    if (status == l_True) {
        extend_solution();
        cancelUntil(0);

        find_all_attach();
        test_all_clause_attached();
    } else if (status == l_False) {
        cancelUntil(0);

        for(const Lit lit: conflict) {
            if (value(lit) == l_Undef) {
                assert(var_inside_assumptions(lit.var()));
            }
        }
        update_assump_conflict_to_orig_outside(conflict);
    }

    //Too slow when running lots of small queries
    #ifdef DEBUG_IMPLICIT_STATS
    check_implicit_stats();
    #endif
}

bool Solver::execute_inprocess_strategy(
    const bool startup
    , const string& strategy
) {
    //std::string input = "abc,def,ghi";
    std::istringstream ss(strategy + ", ");
    std::string token;
    std::string occ_strategy_tokens;

    while(std::getline(ss, token, ',')) {
        if (sumConflicts >= (uint64_t)conf.maxConfl
            || cpuTime() > conf.maxTime
            || must_interrupt_asap()
            || nVars() == 0
            || !ok
        ) {
            return ok;
        }
        assert(watches.get_smudged_list().empty());
        #ifdef SLOW_DEBUG
        check_stats();
        #endif

        token = trim(token);
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        if (conf.verbosity && token.substr(0,3) != "occ" && token != "") {
            cout << "c --> Executing strategy token: " << token << '\n';
        }

        if (!occ_strategy_tokens.empty() && token.substr(0,3) != "occ") {
            if (conf.perform_occur_based_simp
                && occsimplifier
            ) {
                occ_strategy_tokens = trim(occ_strategy_tokens);
                if (conf.verbosity) {
                    cout << "c --> Executing OCC strategy token(s): '"
                    << occ_strategy_tokens << "'\n";
                }
                occsimplifier->simplify(startup, occ_strategy_tokens);
                if (ok && occ_strategy_tokens == "occ-gauss,") {
                    #ifdef USE_GAUSS
                    MatrixFinder finder(this);
                    finder.findMatrixes();
                    #endif
                }
            }
            occ_strategy_tokens.clear();
            if (sumConflicts >= (uint64_t)conf.maxConfl
                || cpuTime() > conf.maxTime
                || must_interrupt_asap()
                || nVars() == 0
                || !ok
            ) {
                return ok;
            }
            #ifdef SLOW_DEBUG
            solver->check_stats();
            #endif
        }

        if (token == "find-comps") {
            if (get_num_free_vars() < conf.compVarLimit) {
                CompFinder findParts(this);
                findParts.find_components();
            }
        } else if (token == "handle-comps") {
            if (compHandler
                && conf.doCompHandler
                && get_num_free_vars() < conf.compVarLimit
                && solveStats.numSimplify >= conf.handlerFromSimpNum
                //Only every 2nd, since it can be costly to find parts
                && solveStats.numSimplify % 2 == 0 //TODO
            ) {
                compHandler->handle();
            }
        }  else if (token == "scc-vrepl") {
            if (conf.doFindAndReplaceEqLits) {
                varReplacer->replace_if_enough_is_found(
                    std::floor((double)get_num_free_vars()*0.001));
            }
        } else if (token == "cache-clean") {
            if (conf.doCache) {
                implCache.clean(this);
            }
        } else if (token == "cache-tryboth") {
            if (conf.doCache) {
                implCache.tryBoth(this);
            }
        } else if (token == "sub-impl") {
            //Susbsume TRI with BIN+TRI+stamp, subsume BIN with BIN
            if (conf.doStrSubImplicit) {
                subsumeImplicit->subsume_implicit();
            }
        } else if (token == "intree-probe") {
            if (conf.doIntreeProbe) {
                intree->intree_probe();
            }
        } else if (token == "probe") {
            if (conf.doProbe)
                prober->probe();
        } else if (token == "sub-str-cls-with-bin") {
            //Subsumes and strengthens long clauses with binary clauses
            if (conf.do_distill_clauses) {
                dist_long_with_impl->distill_long_with_implicit(true);
            }
        } else if (token == "distill-cls") {
            //Enqueues literals in long + tri clauses two-by-two and propagates
            if (conf.do_distill_clauses) {
                distill_all_with_all->distill(conf.distill_queue_by);
            }
        } else if (token == "str-impl") {
            //Strengthens BIN&TRI with BIN&TRI
            if (conf.doStrSubImplicit) {
                dist_impl_with_impl->str_impl_w_impl_stamp();
            }
        } else if (token == "check-cache-size") {
            //Delete and disable cache if too large
            if (conf.doCache) {
                const size_t memUsedMB = implCache.mem_used()/(1024UL*1024UL);
                if (memUsedMB > conf.maxCacheSizeMB) {
                    if (conf.verbosity) {
                        cout
                        << "c Turning off cache, memory used, "
                        << memUsedMB << " MB"
                        << " is over limit of " << conf.maxCacheSizeMB  << " MB"
                        << endl;
                    }
                    implCache.free();
                    conf.doCache = false;
                }
            }
        } else if (token == "renumber" || token == "must-renumber") {
            if (conf.doRenumberVars) {
                //Clean cache before renumber -- very important, otherwise
                //we will be left with lits inside the cache that are out-of-bounds
                if (conf.doCache) {
                    bool setSomething = true;
                    while(setSomething) {
                        if (!implCache.clean(this, &setSomething))
                            return false;
                    }
                }

                renumber_variables(token == "must-renumber");
            }
        } else if (token == "") {
            //Nothing, just an empty comma, ignore
        } else if (token.substr(0,3) == "occ") {
            occ_strategy_tokens += token + ", ";
            //cout << "occ_strategy_tokens now: " << occ_strategy_tokens  << endl;
        } else {
            cout << "ERROR: strategy '" << token << "' not recognised!" << endl;
            exit(-1);
        }

        #ifdef SLOW_DEBUG
        check_stats();
        #endif

        if (!ok) {
            return ok;
        }
    }

    return ok;
}

/**
@brief The function that brings together almost all CNF-simplifications
*/
lbool Solver::simplify_problem(const bool startup)
{
    assert(ok);
    test_all_clause_attached();
    find_all_attach();
    #ifdef DEBUG_IMPLICIT_STATS
    check_stats();
    #endif
    #ifdef SLOW_DEBUG
    assert(check_order_heap_sanity());
    #endif

    clear_order_heap();

    if (conf.verbosity >= 6) {
        cout
        << "c " <<  __func__ << " called"
        << endl;
    }

    if (startup) {
        execute_inprocess_strategy(startup, conf.simplify_schedule_startup);
    } else {
        execute_inprocess_strategy(startup, conf.simplify_schedule_nonstartup);
    }

    //Free unused watch memory
    free_unused_watches();

    if (conf.verbosity >= 6) {
        cout << "c " << __func__ << " finished" << endl;
    }
    conf.global_timeout_multiplier *= conf.global_timeout_multiplier_multiplier;
    conf.global_timeout_multiplier =
        std::min(
            conf.global_timeout_multiplier, conf.orig_global_timeout_multiplier*conf.global_multiplier_multiplier_max
        );

    //Reconfigure
    if (nVars() > 2
        && (longIrredCls.size() > 1 || (binTri.irredBins + binTri.redBins))
    ) {
        if (solveStats.numSimplify == conf.reconfigure_at) {
            SolveFeatures feat = calculate_features();
            if (conf.reconfigure_val == 100) {
                conf.reconfigure_val = get_reconf_from_features(feat, conf.verbosity);
            }
            if (conf.reconfigure_val != 0) {
                reconfigure(conf.reconfigure_val);
            }
        }
    }

    solveStats.numSimplify++;

    if (!ok) {
        return l_False;
    } else {
        check_stats();
        check_implicit_propagated();
        rebuildOrderHeap();
        find_all_attach();
        test_all_clause_attached();

        return l_Undef;
    }
}

void Solver::print_prop_confl_stats(
    std::string name
    , const vector<ClauseUsageStats>& stats
) const {
    for(size_t i = 0; i < stats.size(); i++) {
        //Nothing to do here, no stats really
        if (stats[i].num == 0)
            continue;

        cout
        << name << " : " << std::setw(4) << i
        << " Avg. props: " << std::setw(6) << std::fixed << std::setprecision(2)
        << float_div(stats[i].sumProp, stats[i].num);

        cout
        << name << " : " << std::setw(4) << i
        << " Avg. confls: " << std::setw(6) << std::fixed << std::setprecision(2)
        << float_div(stats[i].sumConfl, stats[i].num);

        if (stats[i].sumLookedAt > 0) {
            cout
            << " Props&confls/looked at: " << std::setw(6) << std::fixed << std::setprecision(2)
            << float_div(stats[i].sumPropAndConfl(), stats[i].sumLookedAt);
        }

        cout << endl;
    }
}

void Solver::print_stats(const double cpu_time) const
{
    cout << "c ------- FINAL TOTAL SEARCH STATS ---------" << endl;
    print_stats_line("c UIP search time"
        , sumSearchStats.cpu_time
        , stats_line_percent(sumSearchStats.cpu_time, cpu_time)
        , "% time"
    );

    if (conf.verbStats >= 2) {
        print_full_restart_stat(cpu_time);
    } else if (conf.verbStats == 1) {
        print_norm_stats(cpu_time);
    } else {
        print_min_stats(cpu_time);
    }
}


void Solver::print_min_stats(const double cpu_time) const
{
    sumSearchStats.print_short(sumPropStats.propagations);
    print_stats_line("c props/decision"
        , float_div(propStats.propagations, sumSearchStats.decisions)
    );
    print_stats_line("c props/conflict"
        , float_div(propStats.propagations, sumConflicts)
    );

    print_stats_line("c 0-depth assigns", trail.size()
        , stats_line_percent(trail.size(), nVars())
        , "% vars"
    );

    //Failed lit stats
    if (conf.doProbe) {
        print_stats_line("c probing time"
            , prober->get_stats().cpu_time
            , stats_line_percent(prober->get_stats().cpu_time, cpu_time)
            , "% time"
        );
    }
    //OccSimplifier stats
    if (conf.perform_occur_based_simp) {
        print_stats_line("c OccSimplifier time"
            , occsimplifier->get_stats().total_time()
            , stats_line_percent(occsimplifier->get_stats().total_time() ,cpu_time)
            , "% time"
        );
    }
    print_stats_line("c SCC time"
        , varReplacer->get_scc_finder()->get_stats().cpu_time
        , stats_line_percent(varReplacer->get_scc_finder()->get_stats().cpu_time, cpu_time)
        , "% time"
    );
    varReplacer->get_scc_finder()->get_stats().print_short(NULL);

    //varReplacer->get_stats().print_short(nVars());
    print_stats_line("c distill time"
                    , distill_all_with_all->get_stats().time_used
                    , stats_line_percent(distill_all_with_all->get_stats().time_used, cpu_time)
                    , "% time"
    );
    print_stats_line("c strength cache-irred time"
                    , dist_long_with_impl->get_stats().irredCacheBased.cpu_time
                    , stats_line_percent(dist_long_with_impl->get_stats().irredCacheBased.cpu_time, cpu_time)
                    , "% time"
    );
    print_stats_line("c strength cache-red time"
                    , dist_long_with_impl->get_stats().redCacheBased.cpu_time
                    , stats_line_percent(dist_long_with_impl->get_stats().redCacheBased.cpu_time, cpu_time)
                    , "% time"
    );
    print_stats_line("c Conflicts in UIP"
        , sumConflicts
        , float_div(sumConflicts, cpu_time)
        , "confl/TOTAL_TIME_SEC"
    );
    print_stats_line("c Total time", cpu_time);
    double vm_usage;
    print_stats_line("c Mem used"
        , (double)memUsedTotal(vm_usage)/(1024UL*1024UL)
        , "MB"
    );
}

void Solver::print_norm_stats(const double cpu_time) const
{
    sumSearchStats.print_short(sumPropStats.propagations);
    print_stats_line("c props/decision"
        , float_div(propStats.propagations, sumSearchStats.decisions)
    );
    print_stats_line("c props/conflict"
        , float_div(propStats.propagations, sumConflicts)
    );

    print_stats_line("c 0-depth assigns", trail.size()
        , stats_line_percent(trail.size(), nVars())
        , "% vars"
    );
    print_stats_line("c 0-depth assigns by CNF"
        , zeroLevAssignsByCNF
        , stats_line_percent(zeroLevAssignsByCNF, nVars())
        , "% vars"
    );

    print_stats_line("c reduceDB time"
        , reduceDB->get_stats().cpu_time
        , stats_line_percent(reduceDB->get_stats().cpu_time, cpu_time)
        , "% time"
    );

    //Failed lit stats
    if (conf.doProbe
        && prober
    ) {
        prober->get_stats().print_short(this, 0, 0);
        print_stats_line("c probing time"
            , prober->get_stats().cpu_time
            , stats_line_percent(prober->get_stats().cpu_time, cpu_time)
            , "% time"
        );

        prober->get_stats().print_short(this, 0, 0);
    }
    //OccSimplifier stats
    if (conf.perform_occur_based_simp) {
        print_stats_line("c OccSimplifier time"
            , occsimplifier->get_stats().total_time()
            , stats_line_percent(occsimplifier->get_stats().total_time() ,cpu_time)
            , "% time"
        );
        occsimplifier->get_stats().print_short();
    }
    print_stats_line("c SCC time"
        , varReplacer->get_scc_finder()->get_stats().cpu_time
        , stats_line_percent(varReplacer->get_scc_finder()->get_stats().cpu_time, cpu_time)
        , "% time"
    );
    varReplacer->get_scc_finder()->get_stats().print_short(NULL);
    varReplacer->print_some_stats(cpu_time);

    //varReplacer->get_stats().print_short(nVars());
    print_stats_line("c distill time"
                    , distill_all_with_all->get_stats().time_used
                    , stats_line_percent(distill_all_with_all->get_stats().time_used, cpu_time)
                    , "% time"
    );
    print_stats_line("c strength cache-irred time"
                    , dist_long_with_impl->get_stats().irredCacheBased.cpu_time
                    , stats_line_percent(dist_long_with_impl->get_stats().irredCacheBased.cpu_time, cpu_time)
                    , "% time"
    );
    print_stats_line("c strength cache-red time"
                    , dist_long_with_impl->get_stats().redCacheBased.cpu_time
                    , stats_line_percent(dist_long_with_impl->get_stats().redCacheBased.cpu_time, cpu_time)
                    , "% time"
    );
    if (conf.doCache) {
        implCache.print_statsSort(this);
    }

    print_stats_line("c Conflicts in UIP"
        , sumConflicts
        , float_div(sumConflicts, cpu_time)
        , "confl/TOTAL_TIME_SEC"
    );
    double vm_usage;
    print_stats_line("c Mem used"
        , (double)memUsedTotal(vm_usage)/(1024UL*1024UL)
        , "MB"
    );
    print_stats_line("c Total time", cpu_time);
}

void Solver::print_full_restart_stat(const double cpu_time) const
{
    sumSearchStats.print(sumPropStats.propagations);
    sumPropStats.print(sumSearchStats.cpu_time);
    print_stats_line("c props/decision"
        , float_div(propStats.propagations, sumSearchStats.decisions)
    );
    print_stats_line("c props/conflict"
        , float_div(propStats.propagations, sumConflicts)
    );
    cout << "c ------- FINAL TOTAL SOLVING STATS END ---------" << endl;
    reduceDB->get_stats().print(cpu_time);

    print_stats_line("c 0-depth assigns", trail.size()
        , stats_line_percent(trail.size(), nVars())
        , "% vars"
    );
    print_stats_line("c 0-depth assigns by CNF"
        , zeroLevAssignsByCNF
        , stats_line_percent(zeroLevAssignsByCNF, nVars())
        , "% vars"
    );

    //Failed lit stats
    if (conf.doProbe) {
        print_stats_line("c probing time"
            , prober->get_stats().cpu_time
            , stats_line_percent(prober->get_stats().cpu_time, cpu_time)
            , "% time"
        );

        prober->get_stats().print(nVars());
    }

    //OccSimplifier stats
    if (conf.perform_occur_based_simp) {
        print_stats_line("c OccSimplifier time"
            , occsimplifier->get_stats().total_time()
            , stats_line_percent(occsimplifier->get_stats().total_time(), cpu_time)
            , "% time"
        );

        occsimplifier->get_stats().print(nVars());
    }

    //TODO after TRI to LONG conversion
    /*if (occsimplifier && conf.doGateFind) {
        occsimplifier->print_gatefinder_stats();
    }*/

    //VarReplacer stats
    print_stats_line("c SCC time"
        , varReplacer->get_scc_finder()->get_stats().cpu_time
        , stats_line_percent(varReplacer->get_scc_finder()->get_stats().cpu_time, cpu_time)
        , "% time"
    );
    varReplacer->get_scc_finder()->get_stats().print();

    varReplacer->get_stats().print(nVars());
    varReplacer->print_some_stats(cpu_time);

    //DistillerAllWithAll stats
    print_stats_line("c distill time"
                    , distill_all_with_all->get_stats().time_used
                    , stats_line_percent(distill_all_with_all->get_stats().time_used, cpu_time)
                    , "% time");
    distill_all_with_all->get_stats().print(nVars());

    print_stats_line("c strength cache-irred time"
                    , dist_long_with_impl->get_stats().irredCacheBased.cpu_time
                    , stats_line_percent(dist_long_with_impl->get_stats().irredCacheBased.cpu_time, cpu_time)
                    , "% time");
    print_stats_line("c strength cache-red time"
                    , dist_long_with_impl->get_stats().redCacheBased.cpu_time
                    , stats_line_percent(dist_long_with_impl->get_stats().redCacheBased.cpu_time, cpu_time)
                    , "% time");
    dist_long_with_impl->get_stats().print();

    if (conf.doStrSubImplicit) {
        subsumeImplicit->get_stats().print();
    }

    if (conf.doCache) {
        implCache.print_stats(this);
    }

    //Other stats
    print_stats_line("c Conflicts in UIP"
        , sumConflicts
        , float_div(sumConflicts, cpu_time)
        , "confl/TOTAL_TIME_SEC"
    );
    print_stats_line("c Total time", cpu_time);
    print_mem_stats();
}

uint64_t Solver::print_watch_mem_used(const uint64_t rss_mem_used) const
{
    size_t alloc = watches.mem_used_alloc();
    print_stats_line("c Mem for watch alloc"
        , alloc/(1024UL*1024UL)
        , "MB"
        , stats_line_percent(alloc, rss_mem_used)
        , "%"
    );

    size_t array = watches.mem_used_array();
    print_stats_line("c Mem for watch array"
        , array/(1024UL*1024UL)
        , "MB"
        , stats_line_percent(array, rss_mem_used)
        , "%"
    );

    return alloc + array;
}

size_t Solver::mem_used() const
{
    size_t mem = 0;
    mem += Searcher::mem_used();
    mem += outside_assumptions.capacity()*sizeof(Lit);

    return mem;
}

uint64_t Solver::mem_used_vardata() const
{
    uint64_t mem = 0;
    mem += assigns.capacity()*sizeof(lbool);
    mem += varData.capacity()*sizeof(VarData);

    return mem;
}

void Solver::print_mem_stats() const
{
    double vm_mem_used = 0;
    const uint64_t rss_mem_used = memUsedTotal(vm_mem_used);
    print_stats_line("c Mem used"
        , rss_mem_used/(1024UL*1024UL)
        , "MB"
    );
    uint64_t account = 0;

    account += print_mem_used_longclauses(rss_mem_used);
    account += print_watch_mem_used(rss_mem_used);

    size_t mem = 0;
    mem += mem_used_vardata();
    print_stats_line("c Mem for assings&vardata"
        , mem/(1024UL*1024UL)
        , "MB"
        , stats_line_percent(mem, rss_mem_used)
        , "%"
    );
    account += mem;

    mem = implCache.mem_used();
    print_stats_line("c Mem for implication cache"
        , mem/(1024UL*1024UL)
        , "MB"
        , stats_line_percent(mem, rss_mem_used)
        , "%"
    );
    account += mem;

    account += print_stamp_mem(rss_mem_used);

    mem = mem_used();
    print_stats_line("c Mem for search&solve"
        , mem/(1024UL*1024UL)
        , "MB"
        , stats_line_percent(mem, rss_mem_used)
        , "%"
    );
    account += mem;

    mem = CNF::mem_used_renumberer();
    print_stats_line("c Mem for renumberer"
        , mem/(1024UL*1024UL)
        , "MB"
        , stats_line_percent(mem, rss_mem_used)
        , "%"
    );
    account += mem;

    if (compHandler) {
        mem = compHandler->mem_used();
        print_stats_line("c Mem for component handler"
            , mem/(1024UL*1024UL)
            , "MB"
            , stats_line_percent(mem, rss_mem_used)
            , "%"
        );
        account += mem;
    }

    if (occsimplifier) {
        mem = occsimplifier->mem_used();
        print_stats_line("c Mem for occsimplifier"
            , mem/(1024UL*1024UL)
            , "MB"
            , stats_line_percent(mem, rss_mem_used)
            , "%"
        );
        account += mem;

        mem = occsimplifier->mem_used_xor();
        print_stats_line("c Mem for xor-finder"
            , mem/(1024UL*1024UL)
            , "MB"
            , stats_line_percent(mem, rss_mem_used)
            , "%"
        );
        account += mem;
    }

    mem = varReplacer->mem_used();
    print_stats_line("c Mem for varReplacer&SCC"
        , mem/(1024UL*1024UL)
        , "MB"
        , stats_line_percent(mem, rss_mem_used)
        , "%"
    );
    account += mem;

    if (subsumeImplicit) {
        mem = subsumeImplicit->mem_used();
        print_stats_line("c Mem for impl subsume"
            , mem/(1024UL*1024UL)
            , "MB"
            , stats_line_percent(mem, rss_mem_used)
            , "%"
        );
        account += mem;
    }


    mem = distill_all_with_all->mem_used();
    mem += dist_long_with_impl->mem_used();
    mem += dist_impl_with_impl->mem_used();
    print_stats_line("c Mem for 3 distills"
        , mem/(1024UL*1024UL)
        , "MB"
        , stats_line_percent(mem, rss_mem_used)
        , "%"
    );
    account += mem;

    if (prober) {
        mem = prober->mem_used() + intree->mem_used();
        print_stats_line("c Mem for prober+intree"
            , mem/(1024UL*1024UL)
            , "MB"
            , stats_line_percent(mem, rss_mem_used)
            , "%"
        );
        account += mem;
    }

    print_stats_line("c Accounted for mem (rss)"
        , stats_line_percent(account, rss_mem_used)
        , "%"
    );
    print_stats_line("c Accounted for mem (vm)"
        , stats_line_percent(account, vm_mem_used)
        , "%"
    );
}

void Solver::print_clause_size_distrib()
{
    size_t size3 = 0;
    size_t size4 = 0;
    size_t size5 = 0;
    size_t sizeLarge = 0;
    for(vector<ClOffset>::const_iterator
        it = longIrredCls.begin(), end = longIrredCls.end()
        ; it != end
        ; ++it
    ) {
        Clause* cl = cl_alloc.ptr(*it);
        switch(cl->size()) {
            case 0:
            case 1:
            case 2:
                assert(false);
                break;
            case 3:
                size3++;
                break;
            case 4:
                size4++;
                break;
            case 5:
                size5++;
                break;
            default:
                sizeLarge++;
                break;
        }
    }

    cout
    << "c clause size stats."
    << " size3: " << size3
    << " size4: " << size4
    << " size5: " << size5
    << " larger: " << sizeLarge << endl;
}


vector<Lit> Solver::get_zero_assigned_lits() const
{
    vector<Lit> lits;
    assert(decisionLevel() == 0);
    for(size_t i = 0; i < assigns.size(); i++) {
        if (assigns[i] != l_Undef) {
            Lit lit(i, assigns[i] == l_False);

            //Update to higher-up
            lit = varReplacer->get_lit_replaced_with(lit);
            if (varData[lit.var()].is_bva == false) {
                lits.push_back(map_inter_to_outer(lit));
            }

            //Everything it repaces has also been set
            const vector<uint32_t> vars = varReplacer->get_vars_replacing(lit.var());
            for(const uint32_t var: vars) {
                if (varData[var].is_bva)
                    continue;

                Lit tmp_lit = Lit(var, false);
                assert(varReplacer->get_lit_replaced_with(tmp_lit).var() == lit.var());
                if (lit != varReplacer->get_lit_replaced_with(tmp_lit)) {
                    tmp_lit ^= true;
                }
                assert(lit == varReplacer->get_lit_replaced_with(tmp_lit));

                lits.push_back(map_inter_to_outer(tmp_lit));
            }
        }
    }

    //Remove duplicates. Because of above replacing-mimicing algo
    //multipe occurrences of literals can be inside
    vector<Lit>::iterator it;
    std::sort(lits.begin(), lits.end());
    it = std::unique (lits.begin(), lits.end());
    lits.resize( std::distance(lits.begin(),it) );

    //Update to outer without BVA
    vector<uint32_t> my_map = build_outer_to_without_bva_map();
    updateLitsMap(lits, my_map);
    for(const Lit lit: lits) {
        assert(lit.var() < nVarsOutside());
    }

    return lits;
}

bool Solver::verify_model_implicit_clauses() const
{
    uint32_t wsLit = 0;
    for (watch_array::const_iterator
        it = watches.begin(), end = watches.end()
        ; it != end
        ; ++it, wsLit++
    ) {
        Lit lit = Lit::toLit(wsLit);
        watch_subarray_const ws = *it;

        for (Watched w: ws) {
            if (w.isBin()
                && model_value(lit) != l_True
                && model_value(w.lit2()) != l_True
            ) {
                cout
                << "bin clause: "
                << lit << " , " << w.lit2()
                << " not satisfied!"
                << endl;

                cout
                << "value of unsat bin clause: "
                << value(lit) << " , " << value(w.lit2())
                << endl;

                return false;
            }
        }
    }

    return true;
}

bool Solver::verify_model_long_clauses(const vector<ClOffset>& cs) const
{
    #ifdef VERBOSE_DEBUG
    cout << "Checking clauses whether they have been properly satisfied." << endl;
    #endif

    bool verificationOK = true;

    for (vector<ClOffset>::const_iterator
        it = cs.begin(), end = cs.end()
        ; it != end
        ; ++it
    ) {
        Clause& cl = *cl_alloc.ptr(*it);
        for (uint32_t j = 0; j < cl.size(); j++)
            if (model_value(cl[j]) == l_True)
                goto next;

        cout << "unsatisfied clause: " << cl << endl;
        verificationOK = false;
        next:
        ;
    }

    return verificationOK;
}

bool Solver::verify_model() const
{
    bool verificationOK = true;
    verificationOK &= verify_model_long_clauses(longIrredCls);
    for(auto& lredcls: longRedCls) {
        verificationOK &= verify_model_long_clauses(lredcls);
    }
    verificationOK &= verify_model_implicit_clauses();

    if (conf.verbosity && verificationOK) {
        cout
        << "c Verified "
        << longIrredCls.size() + longRedCls.size()
            + binTri.irredBins + binTri.redBins
        << " clause(s)."
        << endl;
    }

    return verificationOK;
}

size_t Solver::get_num_nonfree_vars() const
{
    size_t nonfree = 0;
    if (decisionLevel() == 0) {
        nonfree += trail.size();
    } else {
        nonfree += trail_lim[0];
    }

    if (occsimplifier) {
        if (conf.perform_occur_based_simp) {
            nonfree += occsimplifier->get_num_elimed_vars();
        }
    }
    nonfree += varReplacer->get_num_replaced_vars();


    if (compHandler) {
        nonfree += compHandler->get_num_vars_removed();
    }
    return nonfree;
}

size_t Solver::get_num_free_vars() const
{
    return nVarsOuter() - get_num_nonfree_vars();
}

void Solver::print_clause_stats() const
{
    //Irredundant
    print_value_kilo_mega(longIrredCls.size());
    print_value_kilo_mega(binTri.irredBins);
    cout
    << " " << std::setw(7) << std::fixed << std::setprecision(2)
    << ratio_for_stat(litStats.irredLits, longIrredCls.size())
    << " " << std::setw(7) << std::fixed << std::setprecision(2)
    << ratio_for_stat(litStats.irredLits + binTri.irredBins*2
    , longIrredCls.size() + binTri.irredBins)
    ;

    //Redundant
    size_t tot = 0;
    for(auto& lredcls: longRedCls) {
        print_value_kilo_mega(lredcls.size());
        tot += lredcls.size();
    }

    print_value_kilo_mega(binTri.redBins);
    cout
    << " " << std::setw(7) << std::fixed << std::setprecision(2)
    << ratio_for_stat(litStats.redLits, tot)
    << " " << std::setw(7) << std::fixed << std::setprecision(2)
    << ratio_for_stat(litStats.redLits + binTri.redBins*2
    , tot + binTri.redBins)
    ;
}

const char* Solver::get_version_sha1()
{
    return ::get_version_sha1();
}

const char* Solver::get_version_tag()
{
    return ::get_version_tag();
}

const char* Solver::get_compilation_env()
{
    return ::get_compilation_env();
}

void Solver::print_watch_list(watch_subarray_const ws, const Lit lit) const
{
    for (const Watched *it = ws.begin(), *end = ws.end()
        ; it != end
        ; ++it
    ) {
        if (it->isClause()) {
            cout
            << "Clause: " << *cl_alloc.ptr(it->get_offset());
        }

        if (it->isBin()) {
            cout
            << "BIN: " << lit << ", " << it->lit2()
            << " (l: " << it->red() << ")";
        }

        cout << endl;
    }
    cout << endl;
}

void Solver::check_implicit_propagated() const
{
    const double myTime = cpuTime();
    size_t wsLit = 0;
    for(watch_array::const_iterator
        it = watches.begin(), end = watches.end()
        ; it != end
        ; ++it, wsLit++
    ) {
        const Lit lit = Lit::toLit(wsLit);
        watch_subarray_const ws = *it;
        for(const Watched *it2 = ws.begin(), *end2 = ws.end()
            ; it2 != end2
            ; it2++
        ) {
            //Satisfied, or not implicit, skip
            if (value(lit) == l_True
                || it2->isClause()
            ) {
                continue;
            }

            const lbool val1 = value(lit);
            const lbool val2 = value(it2->lit2());

            //Handle binary
            if (it2->isBin()) {
                if (val1 == l_False) {
                    if (val2 != l_True) {
                        cout << "not prop BIN: "
                        << lit << ", " << it2->lit2()
                        << " (red: " << it2->red()
                        << endl;
                    }
                    assert(val2 == l_True);
                }

                if (val2 == l_False)
                    assert(val1 == l_True);
            }
        }
    }
    const double time_used = cpuTime() - myTime;
    if (sqlStats) {
        sqlStats->time_passed_min(
            this
            , "check implicit propagated"
            , time_used
        );
    }
}

size_t Solver::get_num_vars_elimed() const
{
    if (conf.perform_occur_based_simp) {
        return occsimplifier->get_num_elimed_vars();
    } else {
        return 0;
    }
}

void Solver::free_unused_watches()
{
    size_t wsLit = 0;
    for (watch_array::iterator
        it = watches.begin(), end = watches.end()
        ; it != end
        ; ++it, wsLit++
    ) {
        Lit lit = Lit::toLit(wsLit);
        if (varData[lit.var()].removed == Removed::elimed
            || varData[lit.var()].removed == Removed::replaced
            || varData[lit.var()].removed == Removed::decomposed
        ) {
            watch_subarray ws = *it;
            assert(ws.empty());
            ws.clear();
        }
    }

    consolidate_watches();
}

bool Solver::fully_enqueue_these(const vector<Lit>& toEnqueue)
{
    assert(ok);
    assert(decisionLevel() == 0);
    for(const Lit lit: toEnqueue) {
        if (!fully_enqueue_this(lit)) {
            return false;
        }
    }

    return true;
}

bool Solver::fully_enqueue_this(const Lit lit)
{
    const lbool val = value(lit);
    if (val == l_Undef) {
        assert(varData[lit.var()].removed == Removed::none);
        enqueue(lit);
        ok = propagate<true>().isNULL();

        if (!ok) {
            return false;
        }
    } else if (val == l_False) {
        ok = false;
        return false;
    }
    return true;
}

void Solver::new_external_var()
{
    new_var(false);
}

void Solver::new_external_vars(size_t n)
{
    new_vars(n);
}

void Solver::add_in_partial_solving_stats()
{
    Searcher::add_in_partial_solving_stats();
    sumSearchStats += Searcher::get_stats();
    sumPropStats += propStats;
}

unsigned long Solver::get_sql_id() const
{
    if (sqlStats == NULL) {
        return 0;
    }

    return sqlStats->get_runID();
}

bool Solver::add_clause_outer(const vector<Lit>& lits, bool red)
{
    if (!ok) {
        return false;
    }
    check_too_large_variable_number(lits);
    back_number_from_outside_to_outer(lits);
    return addClause(back_number_from_outside_to_outer_tmp, red);
}

bool Solver::add_xor_clause_outer(const vector<uint32_t>& vars, bool rhs)
{
    if (!ok) {
        return false;
    }

    vector<Lit> lits(vars.size());
    for(size_t i = 0; i < vars.size(); i++) {
        lits[i] = Lit(vars[i], false);
    }
    check_too_large_variable_number(lits);

    back_number_from_outside_to_outer(lits);
    addClauseHelper(back_number_from_outside_to_outer_tmp);
    add_xor_clause_inter(back_number_from_outside_to_outer_tmp, rhs, true, false);

    return ok;
}

void Solver::check_too_large_variable_number(const vector<Lit>& lits) const
{
    for (const Lit lit: lits) {
        if (lit.var() >= nVarsOutside()) {
            std::cerr
            << "ERROR: Variable " << lit.var() + 1
            << " inserted, but max var is "
            << nVarsOutside()
            << endl;
            assert(false);
            std::exit(-1);
        }
        release_assert(lit.var() < nVarsOutside()
        && "Clause inserted, but variable inside has not been declared with PropEngine::new_var() !");

        if (lit.var() >= var_Undef) {
            std::cerr << "ERROR: Variable number " << lit.var()
            << "too large. PropBy is limiting us, sorry" << endl;
            assert(false);
            std::exit(-1);
        }
    }
}

void Solver::bva_changed()
{
    datasync->rebuild_bva_map();
}

void Solver::open_file_and_dump_irred_clauses(string fname) const
{
    ClauseDumper dumper(this);
    dumper.open_file_and_dump_irred_clauses(fname);
}

void Solver::open_file_and_dump_red_clauses(string fname) const
{
    ClauseDumper dumper(this);
    dumper.open_file_and_dump_red_clauses(fname);
}

vector<pair<Lit, Lit> > Solver::get_all_binary_xors() const
{
    vector<pair<Lit, Lit> > bin_xors = varReplacer->get_all_binary_xors_outer();

    //Update to outer without BVA
    vector<pair<Lit, Lit> > ret;
    const vector<uint32_t> my_map = build_outer_to_without_bva_map();
    for(std::pair<Lit, Lit> p: bin_xors) {
        if (p.first.var() < my_map.size()
            && p.second.var() < my_map.size()
        ) {
            ret.push_back(std::make_pair(
                getUpdatedLit(p.first, my_map)
                , getUpdatedLit(p.second, my_map)
            ));
        }
    }

    for(const std::pair<Lit, Lit> val: ret) {
        assert(val.first.var() < nVarsOutside());
        assert(val.second.var() < nVarsOutside());
    }

    return ret;
}

void Solver::update_assumptions_after_varreplace()
{
    //Update assumptions
    for(AssumptionPair& lit_pair: assumptions) {
        if (assumptionsSet.size() > lit_pair.lit_inter.var()) {
            assumptionsSet[lit_pair.lit_inter.var()] = false;
        } else {
            assert(value(lit_pair.lit_inter) != l_Undef
                && "There can be NO other reason -- vars in assumptions cannot be elimed or decomposed");
        }

        lit_pair.lit_inter = varReplacer->get_lit_replaced_with(lit_pair.lit_inter);

        if (assumptionsSet.size() > lit_pair.lit_inter.var()) {
            assumptionsSet[lit_pair.lit_inter.var()] = true;
        }
    }
}

//TODO later, this can be removed, get_num_free_vars() is MUCH cheaper to
//compute but may have some bugs here-and-there
uint32_t Solver::num_active_vars() const
{
    uint32_t numActive = 0;
    uint32_t removed_decomposed = 0;
    uint32_t removed_replaced = 0;
    uint32_t removed_set = 0;
    uint32_t removed_elimed = 0;
    uint32_t removed_non_decision = 0;
    for(uint32_t var = 0; var < nVarsOuter(); var++) {
        if (value(var) != l_Undef) {
            if (varData[var].removed != Removed::none)
            {
                cout << "ERROR: var " << var + 1 << " has removed: "
                << removed_type_to_string(varData[var].removed)
                << " but is set to " << value(var) << endl;
                assert(varData[var].removed == Removed::none);
                exit(-1);
            }
            removed_set++;
            continue;
        }
        switch(varData[var].removed) {
            case Removed::decomposed :
                removed_decomposed++;
                continue;
            case Removed::elimed :
                removed_elimed++;
                continue;
            case Removed::replaced:
                removed_replaced++;
                continue;
            case Removed::none:
                break;
        }
        if (varData[var].removed != Removed::none) {
            removed_non_decision++;
        }
        numActive++;
    }
    assert(removed_non_decision == 0);
    if (occsimplifier) {
        assert(removed_elimed == occsimplifier->get_num_elimed_vars());
    } else {
        assert(removed_elimed == 0);
    }

    if (compHandler) {
        assert(removed_decomposed == compHandler->get_num_vars_removed());
    } else {
        assert(removed_decomposed == 0);
    }

    assert(removed_set == ((decisionLevel() == 0) ? trail.size() : trail_lim[0]));

    assert(removed_replaced == varReplacer->get_num_replaced_vars());
    assert(numActive == get_num_free_vars());

    return numActive;
}

SolveFeatures Solver::calculate_features() const
{
    SolveFeaturesCalc extract(this);
    SolveFeatures feat = extract.extract();
    feat.avg_confl_size = hist.conflSizeHistLT.avg();
    feat.avg_confl_glue = hist.glueHistLT.avg();
    feat.avg_num_resolutions = hist.numResolutionsHistLT.avg();
    feat.avg_trail_depth_delta = hist.trailDepthDeltaHist.avg();
    feat.avg_branch_depth = hist.branchDepthHist.avg();
    feat.avg_branch_depth_delta = hist.branchDepthDeltaHist.avg();

    feat.confl_size_min = hist.conflSizeHistLT.getMin();
    feat.confl_size_max = hist.conflSizeHistLT.getMax();
    feat.confl_glue_min = hist.glueHistLT.getMin();
    feat.confl_glue_max = hist.glueHistLT.getMax();
    feat.branch_depth_min = hist.branchDepthHist.getMin();
    feat.branch_depth_max = hist.branchDepthHist.getMax();
    feat.trail_depth_delta_min = hist.trailDepthDeltaHist.getMin();
    feat.trail_depth_delta_max = hist.trailDepthDeltaHist.getMax();
    feat.num_resolutions_min = hist.numResolutionsHistLT.getMin();
    feat.num_resolutions_max = hist.numResolutionsHistLT.getMax();

    if (sumPropStats.propagations != 0
        && sumConflicts != 0
        && sumSearchStats.numRestarts != 0
    ) {
        feat.props_per_confl = (double)sumConflicts / (double)sumPropStats.propagations;
        feat.confl_per_restart = (double)sumConflicts / (double)sumSearchStats.numRestarts;
        feat.decisions_per_conflict = (double)sumSearchStats.decisions / (double)sumConflicts;
        feat.learnt_bins_per_confl = (double)sumSearchStats.learntBins / (double)sumConflicts;
    }

    feat.num_gates_found_last = sumSearchStats.num_gates_found_last;
    feat.num_xors_found_last = sumSearchStats.num_xors_found_last;

    if (conf.verbosity) {
        feat.print_stats();
    }

    return feat;
}

void Solver::reconfigure(int val)
{
    //TODO adjust distill_queue_by !!

    assert(val > 0);
    switch (val) {
        case 3: {
            //Glue clause cleaning
            conf.every_lev1_reduce = 0;
            conf.every_lev2_reduce = 0;
            conf.glue_put_lev1_if_below_or_eq = 0;

            conf.adjust_glue_if_too_many_low = 0;
            conf.ratio_keep_clauses[clean_to_int(ClauseClean::activity)] = 0;
            conf.ratio_keep_clauses[clean_to_int(ClauseClean::glue)] = 0.5;
            conf.inc_max_temp_lev2_red_cls = 1.03;

            reset_temp_cl_num();
            break;
        }

        case 4: {
            conf.every_lev1_reduce = 0;
            conf.every_lev2_reduce = 0;
            conf.glue_put_lev1_if_below_or_eq = 0;
            conf.max_temp_lev2_learnt_clauses = 10000;
            reset_temp_cl_num();
            break;
        }

        case 6: {
            //No more simplifying
            conf.never_stop_search = true;
            break;
        }

        case 7: {
            //Geom restart, but keep low glue clauses
            conf.varElimRatioPerIter = 1;
            conf.restartType = Restart::geom;
            conf.polarity_mode = CMSat::PolarityMode::polarmode_neg;

            conf.every_lev1_reduce = 0;
            conf.every_lev2_reduce = 0;
            conf.glue_put_lev1_if_below_or_eq = 0;
            conf.inc_max_temp_lev2_red_cls = 1.02;

            reset_temp_cl_num();
            break;
        }

        case 12: {
            //Mix of keeping clauses
            conf.do_bva = false;
            conf.varElimRatioPerIter = 1;
            conf.every_lev1_reduce = 0;
            conf.every_lev2_reduce = 0;
            conf.glue_put_lev1_if_below_or_eq = 0;

            conf.glue_put_lev0_if_below_or_eq = 2;
            conf.glue_put_lev1_if_below_or_eq = 4;
            conf.ratio_keep_clauses[clean_to_int(ClauseClean::glue)] = 0.1;
            conf.ratio_keep_clauses[clean_to_int(ClauseClean::activity)] = 0.3;
            conf.inc_max_temp_lev2_red_cls = 1.04;

            conf.var_decay_max = 0.90; //more 'slow' in adjusting activities
            update_var_decay();
            reset_temp_cl_num();
            break;
        }

        case 13: {
            conf.orig_global_timeout_multiplier = 5;
            conf.global_timeout_multiplier = conf.orig_global_timeout_multiplier;
            conf.global_multiplier_multiplier_max = 5;

            conf.num_conflicts_of_search_inc = 1.15;
            conf.more_red_minim_limit_cache = 1200;
            conf.more_red_minim_limit_binary = 600;
            conf.max_num_lits_more_red_min = 20;
            conf.max_temp_lev2_learnt_clauses = 10000;
            conf.var_decay_max = 0.99; //more 'fast' in adjusting activities
            update_var_decay();
            break;
        }

        case 14: {
            conf.shortTermHistorySize = 600;
            break;
        }

        case 15: {
            //Like OLD-OLD minisat
            conf.varElimRatioPerIter = 1;
            conf.restartType = Restart::geom;
            conf.polarity_mode = CMSat::PolarityMode::polarmode_neg;

            conf.every_lev1_reduce = 0;
            conf.every_lev2_reduce = 0;
            conf.glue_put_lev1_if_below_or_eq = 0;
            conf.glue_put_lev0_if_below_or_eq = 0;
            conf.inc_max_temp_lev2_red_cls = 1.02;

            conf.update_glues_on_prop = 0;
            conf.update_glues_on_analyze = 0;
            conf.ratio_keep_clauses[clean_to_int(ClauseClean::glue)] = 0;
            conf.ratio_keep_clauses[clean_to_int(ClauseClean::activity)] = 0.5;
            reset_temp_cl_num();
            break;
        }

        default: {
            cout << "ERROR: You must give a value for reconfigure that is lower" << endl;
            exit(-1);
        }
    }

    if (conf.verbosity) {
        cout << "c [features] reconfigured solver to config " << val << endl;
    }

    /*Note to self: change
     * inc_max_temp_red_cls 1.1 -> 1.3
     * numCleanBetweenSimplify 2->4
     * bva: 1->0
    */
}

void Solver::save_state(const string& fname, const lbool status) const
{
    SimpleOutFile f;
    f.start(fname);

    f.put_lbool(status);
    Searcher::save_state(f, status);
    //f.put_struct(sumStats);
    //f.put_struct(sumPropStats);
    //f.put_vector(outside_assumptions);

    varReplacer->save_state(f);
    if (occsimplifier) {
        occsimplifier->save_state(f);
    }
}

lbool Solver::load_state(const string& fname)
{
    SimpleInFile f;
    f.start(fname);

    const lbool status = f.get_lbool();
    Searcher::load_state(f, status);
    //f.get_struct(sumStats);
    //f.get_struct(sumPropStats);
    //f.get_vector(outside_assumptions);

    varReplacer->load_state(f);
    if (occsimplifier) {
        occsimplifier->load_state(f);
    }

    return status;
}

lbool Solver::load_solution_from_file(const string& fname)
{
    //At this point, model is set up, we just need to fill the l_Undef in
    //from assigns
    lbool status = l_Undef;
    FILE* input_stream = fopen(fname.c_str(), "r");
    if (input_stream == NULL) {
        std::cerr << "ERROR: could not open solution file "
        << fname
        << endl;
        std::exit(-1);
    }
    StreamBuffer<FILE*, FN> in(input_stream);

    unsigned lineNum = 0;
    std::string str;
    for (;;) {
        in.skipWhitespace();
        switch (*in) {
            case EOF:
                goto end;
            case 's': {
                ++in;
                in.skipWhitespace();
                in.parseString(str);
                if (str == "SATISFIABLE") {
                    status = l_True;
                } else if (str == "UNSATISFIABLE") {
                    status = l_False;
                    goto end;
                } else if (str == "INDETERMINATE") {
                    status = l_Undef;
                    goto end;
                } else {
                    std::cerr << "ERROR: Cannot parse solution line startig with 's'"
                    << endl;
                    std::exit(-1);
                }
                status = l_True;
                in.skipLine();
                lineNum++;
                break;
            }
            case 'v': {
                ++in;
                parse_v_line(&in, lineNum);
                in.skipLine();
                lineNum++;
                break;
            }
            case '\n':
                std::cerr
                << "c WARNING: Empty line at line number " << lineNum+1
                << " -- this is not part of the DIMACS specifications. Ignoring."
                << endl;
                in.skipLine();
                lineNum++;
                break;
            default:
                in.skipLine();
                lineNum++;
                break;
        }
    }

    end:
    fclose(input_stream);
    return status;
}

template<typename A>
void Solver::parse_v_line(A* in, const size_t lineNum)
{
    model.resize(nVarsOuter(), l_Undef);

    int32_t parsed_lit;
    uint32_t var;
    for (;;) {
        if (!in->parseInt(parsed_lit, lineNum, true)) {
            exit(-1);
        }
        if (parsed_lit == std::numeric_limits<int32_t>::max()) {
            break;
        }
        if (parsed_lit == 0) break;
        var = abs(parsed_lit)-1;
        if (var >= nVars()) {
            std::cerr
            << "ERROR! "
            << "Variable in solution is too large: " << var << endl
            << "--> At line " << lineNum+1
            << endl;
            std::exit(-1);
        }

        //Don't overwrite previously computed values
        if (model[var] == l_Undef
            && varData[var].removed == Removed::none
        ) {
            model[var] = parsed_lit < 0 ? l_False : l_True;
            if (conf.verbosity >= 10) {
                uint32_t outer_var = map_inter_to_outer(var);
                cout << "Read V line: model for inter var " << (var+1)
                << " (outer ver for this is: " << outer_var+1 << ")"
                << " set to " << model[var] << endl;
            }
        }
    }
}


void Solver::check_implicit_stats(const bool onlypairs) const
{
    //Don't check if in crazy mode
    #ifdef NDEBUG
    return;
    #endif
    const double myTime = cpuTime();

    //Check number of red & irred binary clauses
    uint64_t thisNumRedBins = 0;
    uint64_t thisNumIrredBins = 0;

    size_t wsLit = 0;
    for(watch_array::const_iterator
        it = watches.begin(), end = watches.end()
        ; it != end
        ; ++it, wsLit++
    ) {
        watch_subarray_const ws = *it;
        for(const Watched* it2 = ws.begin(), *end2 = ws.end()
            ; it2 != end2
            ; it2++
        ) {
            if (it2->isBin()) {
                #ifdef DEBUG_IMPLICIT_PAIRS_TRIPLETS
                Lit lits[2];
                lits[0] = Lit::toLit(wsLit);
                lits[1] = it2->lit2();
                std::sort(lits, lits + 2);
                findWatchedOfBin(watches, lits[0], lits[1], it2->red());
                findWatchedOfBin(watches, lits[1], lits[0], it2->red());
                #endif

                if (it2->red())
                    thisNumRedBins++;
                else
                    thisNumIrredBins++;

                continue;
            }
        }
    }

    if (onlypairs) {
        goto end;
    }

    if (thisNumIrredBins/2 != binTri.irredBins) {
        std::cerr
        << "ERROR:"
        << " thisNumIrredBins/2: " << thisNumIrredBins/2
        << " thisNumIrredBins: " << thisNumIrredBins
        << " binTri.irredBins: " << binTri.irredBins
        << endl;
    }
    assert(thisNumIrredBins % 2 == 0);
    assert(thisNumIrredBins/2 == binTri.irredBins);

    if (thisNumRedBins/2 != binTri.redBins) {
        std::cerr
        << "ERROR:"
        << " thisNumRedBins/2: " << thisNumRedBins/2
        << " thisNumRedBins: " << thisNumRedBins
        << " binTri.redBins: " << binTri.redBins
        << endl;
    }
    assert(thisNumRedBins % 2 == 0);
    assert(thisNumRedBins/2 == binTri.redBins);

    end:

    const double time_used = cpuTime() - myTime;
    if (sqlStats) {
        sqlStats->time_passed_min(
            this
            , "check implicit stats"
            , time_used
        );
    }
}

void Solver::check_stats(const bool allowFreed) const
{
    //If in crazy mode, don't check
    #ifdef NDEBUG
    return;
    #endif

    check_implicit_stats();

    const double myTime = cpuTime();
    uint64_t numLitsIrred = count_lits(longIrredCls, false, allowFreed);
    if (numLitsIrred != litStats.irredLits) {
        std::cerr << "ERROR: " << endl
        << "->numLitsIrred: " << numLitsIrred << endl
        << "->litStats.irredLits: " << litStats.irredLits << endl;
    }

    uint64_t numLitsRed = 0;
    for(auto& lredcls: longRedCls) {
        numLitsRed += count_lits(lredcls, true, allowFreed);
    }
    if (numLitsRed != litStats.redLits) {
        std::cerr << "ERROR: " << endl
        << "->numLitsRed: " << numLitsRed << endl
        << "->litStats.redLits: " << litStats.redLits << endl;
    }
    assert(numLitsRed == litStats.redLits);
    assert(numLitsIrred == litStats.irredLits);

    const double time_used = cpuTime() - myTime;
    if (sqlStats) {
        sqlStats->time_passed_min(
            this
            , "check literal stats"
            , time_used
        );
    }
}

void Solver::add_sql_tag(const string& tagname, const string& tag)
{
    if (sqlStats) {
        sqlStats->add_tag(std::make_pair(tagname, tag));
    }
}

uint32_t Solver::undefine(vector<uint32_t>& trail_lim_vars)
{
    undef = new FindUndef;
    undef->trail_lim_vars = &trail_lim_vars;
    undef->can_be_unsetSum = 0;
    undef->num_fixed = 0;
    undef->verbose = conf.verbosity > 15;

    undef->dontLookAtClause.clear();
    undef->dontLookAtClause.resize(longIrredCls.size(), false);
    undef->can_be_unset.clear();
    undef->can_be_unset.resize(nVarsOuter(), 0);
    undef->satisfies.clear();
    undef->satisfies.resize(nVarsOuter(), 0);

    undef_fill_potentials();

    if (undef->verbose) {
        cout << "NUM Can be unset: " << undef->can_be_unsetSum << endl;
        cout << "--" << endl;
        for(size_t i = 0; i < undef->can_be_unset.size(); i++) {
            if (undef->can_be_unset[i]) {
                cout << "Can be unset var  " << i+1 << endl;
            }
        }
        cout << "--" << endl;
    }

    while(undef_check_must_fix()
        && undef->can_be_unsetSum > 0
    ) {
        //Find variable to fix.
        uint32_t maximum = 0;
        uint32_t v = var_Undef;
        for (uint32_t i = 0; i < undef->can_be_unset.size(); i++) {
            if (undef->can_be_unset[i]) {

                if (undef->verbose) {
                    cout << "Var " << i+1 << " can be fixed"
                    << ", it satisfies: " << undef->satisfies[i] << " clauses" << endl;
                }
            }
            if (undef->can_be_unset[i] && undef->satisfies[i] >= maximum) {
                maximum = undef->satisfies[i];
                v = i;
            }
        }
        if (undef->verbose) cout << "--" << endl;
        assert(v != var_Undef);

        //Fix 'v' to be set to curent value
        assert(undef->can_be_unset[v]);
        undef->can_be_unset[v] = 0;
        undef->can_be_unsetSum--;
        undef->num_fixed++;

        if (undef->verbose) cout << "Fixed var " << v+1 << endl;

        std::fill(undef->satisfies.begin(), undef->satisfies.end(), 0);
    }

    //Everything that hasn't been fixed and can be unset, is now unset
    undef_unset_potentials();

    int toret = undef->can_be_unsetSum;
    delete undef;
    return toret;
}

void Solver::undef_fill_potentials()
{
    int trail_at = ((int)undef->trail_lim_vars->size())-1;
    if (undef->verbose) cout << "trail_at: " << trail_at << endl;

    //Mark everything on the trail except at lev 0
    while(trail_at >= 0) {
        uint32_t v = (*undef->trail_lim_vars)[trail_at];
        if (undef->verbose) cout << "Examining trail var: " << v+1 << endl;

        assert(varData[v].removed == Removed::none);
        assert(assumptionsSet.size() > v);
        if (model_value(v) != l_Undef
            && assumptionsSet[v] == false
        ) {
            undef->can_be_unset[v] += 1;
            if (conf.independent_vars == NULL) {
                undef->can_be_unsetSum++;
            }
        }

        trail_at--;
    }
    if (undef->verbose) {
        cout << "-" << endl;
    }

    if (conf.independent_vars) {
        for(uint32_t v: *conf.independent_vars) {
            if (v > nVarsOutside()) {
                cout << "ERROR: Variabe in independent set, " << v+1
                << " is bigger than any variable inside the solver! " << endl
                << " Please examine the call set_independent_vars or the CNF"
                " lines starting with 'c ind'"
                << endl;
                std::exit(-1);
            }

            v = map_to_with_bva(v);
            v = map_outer_to_inter(v);
            if (v < nVars()) {
                undef->can_be_unset[v]++;
                if (undef->can_be_unset[v] == 2) {
                    undef->can_be_unsetSum++;
                }
            }
        }
    }

    if (conf.independent_vars) {
        //Only those with a setting of both independent_vars and in trail
        //can be unset
        for(unsigned char& v: undef->can_be_unset) {
            if (v < 2) {
                v = false;
            }
        }
    }

    //Mark variables replacing others as non-eligible
    vector<uint32_t> replacingVars = varReplacer->get_vars_replacing_others();
    for (const uint32_t v: replacingVars) {
        if (undef->can_be_unset[v]) {
            undef->can_be_unset[v] = false;
            undef->can_be_unsetSum--;
        }
    }
}

void Solver::undef_unset_potentials()
{
    for (uint32_t i = 0; i < undef->can_be_unset.size(); i++) {
        if (undef->can_be_unset[i]) {
            model[i] = l_Undef;
            if (undef->verbose) cout << "Unset variable " << i << endl;
        }
    }
}

template<class C>
bool Solver::undef_look_at_one_clause(const C c)
{
    if (undef->verbose) {
        cout << "Check called on clause: ";
        for(Lit l: *c) {
            cout << l << " ";
            assert(l.var() < model.size());
        }
        cout << endl;
    }

    uint32_t v = var_Undef;
    uint32_t numTrue = 0;
    for (const Lit l: *c) {
        if (model_value(l) == l_True) {
            if (undef->can_be_unset[l.var()]) {
                numTrue ++;
                v = l.var();
            } else {
                return true;
            }
        }
    }

    //Greedy
    if (numTrue == 1) {
        assert(v != var_Undef);
        assert(undef->can_be_unset[v]);

        undef->can_be_unset[v] = false;
        if (undef->verbose) cout << "Setting " << v+1 << " as fixed" << endl;
        undef->can_be_unsetSum--;
        //clause definitely satisfied
        return true;
    }

    //numTrue > 1
    undef->must_fix = true;
    assert(numTrue > 1);
    for (const Lit l: *c) {
        if (model_value(l) == l_True)
            undef->satisfies[l.var()]++;
    }

    //Clause is not definitely satisfied
    return false;
}

bool Solver::undef_check_must_fix()
{
    undef->must_fix = false;

    for (uint32_t i = 0
         ; i < longIrredCls.size()
         ; i++
    ) {
        if (undef->dontLookAtClause[i])
            continue;

        Clause* c = cl_alloc.ptr(longIrredCls[i]);
        if (undef_look_at_one_clause(c)) {
            //clause definitely satisfied
            undef->dontLookAtClause[i] = true;
        }
    }

    for(size_t i = 0; i < nVars()*2; i++) {
        const Lit l = Lit::toLit(i);
        if (!undef->can_be_unset[l.var()]
            && model_value(l) == l_True
        ) {
            continue;
        }
        for(const Watched& w: watches[l]) {
            if (w.isBin()
                && l < w.lit2()
            ) {
                std::array<Lit, 2> c;
                c[0] = l;
                c[1] = w.lit2();
                undef_look_at_one_clause(&c);
            }
        }
    }

    //There is hope
    return undef->must_fix;
}
