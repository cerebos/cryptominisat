/**************************************************************************
CryptoMiniSat -- Copyright (c) 2009 Mate Soos

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "FailedLitSearcher.h"

#include <iomanip>
#include <utility>
#include <set>
using std::make_pair;
using std::set;

#include "Solver.h"
#include "ClauseCleaner.h"
#include "time_mem.h"
#include "VarReplacer.h"
#include "ClauseCleaner.h"
#include "StateSaver.h"
#include "CompleteDetachReattacher.h"
#include "PartHandler.h"
#include "BothCache.h"

#ifdef _MSC_VER
#define __builtin_prefetch(a,b,c)
#endif //_MSC_VER

//#define VERBOSE_DEUBUG

/**
@brief Sets up variables that are used between calls to search()
*/
FailedLitSearcher::FailedLitSearcher(Solver& _solver):
    solver(_solver)
    , tmpPs(2)
    , totalTime(0)
    , numPropsMultiplier(1.0)
    , lastTimeFoundTruths(0)
    , numCalls(0)
{
    lastTimeStopped = solver.mtrand.randInt(solver.nVars());
}

/**
@brief Initialises datastructures for 2-long xor finding by shortening longer xors
*/
void FailedLitSearcher::addFromSolver(const vec< XorClause* >& cs)
{
    xorClauseSizes.clear();
    xorClauseSizes.growTo(cs.size());
    occur.resize(solver.nVars());
    for (Var var = 0; var < solver.nVars(); var++) {
        occur[var].clear();
    }

    uint32_t i = 0;
    for (XorClause * const*it = cs.getData(), * const*end = it + cs.size(); it !=  end; it++, i++) {
        if (it+1 != end)
            __builtin_prefetch(*(it+1), 0, 0);

        const XorClause& cl = **it;
        xorClauseSizes[i] = cl.size();
        for (const Lit *l = cl.getData(), *end2 = l + cl.size(); l != end2; l++) {
            occur[l->var()].push_back(i);
        }
    }
}

/**
@brief Remove the assinged vars from the xors added by addFromSolver()

The thus shortened xors are then treated if they are 2-long and if they
appear twice: by propagating "var" and by propagating "~var"
*/
inline void FailedLitSearcher::removeVarFromXors(const Var var)
{
    vector<uint32_t>& occ = occur[var];
    if (occ.empty()) return;

    for (uint32_t *it = &occ[0], *end = it + occ.size(); it != end; it++) {
        xorClauseSizes[*it]--;
        if (!xorClauseTouched[*it]) {
            xorClauseTouched.setBit(*it);
            investigateXor.push(*it);
        }
    }
}

/**
@brief Undoes what removeVarFromXors() has done
*/
inline void FailedLitSearcher::addVarFromXors(const Var var)
{
    vector<uint32_t>& occ = occur[var];
    if (occ.empty()) return;

    for (uint32_t *it = &occ[0], *end = it + occ.size(); it != end; it++) {
        xorClauseSizes[*it]++;
    }
}

/**
@brief Returns the 2-long xor clause that has been made of the longer xor-clause under current assignement

We KNOW that the xorclause "c" passed as a parameter must be 2-long. We just
need it so that we can work with it. We KNOW it's 2-long because of the
data structures and functions in place

@p[in] c MUST be a 2-long xor clause under current assignement
*/
const FailedLitSearcher::TwoLongXor FailedLitSearcher::getTwoLongXor(const XorClause& c)
{
    TwoLongXor tmp;
    uint32_t num = 0;
    tmp.inverted = c.xorEqualFalse();

    for(const Lit *l = c.getData(), *end = l + c.size(); l != end; l++) {
        if (solver.assigns[l->var()] == l_Undef) {
            assert(num < 2);
            tmp.var[num] = l->var();
            num++;
        } else {
            tmp.inverted ^= (solver.assigns[l->var()] == l_True);
        }
    }

    #ifdef VERBOSE_DEUBUG
    if (num != 2) {
        std::cout << "Num:" << num << std::endl;
        c.plainPrint();
    }
    #endif

    std::sort(&tmp.var[0], &tmp.var[0]+2);
    assert(num == 2);
    return tmp;
}

