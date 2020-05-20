/*********************                                                        */
/*! \file proof.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Andrew Reynolds
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2019 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved.  See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief Implementation of strings proof
 **/

#include "theory/strings/proof_checker.h"

#include "options/strings_options.h"
#include "theory/rewriter.h"
#include "theory/strings/core_solver.h"
#include "theory/strings/regexp_operation.h"
#include "theory/strings/term_registry.h"
#include "theory/strings/theory_strings_preprocess.h"
#include "theory/strings/theory_strings_utils.h"
#include "theory/strings/word.h"

using namespace CVC4::kind;

namespace CVC4 {
namespace theory {
namespace strings {

void StringProofRuleChecker::registerTo(ProofChecker* pc)
{
  pc->registerChecker(PfRule::CONCAT_EQ, this);
  pc->registerChecker(PfRule::CONCAT_UNIFY, this);
  pc->registerChecker(PfRule::CONCAT_CONFLICT, this);
  pc->registerChecker(PfRule::CONCAT_SPLIT, this);
  pc->registerChecker(PfRule::CONCAT_CSPLIT, this);
  pc->registerChecker(PfRule::CONCAT_LPROP, this);
  pc->registerChecker(PfRule::CONCAT_CPROP, this);
  pc->registerChecker(PfRule::LENGTH_POS, this);
  pc->registerChecker(PfRule::LENGTH_NON_EMPTY, this);
  pc->registerChecker(PfRule::CTN_NOT_EQUAL, this);
  pc->registerChecker(PfRule::STRINGS_REDUCTION, this);
  pc->registerChecker(PfRule::STRINGS_EAGER_REDUCTION, this);
  pc->registerChecker(PfRule::RE_INTER, this);
  pc->registerChecker(PfRule::RE_UNFOLD_POS, this);
  pc->registerChecker(PfRule::RE_UNFOLD_NEG, this);
  pc->registerChecker(PfRule::RE_UNFOLD_NEG_CONCAT_FIXED, this);
}

Node StringProofRuleChecker::checkInternal(PfRule id,
                                           const std::vector<Node>& children,
                                           const std::vector<Node>& args)
{
  if (id == PfRule::CONCAT_EQ || id == PfRule::CONCAT_UNIFY
      || id == PfRule::CONCAT_CONFLICT || id == PfRule::CONCAT_SPLIT
      || id == PfRule::CONCAT_CSPLIT || id == PfRule::CONCAT_LPROP
      || id == PfRule::CONCAT_CPROP)
  {
    Trace("strings-pfcheck") << "Checking id " << id << std::endl;
    Assert(children.size() >= 1);
    Assert(args.size() == 1);
    // all rules have an equality
    if (children[0].getKind() != EQUAL)
    {
      return Node::null();
    }
    // convert to concatenation form
    std::vector<Node> tvec;
    std::vector<Node> svec;
    utils::getConcat(children[0][0], tvec);
    utils::getConcat(children[0][1], svec);
    size_t nchildt = tvec.size();
    size_t nchilds = svec.size();
    TypeNode stringType = children[0][0].getType();
    NodeManager* nm = NodeManager::currentNM();
    // extract the Boolean corresponding to whether the rule is reversed
    bool isRev;
    if (!getBool(args[0], isRev))
    {
      return Node::null();
    }
    if (id == PfRule::CONCAT_EQ)
    {
      Assert(children.size() == 1);
      size_t index = 0;
      std::vector<Node> tremVec;
      std::vector<Node> sremVec;
      // scan the concatenation until we exhaust child proofs
      while (index < nchilds && index < nchildt)
      {
        Node currT = tvec[isRev ? (nchildt - 1 - index) : index];
        Node currS = svec[isRev ? (nchilds - 1 - index) : index];
        if (currT != currS)
        {
          if (currT.isConst() && currS.isConst())
          {
            size_t sindex;
            // get the equal prefix/suffix, strip and add the remainders
            Node currR = Word::splitConstant(currT, currS, sindex, isRev);
            if (!currR.isNull())
            {
              // add the constant to remainder vec
              std::vector<Node>& rem = sindex == 0 ? tremVec : sremVec;
              rem.push_back(currR);
              // ignore the current component
              index++;
              // In other words, if we have (currS,currT) = ("ab","abc"), then
              // we proceed to the next component and add currR = "c" to
              // tremVec.
            }
            // otherwise if we are not the same prefix, then both will be added
            // Notice that we do not add maximal prefixes, in other words,
            // ("abc", "abd") may be added to the remainder vectors, and not
            // ("c", "d").
          }
          break;
        }
        index++;
      }
      Assert(index <= nchildt);
      Assert(index <= nchilds);
      // the remainders are equal
      tremVec.insert(isRev ? tremVec.begin() : tremVec.end(),
                     tvec.begin() + (isRev ? 0 : index),
                     tvec.begin() + nchildt - (isRev ? index : 0));
      sremVec.insert(isRev ? sremVec.begin() : sremVec.end(),
                     svec.begin() + (isRev ? 0 : index),
                     svec.begin() + nchilds - (isRev ? index : 0));
      // convert back to node
      Node trem = utils::mkConcat(tremVec, stringType);
      Node srem = utils::mkConcat(sremVec, stringType);
      return trem.eqNode(srem);
    }
    // all remaining rules do something with the first side of each side
    Node t0 = tvec[isRev ? nchildt - 1 : 0];
    Node s0 = svec[isRev ? nchilds - 1 : 0];
    if (id == PfRule::CONCAT_UNIFY)
    {
      Assert(children.size() == 2);
      if (children[1].getKind() != EQUAL)
      {
        return Node::null();
      }
      for (unsigned i = 0; i < 2; i++)
      {
        Node l = children[1][i];
        if (l.getKind() != STRING_LENGTH)
        {
          return Node::null();
        }
        Node term = i == 0 ? t0 : s0;
        if (l[0] == term)
        {
          continue;
        }
        // could be a spliced constant
        bool success = false;
        if (term.isConst() && l[0].isConst())
        {
          size_t lenL = Word::getLength(l[0]);
          success = (isRev && l[0] == Word::suffix(term, lenL))
                    || (!isRev && l[0] == Word::prefix(term, lenL));
        }
        if (!success)
        {
          return Node::null();
        }
      }
      return children[1][0][0].eqNode(children[1][1][0]);
    }
    else if (id == PfRule::CONCAT_CONFLICT)
    {
      Assert(children.size() == 1);
      if (!t0.isConst() || !s0.isConst())
      {
        // not constants
        return Node::null();
      }
      size_t sindex;
      Node r0 = Word::splitConstant(t0, s0, sindex, isRev);
      if (!r0.isNull())
      {
        // Not a conflict due to constants, i.e. s0 is a prefix of t0 or vice
        // versa.
        return Node::null();
      }
      return nm->mkConst(false);
    }
    else if (id == PfRule::CONCAT_SPLIT)
    {
      Assert(children.size() == 2);
      if (children[1].getKind() != NOT || children[1][0].getKind() != EQUAL
          || children[1][0][0].getKind() != STRING_LENGTH
          || children[1][0][0][0] != t0
          || children[1][0][1].getKind() != STRING_LENGTH
          || children[1][0][1][0] != s0)
      {
        return Node::null();
      }
    }
    else if (id == PfRule::CONCAT_CSPLIT)
    {
      Assert(children.size() == 2);
      Node zero = nm->mkConst(Rational(0));
      Node one = nm->mkConst(Rational(1));
      if (children[1].getKind() != NOT || children[1][0].getKind() != EQUAL
          || children[1][0][0].getKind() != STRING_LENGTH
          || children[1][0][0][0] != t0 || children[1][0][1] != zero)
      {
        return Node::null();
      }
      if (!s0.isConst() || !s0.getType().isStringLike() || Word::isEmpty(s0))
      {
        return Node::null();
      }
    }
    else if (id == PfRule::CONCAT_LPROP)
    {
      Assert(children.size() == 2);
      if (children[1].getKind() != GT
          || children[1][0].getKind() != STRING_LENGTH
          || children[1][0][0] != t0
          || children[1][1].getKind() != STRING_LENGTH
          || children[1][1][0] != s0)
      {
        return Node::null();
      }
    }
    else if (id == PfRule::CONCAT_CPROP)
    {
      Assert(children.size() == 2);
      Node zero = nm->mkConst(Rational(0));

      Trace("pfcheck-strings-cprop")
          << "CONCAT_PROP, isRev=" << isRev << std::endl;
      if (children[1].getKind() != NOT || children[1][0].getKind() != EQUAL
          || children[1][0][0].getKind() != STRING_LENGTH
          || children[1][0][0][0] != t0 || children[1][0][1] != zero)
      {
        Trace("pfcheck-strings-cprop")
            << "...failed pattern match" << std::endl;
        return Node::null();
      }
      if (tvec.size() <= 1)
      {
        Trace("pfcheck-strings-cprop")
            << "...failed adjacent constant" << std::endl;
        return Node::null();
      }
      Node w1 = tvec[isRev ? nchildt - 2 : 1];
      if (!w1.isConst() || !w1.getType().isStringLike() || Word::isEmpty(w1))
      {
        Trace("pfcheck-strings-cprop")
            << "...failed adjacent constant content" << std::endl;
        return Node::null();
      }
      Node w2 = s0;
      if (!w2.isConst() || !w2.getType().isStringLike() || Word::isEmpty(w2))
      {
        Trace("pfcheck-strings-cprop") << "...failed constant" << std::endl;
        return Node::null();
      }
      Trace("pfcheck-strings-cprop")
          << "w1,w2 = " << w1 << " " << w2 << std::endl;
      size_t lenW2 = Word::getLength(w2);
      Node w2mc1 =
          isRev ? Word::prefix(w2, lenW2 - 1) : Word::suffix(w2, lenW2 - 1);
      Trace("pfcheck-strings-cprop") << "w2mc1 = " << w2mc1 << std::endl;
      size_t p =
          lenW2
          - (isRev ? Word::roverlap(w2mc1, w1) : Word::overlap(w2mc1, w1));
      Trace("pfcheck-strings-cprop") << "p = " << p << std::endl;
      size_t p2 = isRev ? Word::rfind(w2mc1, w1) : Word::find(w2mc1, w1);
      if (p2 == std::string::npos)
      {
        Trace("pfcheck-strings-cprop") << "p2 = std::string::npos" << std::endl;
      }
      else
      {
        Trace("pfcheck-strings-cprop") << "p2 = " << p2 << std::endl;
      }
      size_t p3 = p2 == std::string::npos ? p : (p > p2 + 1 ? p2 + 1 : p);
      Trace("pfcheck-strings-cprop") << "p3 = " << p3 << std::endl;
      Node w3 = isRev ? Word::suffix(w2, p3) : Word::prefix(w2, p3);
      Trace("pfcheck-strings-cprop") << "w3 = " << w3 << std::endl;
      Node rbody =
          isRev ? utils::mkPrefix(t0,
                                  nm->mkNode(MINUS,
                                             nm->mkNode(STRING_LENGTH, t0),
                                             nm->mkNode(STRING_LENGTH, w3)))
                : utils::mkSuffix(t0, nm->mkNode(STRING_LENGTH, w3));
      Node r = ProofSkolemCache::mkPurifySkolem(rbody, "r");
      Node conc;
      if (isRev)
      {
        conc = t0.eqNode(nm->mkNode(STRING_CONCAT, r, w3));
      }
      else
      {
        conc = t0.eqNode(nm->mkNode(STRING_CONCAT, w3, r));
      }
      return conc;
    }
    // use skolem cache
    SkolemCache skc(false);
    std::vector<Node> newSkolems;
    Node kt0 = ProofSkolemCache::getSkolemForm(t0);
    Node ks0 = ProofSkolemCache::getSkolemForm(s0);
    Node conc =
        CoreSolver::getConclusion(kt0, ks0, id, isRev, &skc, newSkolems);
    conc = ProofSkolemCache::getWitnessForm(conc);
    return conc;
  }
  else if (id == PfRule::CTN_NOT_EQUAL)
  {
    // TODO: probably unnecessary (build into eager reduction?)
  }
  else if (id == PfRule::STRINGS_REDUCTION
           || id == PfRule::STRINGS_EAGER_REDUCTION || id == PfRule::LENGTH_POS)
  {
    Assert(children.empty());
    Assert(args.size() >= 1);
    // These rules are based on called a C++ method for returning a valid
    // lemma involving a single argument term.
    // Must convert to skolem form.
    Node t = ProofSkolemCache::getSkolemForm(args[0]);
    Node ret;
    if (id == PfRule::STRINGS_REDUCTION)
    {
      Assert(args.size() == 1);
      // we do not use optimizations
      SkolemCache skc(false);
      std::vector<Node> conj;
      ret = StringsPreprocess::reduce(t, conj, &skc);
      conj.push_back(t.eqNode(ret));
      ret = mkAnd(conj);
    }
    else if (id == PfRule::STRINGS_EAGER_REDUCTION)
    {
      Assert(args.size() <= 2);
      uint32_t i = 0;
      if (args.size() >= 2)
      {
        getIndex(args[1], i);
      }
      SkolemCache skc(false);
      ret = TermRegistry::eagerReduce(t, &skc, i);
    }
    else if (id == PfRule::LENGTH_POS)
    {
      Assert(args.size() == 1);
      ret = TermRegistry::lengthPositive(t);
    }
    if (ret.isNull())
    {
      return Node::null();
    }
    Node retw = ProofSkolemCache::getWitnessForm(ret);
    return retw;
  }
  else if (id == PfRule::LENGTH_NON_EMPTY)
  {
    Assert(children.size() == 1);
    Assert(args.empty());
    Node nemp = children[0];
    if (nemp.getKind() != NOT || nemp[0].getKind() != EQUAL
        || !nemp[0][1].isConst() || !nemp[0][1].getType().isStringLike())
    {
      return Node::null();
    }
    if (!Word::isEmpty(nemp[0][1]))
    {
      return Node::null();
    }
    NodeManager* nm = NodeManager::currentNM();
    Node zero = nm->mkConst(Rational(0));
    Node clen = nm->mkNode(STRING_LENGTH, nemp[0][0]);
    return clen.eqNode(zero).notNode();
  }
  else if (id == PfRule::RE_INTER)
  {
    Assert(children.size() >= 1);
    Assert(args.empty());
    NodeManager* nm = NodeManager::currentNM();
    std::vector<Node> reis;
    Node x;
    // make the regular expression intersection that summarizes all
    // memberships in the explanation
    for (const Node& c : children)
    {
      bool polarity = c.getKind() != NOT;
      Node catom = polarity ? c : c[0];
      if (catom.getKind() != STRING_IN_REGEXP)
      {
        return Node::null();
      }
      if (x.isNull())
      {
        x = catom[0];
      }
      else if (x != catom[0])
      {
        // different LHS
        return Node::null();
      }
      Node xcurr = catom[0];
      Node rcurr =
          polarity ? catom[1] : nm->mkNode(REGEXP_COMPLEMENT, catom[1]);
      reis.push_back(rcurr);
    }
    Node rei = reis.size() == 1 ? reis[0] : nm->mkNode(REGEXP_INTER, reis);
    return nm->mkNode(STRING_IN_REGEXP, x, rei);
  }
  else if (id == PfRule::RE_UNFOLD_POS || id == PfRule::RE_UNFOLD_NEG
           || id == PfRule::RE_UNFOLD_NEG_CONCAT_FIXED)
  {
    Assert(children.size() == 1);
    Assert(args.empty());
    Node atom = children[0];
    if (id == PfRule::RE_UNFOLD_NEG || id == PfRule::RE_UNFOLD_NEG_CONCAT_FIXED)
    {
      if (atom.getKind() != NOT)
      {
        Trace("strings-pfcheck") << "...fail, missing negation" << std::endl;
        return Node::null();
      }
      atom = atom[0];
    }
    if (atom.getKind() != STRING_IN_REGEXP)
    {
      Trace("strings-pfcheck") << "...fail, non-member" << std::endl;
      return Node::null();
    }
    // must convert to skolem form
    atom = ProofSkolemCache::getSkolemForm(atom);
    Node conc;
    if (id == PfRule::RE_UNFOLD_POS)
    {
      SkolemCache sc;
      conc = RegExpOpr::reduceRegExpPos(children[0], &sc);
    }
    else if (id == PfRule::RE_UNFOLD_NEG)
    {
      conc = RegExpOpr::reduceRegExpNeg(children[0]);
    }
    else if (id == PfRule::RE_UNFOLD_NEG_CONCAT_FIXED)
    {
      if (atom[1].getKind() != REGEXP_CONCAT)
      {
        Trace("strings-pfcheck") << "...fail, no concat regexp" << std::endl;
        return Node::null();
      }
      unsigned index;
      Node reLen = RegExpOpr::getRegExpConcatFixed(atom[1], index);
      if (reLen.isNull())
      {
        Trace("strings-pfcheck") << "...fail, non-fixed lengths" << std::endl;
        return Node::null();
      }
      conc = RegExpOpr::reduceRegExpNegConcatFixed(children[0], reLen, index);
    }
    return ProofSkolemCache::getWitnessForm(conc);
  }
  return Node::null();
}

}  // namespace strings
}  // namespace theory
}  // namespace CVC4