/**
@brief The main function. Initialises data and calls tryBoth() for heavy-lifting

It sets up the ground for tryBoth() and calls it as many times as it sees fit.
One afther the other, the different optimisations' data structures are
initialised, and their limits are set. Then tryBoth is called in two different
forms: somewhat sequentially on varaibles x...z and then on randomly picked
variables.
*/
const bool FailedLitSearcher::search()
{
    assert(solver.decisionLevel() == 0);
    if (solver.nVars() == 0) return solver.ok;
    if (solver.conf.doCacheOTFSSR && numCalls > 0) {
        BothCache bothCache(solver);
        if (!bothCache.tryBoth()) return false;
    }

    uint64_t numProps = 30 * 1000000;
    uint64_t numPropsDifferent = (double)numProps*2.0;

    solver.testAllClauseAttach();
    double myTime = cpuTime();
    uint32_t origHeapSize = solver.order_heap.size();
    StateSaver savedState(solver);
    Heap<Solver::VarOrderLt> order_heap_copy(solver.order_heap); //for hyperbin

    //General Stats
    numFailed = 0;
    goodBothSame = 0;
    numCalls++;

    //If failed var searching is going good, do successively more and more of it
    if ((double)lastTimeFoundTruths > (double)solver.order_heap.size() * 0.10) numPropsMultiplier = std::max(numPropsMultiplier*1.3, 2.0);
    else numPropsMultiplier = 1.0;
    numProps = (uint64_t) ((double)numProps * numPropsMultiplier * solver.conf.failedLitMultiplier);

    //For BothSame
    propagated.resize(solver.nVars(), 0);
    propValue.resize(solver.nVars(), 0);

    //For calculating how many variables have really been set
    origTrailSize = solver.trail.size();

    //For 2-long xor (rule 6 of  Equivalent literal propagation in the DLL procedure by Chu-Min Li)
    toReplaceBefore = solver.varReplacer->getNewToReplaceVars();
    lastTrailSize = solver.trail.size();
    binXorFind = true;
    twoLongXors.clear();
    if (solver.xorclauses.size() < 5 ||
        solver.xorclauses.size() > 30000 ||
        solver.order_heap.size() > 30000 ||
        solver.nClauses() > 100000)
        binXorFind = false;
    if (binXorFind) {
        solver.clauseCleaner->cleanClauses(solver.xorclauses, ClauseCleaner::xorclauses);
        addFromSolver(solver.xorclauses);
    }
    xorClauseTouched.resize(solver.xorclauses.size(), 0);
    newBinXor = 0;

    //For 2-long xor through Le Berre paper
    bothInvert = 0;

    //For HyperBin
    addedBin = 0;
    addedCacheBin = 0;
    unPropagatedBin.resize(solver.nVars(), 0);
    needToVisit.resize(solver.nVars(), 0);
    dontRemoveAncestor.resize(solver.nVars(), 0);
    hyperbinProps = 0;
    maxHyperBinProps = numProps/4;
    removedUselessLearnt = 0;
    removedUselessNonLearnt = 0;

    //uint32_t fromBin;
    origBogoProps = solver.bogoProps;
    uint32_t i;
    for (i = 0; i < solver.nVars(); i++) {
        Var var = (lastTimeStopped + i) % solver.nVars();
        if (solver.assigns[var] != l_Undef || !solver.decision_var[var])
            continue;
        if (solver.bogoProps >= origBogoProps + numProps)
            break;
        if (!tryBoth(Lit(var, false), Lit(var, true)))
            goto end;
    }
    lastTimeStopped = (lastTimeStopped + i) % solver.nVars();

    calcNegPosDist();

    origBogoProps = solver.bogoProps;
    hyperbinProps = 0;
    for (uint32_t i = 0; i < solver.negPosDist.size(); i++) {
        Var var = solver.negPosDist[i].var;
        if (solver.assigns[var] != l_Undef || !solver.decision_var[var])
            continue;
        if (solver.bogoProps >= origBogoProps + numPropsDifferent)  {
            break;
        }
        if (!tryBoth(Lit(var, false), Lit(var, true)))
            goto end;
    }

    while (!order_heap_copy.empty()) {
        Var var = order_heap_copy.removeMin();
        if (solver.assigns[var] != l_Undef || !solver.decision_var[var])
            continue;
        if (solver.bogoProps >= origBogoProps + numPropsDifferent)  {
            break;
        }
        if (!tryBoth(Lit(var, false), Lit(var, true)))
            goto end;
    }
    //if (!tryMultiLevelAll()) goto end;

end:
    if (solver.conf.verbosity  >= 1) printResults(myTime);

    solver.order_heap.filter(Solver::VarFilter(solver));

    if (solver.ok && (numFailed || goodBothSame)) {
        double time = cpuTime();
        if ((int)origHeapSize - (int)solver.order_heap.size() >  (int)origHeapSize/15 && solver.nClauses() + solver.learnts.size() > 500000) {
            CompleteDetachReatacher reattacher(solver);
            reattacher.detachNonBinsNonTris(true);
            const bool ret = reattacher.reattachNonBins();
            release_assert(ret == true);
        } else {
            solver.clauseCleaner->removeAndCleanAll();
        }
        if (solver.conf.verbosity  >= 1 && numFailed + goodBothSame > 100) {
            std::cout << "c Cleaning up after failed var search: " << std::setw(8) << std::fixed << std::setprecision(2) << cpuTime() - time << " s "
            << std::endl;
        }
    }

    lastTimeFoundTruths = solver.trail.size() - origTrailSize;
    totalTime += cpuTime() - myTime;

    savedState.restore();

    solver.testAllClauseAttach();
    return solver.ok;
}


/**
@brief Prints results of failed litaral probing

Printed:
1) Num failed lits
2) Num lits that have been propagated by both "var" and "~var"
3) 2-long Xor clauses that have been found because when propagating "var" and
   "~var", they have been produced by normal xor-clauses shortening to this xor
   clause
4) If var1 propagates var2 and ~var1 propagates ~var2, then var=var2, and this
   is a 2-long XOR clause
5) Number of propagations
6) Time in seconds
*/
void FailedLitSearcher::printResults(const double myTime) const
{
    std::cout << "c Flit: "<< std::setw(5) << numFailed <<
    " Blit: " << std::setw(6) << goodBothSame <<
    " bXBeca: " << std::setw(4) << newBinXor <<
    " bXProp: " << std::setw(4) << bothInvert <<
    " Bin:"   << std::setw(7) << addedBin <<
    " BRemL:" << std::setw(7) << removedUselessLearnt <<
    " BRemN:" << std::setw(7) << removedUselessNonLearnt <<
    " CBin: " << std::setw(7) << addedCacheBin <<
    " P: " << std::setw(4) << std::fixed << std::setprecision(1) << (double)(solver.bogoProps - origBogoProps)/1000000.0  << "M"
    " T: " << std::setw(5) << std::fixed << std::setprecision(2) << cpuTime() - myTime
    << std::endl;
}

/**
@brief The main function of search() doing almost everything in this class

Tries to branch on both lit1 and lit2 and then both-propagates them, fail-lits
them, and hyper-bin resolves them, etc. It is imperative that from the
SAT point of view, EITHER lit1 or lit2 MUST hold. So, if lit1 = ~lit2, it's OK.
Also, if there is a binary clause 'lit1 or lit2' it's also OK.
*/
const bool FailedLitSearcher::tryBoth(const Lit lit1, const Lit lit2)
{
    if (binXorFind) {
        if (lastTrailSize < solver.trail.size()) {
            for (uint32_t i = lastTrailSize; i != solver.trail.size(); i++) {
                removeVarFromXors(solver.trail[i].var());
            }
        }
        lastTrailSize = solver.trail.size();
        xorClauseTouched.setZero();
        investigateXor.clear();
    }

    propagated.removeThese(propagatedBitSet);
    assert(propagated.isZero());
    propagatedBitSet.clear();
    twoLongXors.clear();
    bothSame.clear();
    binXorToAdd.clear();
    #ifdef DEBUG_HYPERBIN
    assert(propagatedVars.empty());
    assert(unPropagatedBin.isZero());
    assert(addBinLater.empty());
    #endif //DEBUG_HYPERBIN
    #ifdef DEBUG_USELESS_LEARNT_BIN_REMOVAL
    dontRemoveAncestor.isZero();
    assert(uselessBin.empty());
    #endif

    solver.newDecisionLevel();
    solver.uncheckedEnqueueLight(lit1);
    failed = (!solver.propagate<false>().isNULL());
    if (failed) {
        solver.cancelUntilLight();
        numFailed++;
        solver.uncheckedEnqueue(~lit1);
        solver.ok = (solver.propagate<true>().isNULL());
        if (!solver.ok) return false;
        return true;
    }

    assert(solver.decisionLevel() > 0);
    //vector<Lit> oldCache;
    vector<Lit> lit1OTFCache;
    for (int c = solver.trail.size()-1; c >= (int)solver.trail_lim[0]; c--) {
        Var x = solver.trail[c].var();
        propagated.setBit(x);
        propagatedBitSet.push_back(x);

        if (solver.conf.doHyperBinRes) {
            unPropagatedBin.setBit(x);
            propagatedVars.push(x);
        }

        if (solver.assigns[x].getBool()) propValue.setBit(x);
        else propValue.clearBit(x);

        if (binXorFind) removeVarFromXors(x);
        if (solver.conf.doCacheOTFSSR && c != (int)solver.trail_lim[0]) {
            lit1OTFCache.push_back(solver.trail[c]);
        }
    }
    if (solver.conf.doCacheOTFSSR) {
        solver.transOTFCache[(~lit1).toInt()].merge(lit1OTFCache, false, solver.seen);
        solver.transOTFCache[(~lit1).toInt()].conflictLastUpdated = solver.conflicts;
    }

    if (binXorFind) {
        for (uint32_t *it = investigateXor.getData(), *end = investigateXor.getDataEnd(); it != end; it++) {
            if (xorClauseSizes[*it] == 2)
                twoLongXors.insert(getTwoLongXor(*solver.xorclauses[*it]));
        }
        for (int c = solver.trail.size()-1; c >= (int)solver.trail_lim[0]; c--) {
            addVarFromXors(solver.trail[c].var());
        }
        xorClauseTouched.setZero();
        investigateXor.clear();
    }

    solver.cancelUntilLight();

    if (solver.conf.doHyperBinRes) hyperBinResAll(lit1/*, oldCache*/);
    //oldCache.clear();
    if (!performAddBinLaters()) return false;

    #ifdef DEBUG_HYPERBIN
    assert(propagatedVars.empty());
    assert(unPropagatedBin.isZero());
    assert(addBinLater.empty());
    #endif //DEBUG_HYPERBIN

    solver.newDecisionLevel();
    solver.uncheckedEnqueueLight(lit2);
    failed = (!solver.propagate<false>().isNULL());
    if (failed) {
        solver.cancelUntilLight();
        numFailed++;
        solver.uncheckedEnqueue(~lit2);
        solver.ok = (solver.propagate<true>().isNULL());
        if (!solver.ok) return false;
        return true;
    }

    assert(solver.decisionLevel() > 0);
    vector<Lit> lit2OTFCache;
    for (int c = solver.trail.size()-1; c >= (int)solver.trail_lim[0]; c--) {
        Var x  = solver.trail[c].var();
        if (propagated[x]) {
            if (propValue[x] == solver.assigns[x].getBool()) {
                //they both imply the same
                bothSame.push(Lit(x, !propValue[x]));
            } else if (c != (int)solver.trail_lim[0]) {
                assert(lit1.var() == lit2.var());
                tmpPs[0] = Lit(lit1.var(), false);
                tmpPs[1] = Lit(x, false);
                binXorToAdd.push_back(BinXorToAdd(tmpPs[0], tmpPs[1], propValue[x], 0));
                bothInvert += solver.varReplacer->getNewToReplaceVars() - toReplaceBefore;
                toReplaceBefore = solver.varReplacer->getNewToReplaceVars();
            }
        }

        if (solver.conf.doHyperBinRes) {
            unPropagatedBin.setBit(x);
            propagatedVars.push(x);
        }

        if (solver.assigns[x].getBool()) propValue.setBit(x);
        else propValue.clearBit(x);

        if (binXorFind) removeVarFromXors(x);
        if (solver.conf.doCacheOTFSSR && c != (int)solver.trail_lim[0]) {
            lit2OTFCache.push_back(solver.trail[c]);
        }
    }
    if (solver.conf.doCacheOTFSSR) {
        solver.transOTFCache[(~lit2).toInt()].merge(lit2OTFCache, false, solver.seen);
        solver.transOTFCache[(~lit2).toInt()].conflictLastUpdated = solver.conflicts;
    }

    //We now add the two-long xors that have been found through longer
    //xor-shortening
    if (binXorFind) {
        if (twoLongXors.size() > 0) {
            for (uint32_t *it = investigateXor.getData(), *end = it + investigateXor.size(); it != end; it++) {
                if (xorClauseSizes[*it] == 2) {
                    TwoLongXor tmp = getTwoLongXor(*solver.xorclauses[*it]);
                    if (twoLongXors.find(tmp) != twoLongXors.end()) {
                        tmpPs[0] = Lit(tmp.var[0], false);
                        tmpPs[1] = Lit(tmp.var[1], false);
                        binXorToAdd.push_back(BinXorToAdd(tmpPs[0], tmpPs[1], tmp.inverted, solver.xorclauses[*it]->getGroup()));
                        newBinXor += solver.varReplacer->getNewToReplaceVars() - toReplaceBefore;
                        toReplaceBefore = solver.varReplacer->getNewToReplaceVars();
                    }
                }
            }
        }
        for (int c = solver.trail.size()-1; c >= (int)solver.trail_lim[0]; c--) {
            addVarFromXors(solver.trail[c].var());
        }
    }
    solver.cancelUntilLight();

    if (solver.conf.doHyperBinRes) hyperBinResAll(lit2/*, oldCache*/);
    //oldCache.clear();
    if (!performAddBinLaters()) return false;

    for(uint32_t i = 0; i != bothSame.size(); i++) {
        solver.uncheckedEnqueue(bothSame[i]);
    }
    goodBothSame += bothSame.size();
    solver.ok = (solver.propagate<true>().isNULL());
    if (!solver.ok) return false;

    for (uint32_t i = 0; i < binXorToAdd.size(); i++) {
        tmpPs[0] = binXorToAdd[i].lit1;
        tmpPs[1] = binXorToAdd[i].lit2;
        solver.addXorClauseInt(tmpPs, binXorToAdd[i].isEqualFalse, binXorToAdd[i].group);
        tmpPs.clear();
        tmpPs.growTo(2);
        if (!solver.ok) return false;
    }

    return true;
}

const bool FailedLitSearcher::performAddBinLaters()
{
    assert(solver.ok);
    for (vector<std::pair<Lit, Lit> >::const_iterator it = addBinLater.begin(), end = addBinLater.end(); it != end; it++) {
        tmpPs[0] = it->first;
        tmpPs[1] = it->second;
        Clause *c = solver.addClauseInt(tmpPs, 0, true);
        release_assert(c == NULL);
        tmpPs.clear();
        tmpPs.growTo(2);
        if (!solver.ok) return false;
        addedCacheBin++;
    }
    addBinLater.clear();

    return true;
}

void FailedLitSearcher::hyperBinResAll(const Lit litProp/*, const vector<Lit>& oldCache*/)
{
    /*if (solver.conf.doAddBinCache) {
        //Not forgetting stuff already known at one point from cache
        for (vector<Lit>::const_iterator it = oldCache.begin(), end = oldCache.end(); it != end; it++) {
        const Lit lit = *it;
        if (solver.value(lit.var()) != l_Undef
            || solver.subsumer->getVarElimed()[lit.var()]
            || solver.xorSubsumer->getVarElimed()[lit.var()]
            || solver.varReplacer->getReplaceTable()[lit.var()].var() != lit.var()
            || solver.partHandler->getSavedState()[lit.var()] != l_Undef)
            continue;

            if (!unPropagatedBin[it->var()]) addBinLater.push_back(std::make_pair(~litProp, *it));
        }
    }*/

    //Hyper-binary resolution
    if (hyperbinProps < maxHyperBinProps) hyperBinResolution(litProp);
    unPropagatedBin.removeThese(propagatedVars);
    propagatedVars.clear();
}

/**
@brief Adds hyper-binary clauses

At this point, unPropagatedBin is set, and propagatedVars is filled with lits
that have been propagated. Here, we propagate ONLY at the binary level,
and compare with propagatedVars and unPropagatedBin. If they match, it's OK. If
not, then we add the relevant binary clauses at the right point. The "right"
point is the point which has the highest in-degree. We approximated the degrees
beforehand with orderLits()
*/
void FailedLitSearcher::hyperBinResolution(const Lit lit)
{
    #ifdef VERBOSE_DEBUG
    std::cout << "Checking one BTC vs UP" << std::endl;
    #endif //VERBOSE_DEBUG

    #ifdef DEBUG_HYPERBIN
    assert(needToVisit.isZero());
    #endif //DEBUG_HYPERBIN

    uint64_t oldBogoProps = solver.bogoProps;
    vec<Lit> toVisit;
    uint64_t extraTime = 0;

    solver.newDecisionLevel();
    solver.uncheckedEnqueueLight2(lit, 0, lit_Undef, false);
    failed = (!solver.propagateBin(uselessBin).isNULL());
    assert(!failed);

    if (solver.conf.doRemUselessLBins && !uselessBin.empty()) {
        for (const Lit *it = uselessBin.getData(), *end = uselessBin.getDataEnd(); it != end; it++) {
            if (dontRemoveAncestor[it->var()]) continue;

            extraTime += solver.watches[lit.toInt()].size()/2;
            extraTime += solver.watches[(~*it).toInt()].size()/2;
            if (findWBin(solver.watches, ~lit, *it, true)) {
                removeWBin(solver.watches[lit.toInt()], *it, true);
                removeWBin(solver.watches[(~*it).toInt()], ~lit, true);
                solver.learnts_literals -= 2;
                solver.numBins--;
                removedUselessLearnt++;
            } else if (!solver.binPropData[it->var()].learntLeadHere) {
                removeWBin(solver.watches[lit.toInt()], *it, false);
                removeWBin(solver.watches[(~*it).toInt()], ~lit, false);
                solver.clauses_literals -= 2;
                solver.numBins--;
                removedUselessNonLearnt++;
            } else {
                continue;
            }

            Var ancestorVar = solver.binPropData[it->var()].lev1Ancestor.var();
            dontRemoveAncestor.setBit(ancestorVar);
            toClearDontRemoveAcestor.push(ancestorVar);
        }
        dontRemoveAncestor.removeThese(toClearDontRemoveAcestor);
        toClearDontRemoveAcestor.clear();
    }
    uselessBin.clear();
    #ifdef DEBUG_USELESS_LEARNT_BIN_REMOVAL
    dontRemoveAncestor.isZero();
    uint64_t backupProps;
    #endif

    assert(solver.decisionLevel() > 0);
    int32_t difference = propagatedVars.size() - (solver.trail.size()-solver.trail_lim[0]);
    #ifdef DEBUG_USELESS_LEARNT_BIN_REMOVAL
    uint32_t propagated = solver.trail.size()-solver.trail_lim[0];
    #endif
    assert(difference >= 0);
    if (difference == 0) {
        solver.cancelUntilLight();
        goto end;
    }
    for (int c = solver.trail.size()-1; c >= (int)solver.trail_lim[0]; c--) {
        Lit x = solver.trail[c];
        unPropagatedBin.clearBit(x.var());
        toVisit.push(x);
        needToVisit.setBit(x.var());
    }
    std::sort(toVisit.getData(), toVisit.getDataEnd(), LitOrder2(solver.binPropData));
    solver.cancelUntilLight();

    #ifdef DEBUG_USELESS_LEARNT_BIN_REMOVAL
    backupBogoProps = solver.bogoProps;
    if (solver.conf.doRemUselessLBins) {
        solver.newDecisionLevel();
        solver.uncheckedEnqueueLight2(lit, 0, lit_Undef, false);
        failed = (!solver.propagateBin(uselessBin).isNULL());
        uselessBin.clear();
        for (int c = solver.trail.size()-1; c >= (int)solver.trail_lim[0]; c--) {
            Lit x = solver.trail[c];
        }
        assert(!failed);
        assert(propagated == solver.trail.size()-solver.trail_lim[0]);
        solver.cancelUntilLight();
    }
    solver.bogoProps = backupBogoProps;
    #endif

    /*************************
    //To check that the ordering is the right way
    // --> i.e. to avoid mistake present in Glucose's ordering*/
    /*std::cout << "--------------------" << std::endl;
    for (uint32_t i = 0; i < toVisit.size(); i++) {
        std::cout << "i:" << std::setw(8) << i
        << " level:" << std::setw(3) << solver.binPropData[toVisit[i].var()].lev
        << " lit : " << toVisit[i]
        << std::endl;
    }
    std::cout << "difference: " << difference << std::endl;
    std::cout << "--------------------" << std::endl;*/
    /***************************/

    //difference between UP and BTC is in unPropagatedBin
    for (Lit *l = toVisit.getData(), *end = toVisit.getDataEnd(); l != end; l++) {
        if (!needToVisit[l->var()]) continue;
        if (!solver.binPropData[l->var()].hasChildren) continue;
        fillImplies(*l);
        addMyImpliesSetAsBins(*l, difference);
        assert(difference >= 0);
        myImpliesSet.clear();

        if (difference == 0) {
            needToVisit.removeTheseLit(toVisit);
            break;
        }
    }
    #ifdef DEBUG_HYPERBIN
    assert(unPropagatedBin.isZero());
    assert(needToVisit.isZero());
    #endif //DEBUG_HYPERBIN

    end:
    hyperbinProps += solver.bogoProps- oldBogoProps + extraTime;
}

void FailedLitSearcher::addMyImpliesSetAsBins(Lit lit, int32_t& difference)
{
    if (myImpliesSet.size() == 0) return;
    if (myImpliesSet.size() == 1) {
        Var var = myImpliesSet[0];
        Lit l2 = Lit(var, !propValue[var]);
        addBin(~lit, l2);
        unPropagatedBin.clearBit(var);
        difference--;
        return;
    }

    vector<BinAddData> litsAddedEach;
    uint32_t i = 0;
    for (const Var *var = myImpliesSet.getData(), *end2 = myImpliesSet.getDataEnd(); var != end2; var++, i++) {
        Lit l2 = Lit(*var, !propValue[*var]);
        solver.newDecisionLevel();
        solver.uncheckedEnqueueLight(l2);
        failed = (!solver.propagateBin(uselessBin).isNULL());
        assert(!failed);
        uselessBin.clear();

        BinAddData addData;
        addData.lit = l2;
        assert(solver.decisionLevel() > 0);
        for (int sublevel = solver.trail.size()-1; sublevel > (int)solver.trail_lim[0]; sublevel--) {
            Lit x = solver.trail[sublevel];
            addData.lits.push_back(x);
        }
        solver.cancelUntilLight();
        litsAddedEach.push_back(addData);
    }
    std::sort(litsAddedEach.begin(), litsAddedEach.end(), BinAddDataSorter());

    while(!litsAddedEach.empty()) {
        assert(!litsAddedEach.empty());
        BinAddData b = *litsAddedEach.begin();
        litsAddedEach.erase(litsAddedEach.begin());

        addBin(~lit, b.lit);
        assert(unPropagatedBin[b.lit.var()]);
        unPropagatedBin.clearBit(b.lit.var());
        difference--;
        for (uint32_t i = 0; i < b.lits.size(); i++) {
            Lit alsoAddedLit = b.lits[i];
            if (unPropagatedBin[alsoAddedLit.var()]) {
                unPropagatedBin.clearBit(alsoAddedLit.var());
                difference--;
                for (vector<BinAddData>::iterator it2 = litsAddedEach.begin(); it2 != litsAddedEach.end(); it2++) {
                    if (it2->lit == alsoAddedLit) {
                        litsAddedEach.erase(it2);
                        break;
                    }
                }
            }
        }
    }
    assert(litsAddedEach.empty());
}

/**
@brief Fills myimplies and myimpliesSet by propagating lit at a binary level

Used to check which variables are propagated by a certain literal when
propagating it only at the binary level
@p[in] the literal to be propagated at the binary level
*/
void FailedLitSearcher::fillImplies(const Lit lit)
{
    solver.newDecisionLevel();
    solver.uncheckedEnqueueLight(lit);
    failed = (!solver.propagate<false>().isNULL());
    assert(!failed);

    assert(solver.decisionLevel() > 0);
    for (int sublevel = solver.trail.size()-1; sublevel >= (int)solver.trail_lim[0]; sublevel--) {
        Var x = solver.trail[sublevel].var();
        needToVisit.clearBit(x);
        if (unPropagatedBin[x]) {
            myImpliesSet.push(x);
        }
    }
    solver.cancelUntilLight();
}

/**
@brief Adds a learnt binary clause to the solver

Used by hyperBinResolution() to add the newly discovered clauses
*/
void FailedLitSearcher::addBin(const Lit lit1, const Lit lit2)
{
    #ifdef VERBOSE_DEBUG
    std::cout << "Adding extra bin: " << lit1 << " " << lit2 << std::endl;
    #endif //VERBOSE_DEBUG

    assert(solver.value(lit1) == l_Undef);
    assert(solver.value(lit2) == l_Undef);
    tmpPs[0] = lit1;
    tmpPs[1] = lit2;

    solver.addClauseInt(tmpPs, 0 , true);
    tmpPs.clear();
    tmpPs.growTo(2);
    assert(solver.ok);
    addedBin++;
}

void FailedLitSearcher::fillToTry(vec<Var>& toTry)
{
    uint32_t max = std::min(solver.negPosDist.size()-1, (size_t)300);
    while(true) {
        Var var = solver.negPosDist[solver.mtrand.randInt(max)].var;
        if (solver.value(var) != l_Undef
            || solver.subsumer->getVarElimed()[var]
            || solver.xorSubsumer->getVarElimed()[var]
            || solver.partHandler->getSavedState()[var] != l_Undef
            ) continue;

        bool OK = true;
        for (uint32_t i = 0; i < toTry.size(); i++) {
            if (toTry[i] == var) {
                OK = false;
                break;
            }
        }
        if (OK) {
            toTry.push(var);
            return;
        }
    }
}

void FailedLitSearcher::calcNegPosDist()
{
    uint32_t varPolCount = 0;
    for (vector<std::pair<uint64_t, uint64_t> >::iterator it = solver.lTPolCount.begin(), end = solver.lTPolCount.end(); it != end; it++, varPolCount++) {
        UIPNegPosDist tmp;
        tmp.var = varPolCount;
        int64_t pos = it->first;
        int64_t neg = it->second;
        int64_t diff = std::abs((long int) (pos-neg));
        tmp.dist = pos*pos + neg*neg - diff*diff;
        if (it->first > 4  && it->second > 4)
            solver.negPosDist.push_back(tmp);
    }
    std::sort(solver.negPosDist.begin(), solver.negPosDist.end(), NegPosSorter());
}

const bool FailedLitSearcher::tryMultiLevelAll()
{
    assert(solver.ok);
    uint32_t backupNumUnits = solver.trail.size();
    double myTime = cpuTime();
    uint32_t numTries = 0;
    uint32_t finished = 0;
    uint64_t oldBogoProps = solver.bogoProps;
    uint32_t enqueued = 0;
    uint32_t numFailed = 0;



    if (solver.negPosDist.size() < 30) return true;

    propagated.resize(solver.nVars(), 0);
    propagated2.resize(solver.nVars(), 0);
    propValue.resize(solver.nVars(), 0);
    assert(propagated.isZero());
    assert(propagated2.isZero());

    vec<Var> toTry;
    while(solver.bogoProps < oldBogoProps + 300*1000*1000) {
        toTry.clear();
        for (uint32_t i = 0; i < 3; i++) {
            fillToTry(toTry);
        }
        numTries++;
        if (!tryMultiLevel(toTry, enqueued, finished, numFailed)) goto end;
    }

    end:
    assert(propagated.isZero());
    assert(propagated2.isZero());

    std::cout
    << "c multiLevelBoth tried " <<  numTries
    << " finished: " << finished
    << " units: " << (solver.trail.size() - backupNumUnits)
    << " enqueued: " << enqueued
    << " numFailed: " << numFailed
    << " time: " << (cpuTime() - myTime)
    << std::endl;

    return solver.ok;
}

const bool FailedLitSearcher::tryMultiLevel(const vec<Var>& vars, uint32_t& enqueued, uint32_t& finished, uint32_t& numFailed)
{
    assert(solver.ok);

    vec<Lit> toEnqueue;
    bool first = true;
    bool last = false;
    //std::cout << "//////////////////" << std::endl;
    for (uint32_t comb = 0; comb < (1U << vars.size()); comb++) {
        last = (comb == (1U << vars.size())-1);
        solver.newDecisionLevel();
        for (uint32_t i = 0; i < vars.size(); i++) {
            solver.uncheckedEnqueueLight(Lit(vars[i], comb&(0x1 << i)));
            //std::cout << "lit: " << Lit(vars[i], comb&(1U << i)) << std::endl;
        }
        //std::cout << "---" << std::endl;
        bool failed = !(solver.propagate<false>().isNULL());
        if (failed) {
            solver.cancelUntilLight();
            if (!first) propagated.setZero();
            numFailed++;
            return true;
        }

        for (int sublevel = solver.trail.size()-1; sublevel > (int)solver.trail_lim[0]; sublevel--) {
            Var x = solver.trail[sublevel].var();
            if (first) {
                propagated.setBit(x);
                if (solver.assigns[x].getBool()) propValue.setBit(x);
                else propValue.clearBit(x);
            } else if (last) {
                if (propagated[x] && solver.assigns[x].getBool() == propValue[x])
                    toEnqueue.push(Lit(x, !propValue[x]));
            } else {
                if (solver.assigns[x].getBool() == propValue[x]) {
                    propagated2.setBit(x);
                }
            }
        }
        solver.cancelUntilLight();
        if (!first && !last) propagated &= propagated2;
        propagated2.setZero();
        if (propagated.isZero()) return true;
        first = false;
    }
    propagated.setZero();
    finished++;

    for (Lit *l = toEnqueue.getData(), *end = toEnqueue.getDataEnd(); l != end; l++) {
        enqueued++;
        solver.uncheckedEnqueue(*l);
    }
    solver.ok = solver.propagate<true>().isNULL();
    //exit(-1);

    return solver.ok;
}
