/*********************                                                        */
/*! \file bv_inverter.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Andrew Reynolds
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2017 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved.  See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief inverse rules for bit-vector operators
 **/

#include "theory/quantifiers/bv_inverter.h"

#include <algorithm>
#include <stack>

#include "options/quantifiers_options.h"
#include "theory/quantifiers/term_util.h"
#include "theory/rewriter.h"
#include "theory/bv/theory_bv_utils.h"


using namespace CVC4::kind;

namespace CVC4 {
namespace theory {
namespace quantifiers {

/*---------------------------------------------------------------------------*/

Node BvInverter::getSolveVariable(TypeNode tn)
{
  std::map<TypeNode, Node>::iterator its = d_solve_var.find(tn);
  if (its == d_solve_var.end())
  {
    Node k = NodeManager::currentNM()->mkSkolem("slv", tn);
    d_solve_var[tn] = k;
    return k;
  }
  else
  {
    return its->second;
  }
}

/*---------------------------------------------------------------------------*/

Node BvInverter::getInversionNode(Node cond, TypeNode tn, BvInverterQuery* m)
{
  TNode solve_var = getSolveVariable(tn);

  // condition should be rewritten
  Node new_cond = Rewriter::rewrite(cond);
  if (new_cond != cond)
  {
    Trace("cegqi-bv-skvinv-debug") << "Condition " << cond
                                   << " was rewritten to " << new_cond
                                   << std::endl;
  }
  // optimization : if condition is ( x = solve_var ) should just return
  // solve_var and not introduce a Skolem this can happen when we ask for
  // the multiplicative inversion with bv1
  Node c;
  if (new_cond.getKind() == EQUAL)
  {
    for (unsigned i = 0; i < 2; i++)
    {
      if (new_cond[i] == solve_var)
      {
        c = new_cond[1 - i];
        Trace("cegqi-bv-skvinv") << "SKVINV : " << c
                                 << " is trivially associated with conditon "
                                 << new_cond << std::endl;
        break;
      }
    }
  }
  // TODO : compute the value if the condition is deterministic, e.g. calc
  // multiplicative inverse of 2 constants
  if (c.isNull())
  {
    NodeManager* nm = NodeManager::currentNM();
    if (m)
    {
      Node x = m->getBoundVariable(tn);
      Node ccond = new_cond.substitute(solve_var, x);
      c = nm->mkNode(kind::CHOICE, nm->mkNode(BOUND_VAR_LIST, x), ccond);
      Trace("cegqi-bv-skvinv")
          << "SKVINV : Make " << c << " for " << new_cond << std::endl;
    }
    else
    {
      Trace("bv-invert") << "...fail for " << cond << " : no inverter query!"
                         << std::endl;
    }
  }
  // currently shouldn't cache since
  // the return value depends on the
  // state of m (which bound variable is returned).
  return c;
}

/*---------------------------------------------------------------------------*/

static bool isInvertible(Kind k, unsigned index)
{
  return  k == NOT
      ||  k == EQUAL
      ||  k == BITVECTOR_ULT
      ||  k == BITVECTOR_SLT
      ||  k == BITVECTOR_COMP
      ||  k == BITVECTOR_NOT
      ||  k == BITVECTOR_NEG
      ||  k == BITVECTOR_CONCAT
      ||  k == BITVECTOR_SIGN_EXTEND
      ||  k == BITVECTOR_PLUS
      ||  k == BITVECTOR_SUB
      ||  k == BITVECTOR_MULT
      ||  k == BITVECTOR_UREM_TOTAL
      ||  k == BITVECTOR_UDIV_TOTAL
      ||  k == BITVECTOR_AND
      ||  k == BITVECTOR_OR
      ||  k == BITVECTOR_XOR
      || k == BITVECTOR_LSHR
      || k == BITVECTOR_ASHR
      || k == BITVECTOR_SHL;
}

Node BvInverter::getPathToPv(
    Node lit,
    Node pv,
    Node sv,
    std::vector<unsigned>& path,
    std::unordered_set<TNode, TNodeHashFunction>& visited)
{
  if (visited.find(lit) == visited.end())
  {
    visited.insert(lit);
    if (lit == pv)
    {
      return sv;
    }
    else
    {
      unsigned rmod = 0;  // TODO : randomize?
      for (unsigned i = 0; i < lit.getNumChildren(); i++)
      {
        unsigned ii = (i + rmod) % lit.getNumChildren();
        // only recurse if the kind is invertible
        // this allows us to avoid paths that go through skolem functions
        if (!isInvertible(lit.getKind(), ii))
        {
          continue;
        }
        Node litc = getPathToPv(lit[ii], pv, sv, path, visited);
        if (!litc.isNull())
        {
          // path is outermost term index last
          path.push_back(ii);
          std::vector<Node> children;
          if (lit.getMetaKind() == kind::metakind::PARAMETERIZED)
          {
            children.push_back(lit.getOperator());
          }
          for (unsigned j = 0; j < lit.getNumChildren(); j++)
          {
            children.push_back(j == ii ? litc : lit[j]);
          }
          return NodeManager::currentNM()->mkNode(lit.getKind(), children);
        }
      }
    }
  }
  return Node::null();
}

Node BvInverter::getPathToPv(
    Node lit, Node pv, Node sv, Node pvs, std::vector<unsigned>& path)
{
  std::unordered_set<TNode, TNodeHashFunction> visited;
  Node slit = getPathToPv(lit, pv, sv, path, visited);
  // if we are able to find a (invertible) path to pv
  if (!slit.isNull() && !pvs.isNull())
  {
    // substitute pvs for the other occurrences of pv
    TNode tpv = pv;
    TNode tpvs = pvs;
    slit = slit.substitute(tpv, tpvs);
  }
  return slit;
}

/*---------------------------------------------------------------------------*/

/* Drop child at given index from expression.
 * E.g., dropChild((x + y + z), 1) -> (x + z)  */
static Node dropChild(Node n, unsigned index)
{
  unsigned nchildren = n.getNumChildren();
  Assert(index < nchildren);
  Kind k = n.getKind();
  Assert(k == BITVECTOR_AND || k == BITVECTOR_OR || k == BITVECTOR_MULT
         || k == BITVECTOR_PLUS);
  NodeBuilder<> nb(NodeManager::currentNM(), k);
  for (unsigned i = 0; i < nchildren; ++i)
  {
    if (i == index) continue;
    nb << n[i];
  }
  return nb.constructNode();
}

static Node getScBvUltUgt(bool pol, Kind k, Node x, Node t)
{
  Assert(k == BITVECTOR_ULT || k == BITVECTOR_UGT);

  NodeManager* nm = NodeManager::currentNM();
  unsigned w = bv::utils::getSize(t);
  Node sc;

  if (k == BITVECTOR_ULT)
  {
    if (pol == true)
    {
      /* x < t
       * with side condition:
       * (distinct t z)
       * where
       * z = 0 with getSize(z) = w  */
      Node scl = nm->mkNode(DISTINCT, t, bv::utils::mkZero(w));
      Node scr = nm->mkNode(k, x, t);
      sc = nm->mkNode(IMPLIES, scl, scr);
    }
    else
    {
      /* x >= t
       * with side condition:
       * true (no side condition)  */
      sc = nm->mkNode(NOT, nm->mkNode(k, x, t));
    }
  }
  else
  {
    Assert(k == BITVECTOR_UGT);
    if (pol == true)
    {
      /* x > t
       * with side condition:
       * (distinct t ones)
       * where
       * ones = ~0 with getSize(ones) = w  */
      Node scl = nm->mkNode(DISTINCT, t, bv::utils::mkOnes(w));
      Node scr = nm->mkNode(k, x, t);
      sc = nm->mkNode(IMPLIES, scl, scr);
    }
    else
    {
      /* x <= t
       * with side condition:
       * true (no side condition)  */
      sc = nm->mkNode(NOT, nm->mkNode(k, x, t));
    }
  }
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

static Node getScBvSltSgt(bool pol, Kind k, Node x, Node t)
{
  Assert(k == BITVECTOR_SLT || k == BITVECTOR_SGT);

  NodeManager* nm = NodeManager::currentNM();
  unsigned w = bv::utils::getSize(t);
  Node sc;

  if (k == BITVECTOR_SLT)
  {
    if (pol == true)
    {
      /* x < t
       * with side condition:
       * (distinct t min)
       * where
       * min is the minimum signed value with getSize(min) = w  */
      Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
      Node scl = nm->mkNode(DISTINCT, min, t);
      Node scr = nm->mkNode(k, x, t);
      sc = nm->mkNode(IMPLIES, scl, scr);
    }
    else
    {
      /* x >= t
       * with side condition:
       * true (no side condition)  */
      sc = nm->mkNode(NOT, nm->mkNode(k, x, t));
    }
  }
  else
  {
    Assert(k == BITVECTOR_SGT);
    if (pol == true)
    {
      /* x > t
       * with side condition:
       * (distinct t max)
       * where
       * max is the signed maximum value with getSize(max) = w  */
      Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
      Node scl = nm->mkNode(DISTINCT, t, max);
      Node scr = nm->mkNode(k, x, t);
      sc = nm->mkNode(IMPLIES, scl, scr);
    }
    else
    {
      /* x <= t
       * with side condition:
       * true (no side condition)  */
      sc = nm->mkNode(NOT, nm->mkNode(k, x, t));
    }
  }
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

static Node getScBvMult(bool pol,
                        Kind litk,
                        Kind k,
                        unsigned idx,
                        Node x,
                        Node s,
                        Node t)
{
  Assert(k == BITVECTOR_MULT);
  Assert (litk == EQUAL
       || litk == BITVECTOR_ULT || litk == BITVECTOR_SLT
       || litk == BITVECTOR_UGT || litk == BITVECTOR_SGT);

  NodeManager* nm = NodeManager::currentNM();
  Node scl;
  unsigned w = bv::utils::getSize(s);
  Assert (w == bv::utils::getSize(t));

  if (litk == EQUAL)
  {
    Node z = bv::utils::mkZero(w);

    if (pol)
    {
      /* x * s = t
       * with side condition (synthesized):
       * (= (bvand (bvor (bvneg s) s) t) t)
       *
       * is equivalent to:
       * ctz(t) >= ctz(s)
       * ->
       * (or
       *   (= t z)
       *   (and
       *     (bvuge (bvand t (bvneg t)) (bvand s (bvneg s)))
       *     (distinct s z)))
       * where
       * z = 0 with getSize(z) = w  */
      Node o = nm->mkNode(BITVECTOR_OR, nm->mkNode(BITVECTOR_NEG, s), s);
      scl = nm->mkNode(EQUAL, nm->mkNode(BITVECTOR_AND, o, t), t);
    }
    else
    {
      /* x * s != t
       * with side condition:
       * (or (distinct t z) (distinct s z))
       * where
       * z = 0 with getSize(z) = w  */
      scl = nm->mkNode(OR, t.eqNode(z).notNode(), s.eqNode(z).notNode());
    }
  }
  else if (litk == BITVECTOR_ULT)
  {
    if (pol)
    {
      /* x * s < t
       * with side condition (synthesized):
       * (distinct t z)
       * where
       * z = 0 with getSize(z) = w  */
      Node z = bv::utils::mkZero(w);
      scl =  nm->mkNode(DISTINCT, t, z);
    }
    else
    {
      /* x * s >= t
       * with side condition (synthesized):
       * (bvuge (bvor (bvneg s) s) t)  */
      Node o = nm->mkNode(BITVECTOR_OR, nm->mkNode(BITVECTOR_NEG, s), s);
      scl = nm->mkNode(BITVECTOR_UGE, o, t);
    }
  }
  else if (litk == BITVECTOR_UGT)
  {
    if (pol)
    {
      /* x * s > t
       * with side condition (synthesized):
       * (bvult t (bvor (bvneg s) s))  */
      Node o = nm->mkNode(BITVECTOR_OR, nm->mkNode(BITVECTOR_NEG, s), s);
      scl = nm->mkNode(BITVECTOR_ULT, t, o);
    }
    else
    {
      /* x * s <= t
       * true (no side condition)  */
      scl = nm->mkConst<bool>(true);
    }
  }
  else if (litk == BITVECTOR_SLT)
  {
    if (pol)
    {
      /* x * s < t
       * with side condition (synthesized):
       * (bvslt (bvand (bvnot (bvneg t)) (bvor (bvneg s) s)) t)  */
      Node a1 = nm->mkNode(BITVECTOR_NOT, nm->mkNode(BITVECTOR_NEG, t));
      Node a2 = nm->mkNode(BITVECTOR_OR, nm->mkNode(BITVECTOR_NEG, s), s);
      scl = nm->mkNode(BITVECTOR_SLT, nm->mkNode(BITVECTOR_AND, a1, a2), t);
    }
    else
    {
      /* x * s >= t
       * with side condition (synthesized):
       * (bvsge (bvand (bvor (bvneg s) s) max) t)
       * where
       * max is the signed maximum value with getSize(max) = w  */
      Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
      Node o = nm->mkNode(BITVECTOR_OR, nm->mkNode(BITVECTOR_NEG, s), s);
      Node a = nm->mkNode(BITVECTOR_AND, o, max);
      scl = nm->mkNode(BITVECTOR_SGE, a, t);
    }
  }
  else  /* litk == BITVECTOR_SGT */
  {
    if (pol)
    {
      /* x * s > t
       * with side condition (synthesized):
       * (bvslt t (bvsub t (bvor (bvor s t) (bvneg s))))  */
      Node o = nm->mkNode(BITVECTOR_OR,
          nm->mkNode(BITVECTOR_OR, s, t), nm->mkNode(BITVECTOR_NEG, s));
      Node sub = nm->mkNode(BITVECTOR_SUB, t, o);
      scl = nm->mkNode(BITVECTOR_SLT, t, sub);
    }
    else
    {
      /* x * s <= t
       * with side condition (synthesized):
       * (not (and (= s z) (bvslt t s)))
       * where
       * z = 0 with getSize(z) = w  */
      Node z = bv::utils::mkZero(w);
      scl = nm->mkNode(AND, s.eqNode(z), nm->mkNode(BITVECTOR_SLT, t, s));
      scl = scl.notNode();
    }
  }

  Node scr =
    nm->mkNode(litk, idx == 0 ? nm->mkNode(k, x, s) : nm->mkNode(k, s, x), t);
  Node sc = nm->mkNode(IMPLIES, scl, pol ? scr : scr.notNode());
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

static Node getScBvUrem(bool pol,
                        Kind litk,
                        Kind k,
                        unsigned idx,
                        Node x,
                        Node s,
                        Node t)
{
  Assert(k == BITVECTOR_UREM_TOTAL);
  Assert (litk == EQUAL
       || litk == BITVECTOR_ULT || litk == BITVECTOR_SLT
       || litk == BITVECTOR_UGT || litk == BITVECTOR_SGT);

  NodeManager* nm = NodeManager::currentNM();
  Node scl;
  unsigned w = bv::utils::getSize(s);
  Assert (w == bv::utils::getSize(t));

  if (litk == EQUAL)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x % s = t
         * with side condition (synthesized):
         * (bvuge (bvnot (bvneg s)) t)  */
        Node neg = nm->mkNode(BITVECTOR_NEG, s);
        scl = nm->mkNode(BITVECTOR_UGE, nm->mkNode(BITVECTOR_NOT, neg), t);
      }
      else
      {
        /* x % s != t
         * with side condition:
         * (or (distinct s (_ bv1 w)) (distinct t z))
         * where
         * z = 0 with getSize(z) = w  */
        Node z = bv::utils::mkZero(w);
        scl = nm->mkNode(OR,
            s.eqNode(bv::utils::mkOne(w)).notNode(),
            t.eqNode(z).notNode());
      }
    }
    else
    {
      if (pol)
      {
        /* s % x = t
         * with side condition (synthesized):
         * (bvuge (bvand (bvsub (bvadd t t) s) s) t)
         *
         * is equivalent to:
         * (or (= s t)
         *     (and (bvugt s t)
         *          (bvugt (bvsub s t) t)
         *          (or (= t z) (distinct (bvsub s (_ bv1 w)) t))))
         * where
         * z = 0 with getSize(z) = w  */
        Node add = nm->mkNode(BITVECTOR_PLUS, t, t);
        Node sub = nm->mkNode(BITVECTOR_SUB, add, s);
        Node a = nm->mkNode(BITVECTOR_AND, sub, s);
        scl = nm->mkNode(BITVECTOR_UGE, a, t);
      }
      else
      {
        /* s % x != t
         * with side condition:
         * (or (distinct s z) (distinct t z))
         * where
         * z = 0 with getSize(z) = w  */
        Node z = bv::utils::mkZero(w);
        scl = nm->mkNode(OR, s.eqNode(z).notNode(), t.eqNode(z).notNode());
      }
    }
  }
  else if (litk == BITVECTOR_ULT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x % s < t
         * with side condition:
         * (distinct t z)
         * where
         * z = 0 with getSize(z) = w  */
        Node z = bv::utils::mkZero(w);
        scl = t.eqNode(z).notNode();
      }
      else
      {
        /* x % s >= t
         * with side condition (synthesized):
         * (bvuge (bvnot (bvneg s)) t)  */
        Node neg = nm->mkNode(BITVECTOR_NEG, s);
        scl = nm->mkNode(BITVECTOR_UGE, nm->mkNode(BITVECTOR_NOT, neg), t);
      }
    }
    else
    {
      if (pol)
      {
        /* s % x < t
         * with side condition:
         * (distinct t z)
         * where
         * z = 0 with getSize(z) = w  */
        Node z = bv::utils::mkZero(w);
        scl = t.eqNode(z).notNode();
      }
      else
      {
        /* s % x >= t
         * with side condition (combination of = and >):
         * (or
         *   (bvuge (bvand (bvsub (bvadd t t) s) s) t)  ; eq, synthesized
         *   (bvult t s))                               ; ugt, synthesized  */
        Node add = nm->mkNode(BITVECTOR_PLUS, t, t);
        Node sub = nm->mkNode(BITVECTOR_SUB, add, s);
        Node a = nm->mkNode(BITVECTOR_AND, sub, s);
        Node sceq = nm->mkNode(BITVECTOR_UGE, a, t);
        Node scugt = nm->mkNode(BITVECTOR_ULT, t, s);
        scl = nm->mkNode(OR, sceq, scugt);
      }
    }
  }
  else if (litk == BITVECTOR_UGT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x % s > t
         * with side condition (synthesized):
         * (bvult t (bvnot (bvneg s)))  */
        Node nt = nm->mkNode(BITVECTOR_NOT, nm->mkNode(BITVECTOR_NEG, s));
        scl = nm->mkNode(BITVECTOR_ULT, t, nt);
      }
      else
      {
        /* x % s <= t
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
    else
    {
      if (pol)
      {
        /* s % x > t
         * with side condition (synthesized):
         * (bvult t s)  */
        scl = nm->mkNode(BITVECTOR_ULT, t, s);
      }
      else
      {
        /* s % x <= t
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
  }
  else if (litk == BITVECTOR_SLT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x % s < t
         * with side condition (synthesized):
         * (bvslt (bvnot t) (bvor (bvneg s) (bvneg t)))  */
        Node o1 = nm->mkNode(BITVECTOR_NEG, s);
        Node o2 = nm->mkNode(BITVECTOR_NEG, t);
        Node o = nm->mkNode(BITVECTOR_OR, o1, o2);
        scl = nm->mkNode(BITVECTOR_SLT, nm->mkNode(BITVECTOR_NOT, t), o);
      }
      else
      {
        /* x % s >= t
         * with side condition (synthesized):
         * (or (bvslt t s) (bvsge z s))
         * where
         * z = 0 with getSize(z) = w  */
        Node z = bv::utils::mkZero(w);
        Node s1 = nm->mkNode(BITVECTOR_SLT, t, s);
        Node s2 = nm->mkNode(BITVECTOR_SGE, z, s);
        scl = nm->mkNode(OR, s1, s2);
      }
    }
    else
    {
      Node z = bv::utils::mkZero(w);

      if (pol)
      {
        /* s % x < t
         * with side condition (synthesized):
         * (or (bvslt s t) (bvslt z t))
         * where
         * z = 0 with getSize(z) = w  */
        Node slt1 = nm->mkNode(BITVECTOR_SLT, s, t);
        Node slt2 = nm->mkNode(BITVECTOR_SLT, z, t);
        scl = nm->mkNode(OR, slt1, slt2);
      }
      else
      {
        /* s % x >= t
         * with side condition:
         * (and
         *   (=> (bvsge s z) (bvsge s t))
         *   (=> (and (bvslt s z) (bvsge t z)) (bvugt (bvsub s t) t)))
         * where
         * z = 0 with getSize(z) = w  */
        Node i1 = nm->mkNode(IMPLIES,
            nm->mkNode(BITVECTOR_SGE, s, z), nm->mkNode(BITVECTOR_SGE, s, t));
        Node i2 = nm->mkNode(IMPLIES,
            nm->mkNode(AND,
              nm->mkNode(BITVECTOR_SLT, s, z), nm->mkNode(BITVECTOR_SGE, t, z)),
            nm->mkNode(BITVECTOR_UGT, nm->mkNode(BITVECTOR_SUB, s, t), t));
        scl = nm->mkNode(AND, i1, i2);
      }
    }
  }
  else  /* litk == BITVECTOR_SGT  */
  {
    if (idx == 0)
    {
      Node z = bv::utils::mkZero(w);

      if (pol)
      {
        /* x % s > t
         * with side condition:
         *
         * (and
         *   (and
         *     (=> (bvsgt s z) (bvslt t (bvnot (bvneg s))))
         *     (=> (bvsle s z) (distinct t max)))
         *   (or (distinct t z) (distinct s (_ bv1 w))))
         * where
         * z = 0 with getSize(z) = w
         * and max is the maximum signed value with getSize(max) = w  */
        Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
        Node nt = nm->mkNode(BITVECTOR_NOT, nm->mkNode(BITVECTOR_NEG, s));
        Node i1 = nm->mkNode(IMPLIES,
            nm->mkNode(BITVECTOR_SGT, s, z), nm->mkNode(BITVECTOR_SLT, t, nt));
        Node i2 = nm->mkNode(IMPLIES,
            nm->mkNode(BITVECTOR_SLE, s, z), t.eqNode(max).notNode());
        Node a1 = nm->mkNode(AND, i1, i2);
        Node a2 = nm->mkNode(OR,
            t.eqNode(z).notNode(), s.eqNode(bv::utils::mkOne(w)).notNode());
        scl = nm->mkNode(AND, a1, a2);
      }
      else
      {
        /* x % s <= t
         * with side condition (synthesized):
         * (bvslt ones (bvand (bvneg s) t))
         * where
         * z = 0 with getSize(z) = w
         * and ones = ~0 with getSize(ones) = w  */
        Node a = nm->mkNode(BITVECTOR_AND, nm->mkNode(BITVECTOR_NEG, s), t);
        scl = nm->mkNode(BITVECTOR_SLT, bv::utils::mkOnes(w), a);
      }
    }
    else
    {
      if (pol)
      {
        /* s % x > t
         * with side condition:
         * (and
         *   (=> (bvsge s z) (bvsgt s t))
         *   (=> (bvslt s z)
         *    (bvsgt (bvlshr (bvsub s (_ bv1 w)) (_ bv1 w)) t)))
         * where
         * z = 0 with getSize(z) = w  */
        Node z = bv::utils::mkZero(w);
        Node i1 = nm->mkNode(IMPLIES,
            nm->mkNode(BITVECTOR_SGE, s, z), nm->mkNode(BITVECTOR_SGT, s, t));
        Node shr = nm->mkNode(BITVECTOR_LSHR,
            bv::utils::mkDec(s), bv::utils::mkOne(w));
        Node i2 = nm->mkNode(IMPLIES,
            nm->mkNode(BITVECTOR_SLT, s, z), nm->mkNode(BITVECTOR_SGT, shr, t));
        scl = nm->mkNode(AND, i1, i2);
      }
      else
      {
        /* s % x <= t
         * with side condition (synthesized):
         * (or (bvult t min) (bvsge t s))
         * where
         * min is the minimum signed value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        Node o1 = nm->mkNode(BITVECTOR_ULT, t, min);
        Node o2 = nm->mkNode(BITVECTOR_SGE, t, s);
        scl = nm->mkNode(OR, o1, o2);
      }
    }
  }

  Node scr =
    nm->mkNode(litk, idx == 0 ? nm->mkNode(k, x, s) : nm->mkNode(k, s, x), t);
  Node sc = nm->mkNode(IMPLIES, scl, pol ? scr : scr.notNode());
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

static Node getScBvUdiv(bool pol,
                        Kind litk,
                        Kind k,
                        unsigned idx,
                        Node x,
                        Node s,
                        Node t)
{
  Assert(k == BITVECTOR_UDIV_TOTAL);
  Assert(litk == EQUAL
      || litk == BITVECTOR_ULT || litk == BITVECTOR_SLT
      || litk == BITVECTOR_UGT || litk == BITVECTOR_SGT);

  NodeManager* nm = NodeManager::currentNM();
  unsigned w = bv::utils::getSize(s);
  Assert (w == bv::utils::getSize(t));
  Node scl;
  Node z = bv::utils::mkZero(w);

  if (litk == EQUAL)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x udiv s = t
         * with side condition (synthesized):
         * (= (bvudiv (bvmul s t) s) t)
         *
         * is equivalent to:
         * (or
         *   (and (= t (bvnot z))
         *        (or (= s z) (= s (_ bv1 w))))
         *   (and (distinct t (bvnot z))
         *        (distinct s z)
         *        (not (umulo s t))))
         *
         * where
         * umulo(s, t) is true if s * t produces and overflow
         * and z = 0 with getSize(z) = w  */
        Node mul = nm->mkNode(BITVECTOR_MULT, s, t);
        Node div = nm->mkNode(BITVECTOR_UDIV_TOTAL, mul, s);
        scl = nm->mkNode(EQUAL, div, t);
      }
      else
      {
        /* x udiv s != t
         * with side condition:
         * (or (distinct s z) (distinct t ones))
         * where
         * z = 0 with getSize(z) = w
         * and ones = ~0 with getSize(ones) = w  */
        Node ones = bv::utils::mkOnes(w);
        scl = nm->mkNode(OR, s.eqNode(z).notNode(), t.eqNode(ones).notNode());
      }
    }
    else
    {
      if (pol)
      {
        /* s udiv x = t
         * with side condition (synthesized):
         * (= (bvudiv s (bvudiv s t)) t)
         *
         * is equivalent to:
         * (or
         *   (= s t)
         *   (= t (bvnot z))
         *   (and
         *     (bvuge s t)
         *     (or
         *       (= (bvurem s t) z)
         *       (bvule (bvadd (bvudiv s (bvadd t (_ bv1 w))) (_ bv1 w))
         *              (bvudiv s t)))
         *     (=> (= s (bvnot (_ bv0 8))) (distinct t (_ bv0 8)))))
         *
         * where
         * z = 0 with getSize(z) = w  */
        Node div = nm->mkNode(BITVECTOR_UDIV_TOTAL, s, t);
        scl = nm->mkNode(EQUAL, nm->mkNode(BITVECTOR_UDIV_TOTAL, s, div), t);
      }
      else
      {
        /* s udiv x != t
         * with side condition (w > 1):
         * true (no side condition)
         *
         * with side condition (w == 1):
         * (= (bvand s t) z)
         *
         * where
         * z = 0 with getSize(z) = w  */
        if (w > 1)
        {
          scl = nm->mkConst<bool>(true);
        }
        else
        {
          scl = nm->mkNode(BITVECTOR_AND, s, t).eqNode(z);
        }
      }
    }
  }
  else if (litk == BITVECTOR_ULT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x udiv s < t
         * with side condition (synthesized):
         * (and (bvult z s) (bvult z t))
         * where
         * z = 0 with getSize(z) = w  */
        Node u1 = nm->mkNode(BITVECTOR_ULT, z, s);
        Node u2 = nm->mkNode(BITVECTOR_ULT, z, t);
        scl = nm->mkNode(AND, u1, u2);
      }
      else
      {
        /* x udiv s >= t
         * with side condition (synthesized):
         * (= (bvand (bvudiv (bvmul s t) t) s) s)  */
        Node mul = nm->mkNode(BITVECTOR_MULT, s, t);
        Node div = nm->mkNode(BITVECTOR_UDIV_TOTAL, mul, t);
        scl = nm->mkNode(EQUAL, nm->mkNode(BITVECTOR_AND, div, s), s);
      }
    }
    else
    {
      if (pol)
      {
        /* s udiv x < t
         * with side condition (synthesized):
         * (and (bvult z (bvnot (bvand (bvneg t) s))) (bvult z t))
         * where
         * z = 0 with getSize(z) = w  */
        Node a = nm->mkNode(BITVECTOR_AND, nm->mkNode(BITVECTOR_NEG, t), s);
        Node u1 = nm->mkNode(BITVECTOR_ULT, z, nm->mkNode(BITVECTOR_NOT, a));
        Node u2 = nm->mkNode(BITVECTOR_ULT, z, t);
        scl = nm->mkNode(AND, u1, u2);
      }
      else
      {
        /* s udiv x >= t
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
  }
  else if (litk == BITVECTOR_UGT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x udiv s > t
         * with side condition:
         * (bvugt (bvudiv ones s) t)
         * where
         * ones = ~0 with getSize(ones) = w  */
        Node ones = bv::utils::mkOnes(w);
        Node div = nm->mkNode(BITVECTOR_UDIV_TOTAL, ones, s);
        scl = nm->mkNode(BITVECTOR_UGT, div, t);
      }
      else
      {
        /* x udiv s <= t
         * with side condition (synthesized):
         * (bvuge (bvor s t) (bvnot (bvneg s)))  */
        Node u1 = nm->mkNode(BITVECTOR_OR, s, t);
        Node u2 = nm->mkNode(BITVECTOR_NOT, nm->mkNode(BITVECTOR_NEG, s));
        scl = nm->mkNode(BITVECTOR_UGE, u1, u2);
      }
    }
    else
    {
      if (pol)
      {
        /* s udiv x > t
         * with side condition (synthesized):
         * (bvult t ones)
         * where
         * ones = ~0 with getSize(ones) = w  */
        Node ones = bv::utils::mkOnes(w);
        scl = nm->mkNode(BITVECTOR_ULT, t, ones);
      }
      else
      {
        /* s udiv x <= t
         * with side condition (synthesized):
         * (bvult z (bvor (bvnot s) t))
         * where
         * z = 0 with getSize(z) = w  */
        scl = nm->mkNode(BITVECTOR_ULT,
            z, nm->mkNode(BITVECTOR_OR, nm->mkNode(BITVECTOR_NOT, s), t));
      }
    }
  }
  else if (litk == BITVECTOR_SLT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x udiv s < t
         * with side condition:
         * (=> (bvsle t z) (bvslt (bvudiv min s) t))
         * where
         * z = 0 with getSize(z) = w
         * and min is the minimum signed value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        Node sle = nm->mkNode(BITVECTOR_SLE, t, z);
        Node div = nm->mkNode(BITVECTOR_UDIV_TOTAL, min, s);
        Node slt = nm->mkNode(BITVECTOR_SLT, div, t);
        scl = nm->mkNode(IMPLIES, sle, slt);
      }
      else
      {
        /* x udiv s >= t
         * with side condition:
         * (or
         *   (bvsge (bvudiv ones s) t)
         *   (bvsge (bvudiv max s) t))
         * where
         * ones = ~0 with getSize(ones) = w
         * and max is the maximum signed value with getSize(max) = w  */
        Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
        Node ones = bv::utils::mkOnes(w);
        Node udiv1 = nm->mkNode(BITVECTOR_UDIV_TOTAL, ones, s);
        Node udiv2 = nm->mkNode(BITVECTOR_UDIV_TOTAL, max, s);
        Node sge1 = nm->mkNode(BITVECTOR_SGE, udiv1, t);
        Node sge2 = nm->mkNode(BITVECTOR_SGE, udiv2, t);
        scl = nm->mkNode(OR, sge1, sge2);
      }
    }
    else
    {
      if (pol)
      {
        /* s udiv x < t
         * with side condition (synthesized):
         * (or (bvslt s t) (bvsge t z))
         * where
         * z = 0 with getSize(z) = w  */
        Node slt = nm->mkNode(BITVECTOR_SLT, s, t);
        Node sge = nm->mkNode(BITVECTOR_SGE, t, z);
        scl = nm->mkNode(OR, slt, sge);
      }
      else
      {
        /* s udiv x >= t
         * with side condition (w > 1):
         * (and
         *   (=> (bvsge s z) (bvsge s t))
         *   (=> (bvslt s z) (bvsge (bvlshr s (_ bv1 w)) t)))
         *
         * with side condition (w == 1):
         * (bvsge s t)
         *
         * where
         * z = 0 with getSize(z) = w  */

        if (w > 1)
        {
          Node div = nm->mkNode(BITVECTOR_LSHR, s, bv::utils::mkConst(w, 1));
          Node i1 = nm->mkNode(IMPLIES,
                               nm->mkNode(BITVECTOR_SGE, s, z),
                               nm->mkNode(BITVECTOR_SGE, s, t));
          Node i2 = nm->mkNode(IMPLIES,
                               nm->mkNode(BITVECTOR_SLT, s, z),
                               nm->mkNode(BITVECTOR_SGE, div, t));
          scl = nm->mkNode(AND, i1, i2);
        }
        else
        {
          scl = nm->mkNode(BITVECTOR_SGE, s, t);
        }
      }
    }
  }
  else  /* litk == BITVECTOR_SGT */
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x udiv s > t
         * with side condition:
         * (or
         *   (bvsgt (bvudiv ones s) t)
         *   (bvsgt (bvudiv max s) t))
         * where
         * ones = ~0 with getSize(ones) = w
         * and max is the maximum signed value with getSize(max) = w  */
        Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
        Node ones = bv::utils::mkOnes(w);
        Node div1 = nm->mkNode(BITVECTOR_UDIV_TOTAL, ones, s);
        Node sgt1 = nm->mkNode(BITVECTOR_SGT, div1, t);
        Node div2 = nm->mkNode(BITVECTOR_UDIV_TOTAL, max, s);
        Node sgt2 = nm->mkNode(BITVECTOR_SGT, div2, t);
        scl = nm->mkNode(OR, sgt1, sgt2);
      }
      else
      {
        /* x udiv s <= t
         * with side condition (combination of = and <):
         * (or
         *   (= (bvudiv (bvmul s t) s) t)                ; eq, synthesized
         *   (=> (bvsle t z) (bvslt (bvudiv min s) t)))  ; slt
         * where
         * z = 0 with getSize(z) = w
         * and min is the minimum signed value with getSize(min) = w  */
        Node mul = nm->mkNode(BITVECTOR_MULT, s, t);
        Node div1 = nm->mkNode(BITVECTOR_UDIV_TOTAL, mul, s);
        Node o1 = nm->mkNode(EQUAL, div1, t);
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        Node sle = nm->mkNode(BITVECTOR_SLE, t, z);
        Node div2 = nm->mkNode(BITVECTOR_UDIV_TOTAL, min, s);
        Node slt = nm->mkNode(BITVECTOR_SLT, div2, t);
        Node o2 = nm->mkNode(IMPLIES, sle, slt);
        scl = nm->mkNode(OR, o1, o2);
      }
    }
    else
    {
      if (pol)
      {
        /* s udiv x > t
         * with side condition (w > 1):
         * (and
         *   (=> (bvsge s z) (bvsgt s t))
         *   (=> (bvslt s z) (bvsgt (bvlshr s (_ bv1 w)) t)))
         *
         * with side condition (w == 1):
         * (bvsgt s t)
         *
         * where
         * z = 0 with getSize(z) = w  */
        if (w > 1)
        {
          Node div = nm->mkNode(BITVECTOR_LSHR, s, bv::utils::mkConst(w, 1));
          Node i1 = nm->mkNode(IMPLIES,
                               nm->mkNode(BITVECTOR_SGE, s, z),
                               nm->mkNode(BITVECTOR_SGT, s, t));
          Node i2 = nm->mkNode(IMPLIES,
                               nm->mkNode(BITVECTOR_SLT, s, z),
                               nm->mkNode(BITVECTOR_SGT, div, t));
          scl = nm->mkNode(AND, i1, i2);
        }
        else
        {
          scl = nm->mkNode(BITVECTOR_SGT, s, t);
        }
      }
      else
      {
        /* s udiv x <= t
         * with side condition (synthesized):
         * (not (and (bvslt t (bvnot #x0)) (bvslt t s)))
         * <->
         * (or (bvsge t ones) (bvsge t s))
         * where
         * ones = ~0 with getSize(ones) = w  */
        Node ones = bv::utils::mkOnes(w);
        Node sge1 = nm->mkNode(BITVECTOR_SGE, t, ones);
        Node sge2 = nm->mkNode(BITVECTOR_SGE, t, s);
        scl = nm->mkNode(OR, sge1, sge2);
      }
    }
  }

  Node scr =
    nm->mkNode(litk, idx == 0 ? nm->mkNode(k, x, s) : nm->mkNode(k, s, x), t);
  Node sc = nm->mkNode(IMPLIES, scl, pol ? scr : scr.notNode());
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

static Node getScBvAndOr(bool pol,
                         Kind litk,
                         Kind k,
                         unsigned idx,
                         Node x,
                         Node s,
                         Node t)
{
  Assert (k == BITVECTOR_AND || k == BITVECTOR_OR);
  Assert (litk == EQUAL || litk == BITVECTOR_ULT || litk == BITVECTOR_SLT
          || litk == BITVECTOR_UGT || litk == BITVECTOR_SGT);

  NodeManager* nm = NodeManager::currentNM();
  unsigned w = bv::utils::getSize(s);
  Assert (w == bv::utils::getSize(t));
  Node scl;

  if (litk == EQUAL)
  {
    if (pol)
    {
      /* x & s = t
       * x | s = t
       * with side condition:
       * (= (bvand t s) t)
       * (= (bvor t s) t)  */
      scl = nm->mkNode(EQUAL, t, nm->mkNode(k, t, s));
    }
    else
    {
      if (k == BITVECTOR_AND)
      {
        /* x & s = t
         * with side condition:
         * (or (distinct s z) (distinct t z))
         * where
         * z = 0 with getSize(z) = w  */
        Node z = bv::utils::mkZero(w);
        scl = nm->mkNode(OR, s.eqNode(z).notNode(), t.eqNode(z).notNode());
      }
      else
      {
        /* x | s = t
         * with side condition:
         * (or (distinct s ones) (distinct t ones))
         * where
         * ones = ~0 with getSize(ones) = w  */
        Node n = bv::utils::mkOnes(w);
        scl = nm->mkNode(OR, s.eqNode(n).notNode(), t.eqNode(n).notNode());
      }
    }
  }
  else if (litk == BITVECTOR_ULT)
  {
    if (pol)
    {
      if (k == BITVECTOR_AND)
      {
        /* x & s < t
         * with side condition (synthesized):
         * (distinct t z)
         * where
         * z = 0 with getSize(z) = 0  */
        Node z = bv::utils::mkZero(w);
        scl = t.eqNode(z).notNode();
      }
      else
      {
        /* x | s < t
         * with side condition (synthesized):
         * (bvult s t)  */
        scl = nm->mkNode(BITVECTOR_ULT, s, t);
      }
    }
    else
    {
      if (k == BITVECTOR_AND)
      {
        /* x & s >= t
         * with side condition (synthesized):
         * (bvuge s t)  */
        scl = nm->mkNode(BITVECTOR_UGE, s, t);
      }
      else
      {
        /* x | s >= t
         * with side condition (synthesized):
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
  }
  else if (litk == BITVECTOR_UGT)
  {
    if (pol)
    {
      if (k == BITVECTOR_AND)
      {
        /* x & s > t
         * with side condition (synthesized):
         * (bvult t s)  */
        scl = nm->mkNode(BITVECTOR_ULT, t, s);
      }
      else
      {
        /* x | s > t
         * with side condition (synthesized):
         * (bvult t ones)
         * where
         * ones = ~0 with getSize(ones) = w  */
        scl = nm->mkNode(BITVECTOR_ULT, t, bv::utils::mkOnes(w));
      }
    }
    else
    {
      if (k == BITVECTOR_AND)
      {
        /* x & s <= t
         * with side condition (synthesized):
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
      else
      {
        /* x | s <= t
         * with side condition (synthesized):
         * (bvuge t s)  */
        scl = nm->mkNode(BITVECTOR_UGE, t, s);
      }
    }
  }
  else if (litk == BITVECTOR_SLT)
  {
    if (pol)
    {
      if (k == BITVECTOR_AND)
      {
        /* x & s < t
         * with side condition (synthesized):
         * (bvslt (bvand (bvnot (bvneg t)) s) t)  */
        Node nnt = nm->mkNode(BITVECTOR_NOT, nm->mkNode(BITVECTOR_NEG, t));
        scl = nm->mkNode(BITVECTOR_SLT, nm->mkNode(BITVECTOR_AND, nnt, s), t);
      }
      else
      {
        /* x | s < t
         * with side condition (synthesized):
         * (bvslt (bvor (bvnot (bvsub s t)) s) t)  */
        Node st = nm->mkNode(BITVECTOR_NOT, nm->mkNode(BITVECTOR_SUB, s, t));
        scl = nm->mkNode(BITVECTOR_SLT, nm->mkNode(BITVECTOR_OR, st, s), t);
      }
    }
    else
    {
      if (k == BITVECTOR_AND)
      {
        /* x & s >= t
         * with side condition (case = combined with synthesized bvsgt):
         * (or
         *  (= (bvand s t) t)
         *  (bvslt t (bvand (bvsub t s) s)))  */
        Node sc_sgt = nm->mkNode(
            BITVECTOR_SLT,
            t,
            nm->mkNode(BITVECTOR_AND, nm->mkNode(BITVECTOR_SUB, t, s), s));
        Node sc_eq = nm->mkNode(BITVECTOR_AND, s, t).eqNode(t);
        scl = sc_eq.orNode(sc_sgt);
      }
      else
      {
        /* x | s >= t
         * with side condition (synthesized):
         * (bvsge s (bvand s t))  */
        scl = nm->mkNode(BITVECTOR_SGE, s, nm->mkNode(BITVECTOR_AND, s, t));
      }
    }
  }
  else
  {
    Assert(litk == BITVECTOR_SGT);
    if (pol)
    {
      /* x & s > t
       * x | s > t
       * with side condition (synthesized):
       * (bvslt t (bvand s max))
       * (bvslt t (bvor s max))
       * where
       * max is the signed maximum value with getSize(max) = w  */
      Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
      scl = nm->mkNode(BITVECTOR_SLT, t, nm->mkNode(k, s, max));
    }
    else
    {
      if (k == BITVECTOR_AND)
      {
        /* x & s <= t
         * with side condition (synthesized):
         * (bvuge s (bvand t min))
         * where
         * min is the signed minimum value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        scl = nm->mkNode(BITVECTOR_UGE, s, nm->mkNode(BITVECTOR_AND, t, min));
      }
      else
      {
        /* x | s <= t
         * with side condition (synthesized):
         * (bvsge t (bvor s min))
         * where
         * min is the signed minimum value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        scl = nm->mkNode(BITVECTOR_SGE, t, nm->mkNode(BITVECTOR_OR, s, min));
      }
    }
  }
  Node scr = nm->mkNode(litk, nm->mkNode(k, x, s), t);
  Node sc = nm->mkNode(IMPLIES, scl, pol ? scr : scr.notNode());
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

static Node defaultShiftSc(Kind litk, Kind shk, Node s, Node t)
{
  unsigned w;
  NodeBuilder<> nb(OR);
  NodeManager *nm;

  nm = NodeManager::currentNM();

  w = bv::utils::getSize(s);
  Assert(w == bv::utils::getSize(t));

  nb << nm->mkNode(litk, s, t);
  for (unsigned i = 1; i <= w; i++)
  {
    Node sw = bv::utils::mkConst(w, i);
    nb << nm->mkNode(litk, nm->mkNode(shk, s, sw), t);
  }
  if (nb.getNumChildren() == 1)
    return nb[0];
  return nb.constructNode();
}


static Node getScBvLshr(bool pol,
                        Kind litk,
                        Kind k,
                        unsigned idx,
                        Node x,
                        Node s,
                        Node t)
{
  Assert(k == BITVECTOR_LSHR);
  Assert(litk == EQUAL || litk == BITVECTOR_ULT || litk == BITVECTOR_SLT
         || litk == BITVECTOR_UGT || litk == BITVECTOR_SGT);

  NodeManager* nm = NodeManager::currentNM();
  Node scl;
  unsigned w = bv::utils::getSize(s);
  Assert(w == bv::utils::getSize(t));
  Node z = bv::utils::mkZero(w);

  if (litk == EQUAL)
  {
    if (idx == 0)
    {
      Node ww = bv::utils::mkConst(w, w);

      if (pol)
      {
        /* x >> s = t
         * with side condition (synthesized):
         * (= (bvlshr (bvshl t s) s) t)  */
        Node shl = nm->mkNode(BITVECTOR_SHL, t, s);
        Node lshr = nm->mkNode(BITVECTOR_LSHR, shl, s);
        scl = lshr.eqNode(t);
      }
      else
      {
        /* x >> s != t
         * with side condition:
         * (or (distinct t z) (bvult s w))
         * where
         * z = 0 with getSize(z) = w
         * and w = getSize(s) = getSize(t)  */
        scl = nm->mkNode(OR,
            t.eqNode(z).notNode(),
            nm->mkNode(BITVECTOR_ULT, s, ww));
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x = t
         * with side condition:
         * (or (= (bvlshr s i) t) ...)
         * for i in 0..w  */
        scl = defaultShiftSc(EQUAL, BITVECTOR_LSHR, s, t);
      }
      else
      {
        /* s >> x != t
         * with side condition:
         * (or (distinct s z) (distinct t z))
         * where
         * z = 0 with getSize(z) = w  */
        scl = nm->mkNode(OR, s.eqNode(z).notNode(), t.eqNode(z).notNode());
      }
    }
  }
  else if (litk == BITVECTOR_ULT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x >> s < t
         * with side condition (synthesized):
         * (distinct t z)
         * where
         * z = 0 with getSize(z) = w  */
        scl = t.eqNode(z).notNode();
      }
      else
      {
        /* x >> s >= t
         * with side condition (synthesized):
         * (= (bvlshr (bvshl t s) s) t)  */
        Node ts = nm->mkNode(BITVECTOR_SHL, t, s);
        scl = nm->mkNode(BITVECTOR_LSHR, ts, s).eqNode(t);
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x < t
         * with side condition (synthesized):
         * (distinct t z)
         * where
         * z = 0 with getSize(z) = w  */
        scl = t.eqNode(z).notNode();
      }
      else
      {
        /* s >> x >= t
         * with side condition (synthesized):
         * (bvuge s t)  */
        scl = nm->mkNode(BITVECTOR_UGE, s, t);
      }
    }
  }
  else if (litk == BITVECTOR_UGT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x >> s > t
         * with side condition (synthesized):
         * (bvult t (bvlshr (bvnot s) s))  */
        Node lshr = nm->mkNode(BITVECTOR_LSHR, nm->mkNode(BITVECTOR_NOT, s), s);
        scl = nm->mkNode(BITVECTOR_ULT, t, lshr);
      }
      else
      {
        /* x >> s <= t
         * with side condition:
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x > t
         * with side condition (synthesized):
         * (bvult t s)  */
        scl = nm->mkNode(BITVECTOR_ULT, t, s);
      }
      else
      {
        /* s >> x <= t
         * with side condition:
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
  }
  else if (litk == BITVECTOR_SLT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x >> s < t
         * with side condition (synthesized):
         * (bvslt (bvlshr (bvnot (bvneg t)) s) t)  */
        Node nnt = nm->mkNode(BITVECTOR_NOT, nm->mkNode(BITVECTOR_NEG, t));
        Node lshr = nm->mkNode(BITVECTOR_LSHR, nnt, s);
        scl = nm->mkNode(BITVECTOR_SLT, lshr, t);
      }
      else
      {
        /* x >> s >= t
         * with side condition:
         * (=> (not (= s z)) (bvsge (bvlshr ones s) t))
         * where
         * z = 0 with getSize(z) = w
         * and ones = ~0 with getSize(ones) = w  */
        Node ones = bv::utils::mkOnes(w);
        Node lshr = nm->mkNode(BITVECTOR_LSHR, ones, s);
        Node nz = s.eqNode(z).notNode();
        scl = nz.impNode(nm->mkNode(BITVECTOR_SGE, lshr, t));
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x < t
         * with side condition (synthesized):
         * (or (bvslt s t) (bvslt z t))
         * where
         * z = 0 with getSize(z) = w  */
        Node st = nm->mkNode(BITVECTOR_SLT, s, t);
        Node zt = nm->mkNode(BITVECTOR_SLT, z, t);
        scl = st.orNode(zt);
      }
      else
      {
        /* s >> x >= t
         * with side condition:
         * (and
         *  (=> (bvslt s z) (bvsge (bvlshr s (_ bv1 w)) t))
         *  (=> (bvsge s z) (bvsge s t)))
         * where
         * z = 0 with getSize(z) = w  */
        Node one = bv::utils::mkConst(w, 1);
        Node sz = nm->mkNode(BITVECTOR_SLT, s, z);
        Node lshr = nm->mkNode(BITVECTOR_LSHR, s, one);
        Node sge1 = nm->mkNode(BITVECTOR_SGE, lshr, t);
        Node sge2 = nm->mkNode(BITVECTOR_SGE, s, t);
        scl = sz.impNode(sge1).andNode(sz.notNode().impNode(sge2));
      }
    }
  }
  else
  {
    Assert(litk == BITVECTOR_SGT);
    if (idx == 0)
    {
      if (pol)
      {
        /* x >> s > t
         * with side condition (synthesized):
         * (bvslt t (bvlshr (bvshl max s) s))
         * where
         * max is the signed maximum value with getSize(max) = w  */
        Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
        Node shl = nm->mkNode(BITVECTOR_SHL, max, s);
        Node lshr = nm->mkNode(BITVECTOR_LSHR, shl, s);
        scl = nm->mkNode(BITVECTOR_SLT, t, lshr);
      }
      else
      {
        /* x >> s <= t
         * with side condition (synthesized):
         * (bvsge t (bvlshr t s))  */
        scl = nm->mkNode(BITVECTOR_SGE, t, nm->mkNode(BITVECTOR_LSHR, t, s));
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x > t
         * with side condition:
         * (and
         *  (=> (bvslt s z) (bvsgt (bvlshr s one) t))
         *  (=> (bvsge s z) (bvsgt s t)))
         * where
         * z = 0 and getSize(z) = w  */
        Node one = bv::utils::mkOne(w);
        Node sz = nm->mkNode(BITVECTOR_SLT, s, z);
        Node lshr = nm->mkNode(BITVECTOR_LSHR, s, one);
        Node sgt1 = nm->mkNode(BITVECTOR_SGT, lshr, t);
        Node sgt2 = nm->mkNode(BITVECTOR_SGT, s, t);
        scl = sz.impNode(sgt1).andNode(sz.notNode().impNode(sgt2));
      }
      else
      {
        /* s >> x <= t
         * with side condition (synthesized):
         * (or (bvult t min) (bvsge t s))
         * where
         * min is the minimum signed value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        Node ult = nm->mkNode(BITVECTOR_ULT, t, min);
        Node sge = nm->mkNode(BITVECTOR_SGE, t, s);
        scl = ult.orNode(sge);
      }
    }
  }
  Node scr =
    nm->mkNode(litk, idx == 0 ? nm->mkNode(k, x, s) : nm->mkNode(k, s, x), t);
  Node sc = nm->mkNode(IMPLIES, scl, pol ? scr : scr.notNode());
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

static Node getScBvAshr(bool pol,
                        Kind litk,
                        Kind k,
                        unsigned idx,
                        Node x,
                        Node s,
                        Node t)
{
  Assert(k == BITVECTOR_ASHR);
  Assert(litk == EQUAL || litk == BITVECTOR_ULT || litk == BITVECTOR_SLT
         || litk == BITVECTOR_UGT || litk == BITVECTOR_SGT);

  NodeManager* nm = NodeManager::currentNM();
  Node scl;
  unsigned w = bv::utils::getSize(s);
  Assert(w == bv::utils::getSize(t));
  Node z = bv::utils::mkZero(w);
  Node n = bv::utils::mkOnes(w);

  if (litk == EQUAL)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x >> s = t
         * with side condition:
         * (and
         *  (=> (bvult s w) (= (bvashr (bvshl t s) s) t))
         *  (=> (bvuge s w) (or (= t ones) (= t z)))
         * )
         * where
         * z = 0 with getSize(z) = w
         * and ones = ~0 with getSize(ones) = w
         * and w = getSize(t) = getSize(s)  */
        Node ww = bv::utils::mkConst(w, w);
        Node shl = nm->mkNode(BITVECTOR_SHL, t, s);
        Node ashr = nm->mkNode(BITVECTOR_ASHR, shl, s);
        Node ult = nm->mkNode(BITVECTOR_ULT, s, ww);
        Node imp1 = ult.impNode(ashr.eqNode(t));
        Node to = t.eqNode(n);
        Node tz = t.eqNode(z);
        Node imp2 = ult.notNode().impNode(to.orNode(tz));
        scl = imp1.andNode(imp2);
      }
      else
      {
        /* x >> s != t
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x = t
         * with side condition:
         * (or (= (bvashr s i) t) ...)
         * for i in 0..w  */
        scl = defaultShiftSc(EQUAL, BITVECTOR_ASHR, s, t);
      }
      else
      {
        /* s >> x != t
         * with side condition:
         * (and
         *  (or (not (= t z)) (not (= s z)))
         *  (or (not (= t ones)) (not (= s ones))))
         * where
         * z = 0 with getSize(z) = w
         * and ones = ~0 with getSize(ones) = w  */
        scl = nm->mkNode(AND,
            nm->mkNode(OR, t.eqNode(z).notNode(), s.eqNode(z).notNode()),
            nm->mkNode(OR, t.eqNode(n).notNode(), s.eqNode(n).notNode()));
      }
    }
  }
  else if (litk == BITVECTOR_ULT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x >> s < t
         * with side condition (synthesized):
         * (distinct t z)
         * where
         * z = 0 with getSize(z) = w  */
        scl = t.eqNode(z).notNode();
      }
      else
      {
        /* x >> s >= t
         * with side condition (synthesized):
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x < t
         * with side condition (synthesized):
         * (and (not (and (bvuge s t) (bvslt s z))) (not (= t z)))
         * where
         * z = 0 with getSize(z) = w  */
        Node st = nm->mkNode(BITVECTOR_UGE, s, t);
        Node sz = nm->mkNode(BITVECTOR_SLT, s, z);
        Node tz = t.eqNode(z).notNode();
        scl = st.andNode(sz).notNode().andNode(tz);
      }
      else
      {
        /* s >> x >= t
         * with side condition (synthesized):
         * (not (and (bvult s (bvnot s)) (bvult s t)))  */
        Node ss = nm->mkNode(BITVECTOR_ULT, s, nm->mkNode(BITVECTOR_NOT, s));
        Node st = nm->mkNode(BITVECTOR_ULT, s, t);
        scl = ss.andNode(st).notNode();
      }
    }
  }
  else if (litk == BITVECTOR_UGT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x >> s > t
         * with side condition (synthesized):
         * (bvult t ones)
         * where
         * ones = ~0 with getSize(ones) = w  */
        scl = nm->mkNode(BITVECTOR_ULT, t, bv::utils::mkOnes(w));
      }
      else
      {
        /* x >> s <= t
         * with side condition (synthesized):
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x > t
         * with side condition (synthesized):
         * (or (bvslt s (bvlshr s (bvnot t))) (bvult t s))  */
        Node lshr = nm->mkNode(BITVECTOR_LSHR, s, nm->mkNode(BITVECTOR_NOT, t));
        Node ts = nm->mkNode(BITVECTOR_ULT, t, s);
        Node slt = nm->mkNode(BITVECTOR_SLT, s, lshr);
        scl = slt.orNode(ts);
      }
      else
      {
        /* s >> x <= t
         * with side condition (synthesized):
         * (or (bvult s min) (bvuge t s))
         * where
         * min is the minimum signed value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        Node ult = nm->mkNode(BITVECTOR_ULT, s, min);
        Node uge = nm->mkNode(BITVECTOR_UGE, t, s);
        scl = ult.orNode(uge);
      }
    }
  }
  else if (litk == BITVECTOR_SLT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x >> s < t
         * with side condition:
         * (bvslt (bvashr min s) t)
         * where
         * min is the minimum signed value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        scl = nm->mkNode(BITVECTOR_SLT, nm->mkNode(BITVECTOR_ASHR, min, s), t);
      }
      else
      {
        /* x >> s >= t
         * with side condition:
         * (bvsge (bvlshr max s) t)
         * where
         * max is the signed maximum value with getSize(max) = w  */
        Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
        scl = nm->mkNode(BITVECTOR_SGE, nm->mkNode(BITVECTOR_LSHR, max, s), t);
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x < t
         * with side condition (synthesized):
         * (or (bvslt s t) (bvslt z t))
         * where
         * z = 0 and getSize(z) = w  */
        Node st = nm->mkNode(BITVECTOR_SLT, s, t);
        Node zt = nm->mkNode(BITVECTOR_SLT, z, t);
        scl = st.orNode(zt);
      }
      else
      {
        /* s >> x >= t
         * with side condition (synthesized):
         * (not (and (bvult t (bvnot t)) (bvslt s t)))  */
        Node tt = nm->mkNode(BITVECTOR_ULT, t, nm->mkNode(BITVECTOR_NOT, t));
        Node st = nm->mkNode(BITVECTOR_SLT, s, t);
        scl = tt.andNode(st).notNode();
      }
    }
  }
  else
  {
    Assert(litk == BITVECTOR_SGT);
    Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
    if (idx == 0)
    {
      Node lshr = nm->mkNode(BITVECTOR_LSHR, max, s);
      if (pol)
      {
        /* x >> s > t
         * with side condition (synthesized):
         * (bvslt t (bvlshr max s)))
         * where
         * max is the signed maximum value with getSize(max) = w  */
        scl = nm->mkNode(BITVECTOR_SLT, t, lshr);
      }
      else
      {
        /* x >> s <= t
         * with side condition (synthesized):
         * (bvsge t (bvnot (bvlshr max s)))
         * where
         * max is the signed maximum value with getSize(max) = w  */
        scl = nm->mkNode(BITVECTOR_SGE, t, nm->mkNode(BITVECTOR_NOT, lshr));
      }
    }
    else
    {
      if (pol)
      {
        /* s >> x > t
         * with side condition (synthesized):
         * (and (bvslt t (bvand s max)) (bvslt t (bvor s max)))
         * where
         * max is the signed maximum value with getSize(max) = w  */
        Node sam = nm->mkNode(BITVECTOR_AND, s, max);
        Node som = nm->mkNode(BITVECTOR_OR, s, max);
        Node slta = nm->mkNode(BITVECTOR_SLT, t, sam);
        Node slto = nm->mkNode(BITVECTOR_SLT, t, som);
        scl = slta.andNode(slto);
      }
      else
      {
        /* s >> x <= t
         * with side condition (synthesized):
         * (or (bvsge t z) (bvsge t s))
         * where
         * z = 0 and getSize(z) = w  */
        Node tz = nm->mkNode(BITVECTOR_SGE, t, z);
        Node ts = nm->mkNode(BITVECTOR_SGE, t, s);
        scl = tz.orNode(ts);
      }
    }
  }
  Node scr =
      nm->mkNode(litk, idx == 0 ? nm->mkNode(k, x, s) : nm->mkNode(k, s, x), t);
  Node sc = nm->mkNode(IMPLIES, scl, pol ? scr : scr.notNode());
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

static Node getScBvShl(bool pol,
                       Kind litk,
                       Kind k,
                       unsigned idx,
                       Node x,
                       Node s,
                       Node t)
{
  Assert(k == BITVECTOR_SHL);
  Assert(litk == EQUAL || litk == BITVECTOR_ULT || litk == BITVECTOR_SLT
         || litk == BITVECTOR_UGT || litk == BITVECTOR_SGT);

  NodeManager* nm = NodeManager::currentNM();
  Node scl;
  unsigned w = bv::utils::getSize(s);
  Assert(w == bv::utils::getSize(t));
  Node z = bv::utils::mkZero(w);

  if (litk == EQUAL)
  {
    if (idx == 0)
    {
      Node ww = bv::utils::mkConst(w, w);

      if (pol)
      {
        /* x << s = t
         * with side condition (synthesized):
         * (= (bvshl (bvlshr t s) s) t)  */
        Node lshr = nm->mkNode(BITVECTOR_LSHR, t, s);
        Node shl = nm->mkNode(BITVECTOR_SHL, lshr, s);
        scl = shl.eqNode(t);
      }
      else
      {
        /* x << s != t
         * with side condition:
         * (or (distinct t z) (bvult s w))
         * with
         * w = getSize(s) = getSize(t)
         * and z = 0 with getSize(z) = w  */
        scl = nm->mkNode(OR,
            t.eqNode(z).notNode(),
            nm->mkNode(BITVECTOR_ULT, s, ww));
      }
    }
    else
    {
      if (pol)
      {
        /* s << x = t
         * with side condition:
         * (or (= (bvshl s i) t) ...)
         * for i in 0..w  */
        scl = defaultShiftSc(EQUAL, BITVECTOR_SHL, s, t);
      }
      else
      {
        /* s << x != t
         * with side condition:
         * (or (distinct s z) (distinct t z))
         * where
         * z = 0 with getSize(z) = w  */
        scl = nm->mkNode(OR, s.eqNode(z).notNode(), t.eqNode(z).notNode());
      }
    }
  }
  else if (litk == BITVECTOR_ULT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x << s < t
         * with side condition (synthesized):
         * (not (= t z))  */
        scl = t.eqNode(z).notNode();
      }
      else
      {
        /* x << s >= t
         * with side condition (synthesized):
         * (bvuge (bvshl ones s) t)  */
        Node shl = nm->mkNode(BITVECTOR_SHL, bv::utils::mkOnes(w), s);
        scl = nm->mkNode(BITVECTOR_UGE, shl, t);
      }
    }
    else
    {
      if (pol)
      {
        /* s << x < t
         * with side condition (synthesized):
         * (not (= t z))  */
        scl = t.eqNode(z).notNode();
      }
      else
      {
        /* s << x >= t
         * with side condition:
         * (or (bvuge (bvshl s i) t) ...)
         * for i in 0..w  */
        scl = defaultShiftSc(BITVECTOR_UGE, BITVECTOR_SHL, s, t);
      }
    }
  }
  else if (litk == BITVECTOR_UGT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x << s > t
         * with side condition (synthesized):
         * (bvult t (bvshl ones s))
         * where
         * ones = ~0 with getSize(ones) = w  */
        Node shl = nm->mkNode(BITVECTOR_SHL, bv::utils::mkOnes(w), s);
        scl = nm->mkNode(BITVECTOR_ULT, t, shl);
      }
      else
      {
        /* x << s <= t
         * with side condition:
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
    else
    {
      if (pol)
      {
        /* s << x > t
         * with side condition:
         * (or (bvugt (bvshl s i) t) ...)
         * for i in 0..w  */
        scl = defaultShiftSc(BITVECTOR_UGT, BITVECTOR_SHL, s, t);
      }
      else
      {
        /* s << x <= t
         * with side condition:
         * true (no side condition)  */
        scl = nm->mkConst<bool>(true);
      }
    }
  }
  else if (litk == BITVECTOR_SLT)
  {
    if (idx == 0)
    {
      if (pol)
      {
        /* x << s < t
         * with side condition (synthesized):
         * (bvslt (bvshl (bvlshr min s) s) t)
         * where
         * min is the signed minimum value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        Node lshr = nm->mkNode(BITVECTOR_LSHR, min, s);
        Node shl = nm->mkNode(BITVECTOR_SHL, lshr, s);
        scl = nm->mkNode(BITVECTOR_SLT, shl, t);
      }
      else
      {
        /* x << s >= t
         * with side condition (synthesized):
         * (bvsge (bvand (bvshl max s) max) t)
         * where
         * max is the signed maximum value with getSize(max) = w  */
        Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
        Node shl = nm->mkNode(BITVECTOR_SHL, max, s);
        scl = nm->mkNode(BITVECTOR_SGE, nm->mkNode(BITVECTOR_AND, shl, max), t);
      }
    }
    else
    {
      if (pol)
      {
        /* s << x < t
         * with side condition (synthesized):
         * (bvult (bvshl min s) (bvadd t min))
         * where
         * min is the signed minimum value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        Node shl = nm->mkNode(BITVECTOR_SHL, min, s);
        Node add = nm->mkNode(BITVECTOR_PLUS, t, min);
        scl = nm->mkNode(BITVECTOR_ULT, shl, add);
      }
      else
      {
        /* s << x >= t
         * with side condition:
         * (or (bvsge (bvshl s i) t) ...)
         * for i in 0..w  */
        scl = defaultShiftSc(BITVECTOR_SGE, BITVECTOR_SHL, s, t);
      }
    }
  }
  else
  {
    Assert(litk == BITVECTOR_SGT);
    if (idx == 0)
    {
      if (pol)
      {
        /* x << s > t
         * with side condition (synthesized):
         * (bvslt t (bvand (bvshl max s) max))
         * where
         * max is the signed maximum value with getSize(max) = w  */
        Node max = bv::utils::mkConst(bv::utils::mkBitVectorMaxSigned(w));
        Node shl = nm->mkNode(BITVECTOR_SHL, max, s);
        scl = nm->mkNode(BITVECTOR_SLT, t, nm->mkNode(BITVECTOR_AND, shl, max));
      }
      else
      {
        /* x << s <= t
         * with side condition (synthesized):
         * (bvult (bvlshr t (bvlshr t s)) min)
         * where
         * min is the signed minimum value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        Node ts = nm->mkNode(BITVECTOR_LSHR, t, s);
        scl = nm->mkNode(BITVECTOR_ULT, nm->mkNode(BITVECTOR_LSHR, t, ts), min);
      }
    }
    else
    {
      if (pol)
      {
        /* s << x > t
         * with side condition:
         * (or (bvsgt (bvshl s i) t) ...)
         * for i in 0..w  */
        scl = defaultShiftSc(BITVECTOR_SGT, BITVECTOR_SHL, s, t);
      }
      else
      {
        /* s << x <= t
         * with side condition (synthesized):
         * (bvult (bvlshr t s) min)
         * where
         * min is the signed minimum value with getSize(min) = w  */
        Node min = bv::utils::mkConst(bv::utils::mkBitVectorMinSigned(w));
        scl = nm->mkNode(BITVECTOR_ULT, nm->mkNode(BITVECTOR_LSHR, t, s), min);
      }
    }
  }
  Node scr =
    nm->mkNode(litk, idx == 0 ? nm->mkNode(k, x, s) : nm->mkNode(k, s, x), t);
  Node sc = nm->mkNode(IMPLIES, scl, pol ? scr : scr.notNode());
  Trace("bv-invert") << "Add SC_" << k << "(" << x << "): " << sc << std::endl;
  return sc;
}

Node BvInverter::solveBvLit(Node sv,
                            Node lit,
                            std::vector<unsigned>& path,
                            BvInverterQuery* m)
{
  Assert(!path.empty());

  bool pol = true;
  unsigned index, nchildren;
  NodeManager* nm = NodeManager::currentNM();
  Kind k, litk;

  Assert(!path.empty());
  index = path.back();
  Assert(index < lit.getNumChildren());
  path.pop_back();
  litk = k = lit.getKind();

  /* Note: option --bool-to-bv is currently disabled when CBQI BV
   *       is enabled. We currently do not support Boolean operators
   *       that are interpreted as bit-vector operators of width 1.  */

  /* Boolean layer ----------------------------------------------- */

  if (k == NOT)
  {
    pol = !pol;
    lit = lit[index];
    Assert(!path.empty());
    index = path.back();
    Assert(index < lit.getNumChildren());
    path.pop_back();
    litk = k = lit.getKind();
  }

  Assert(k == EQUAL
      || k == BITVECTOR_ULT
      || k == BITVECTOR_SLT);

  Node sv_t = lit[index];
  Node t = lit[1-index];
  if (litk == BITVECTOR_ULT && index == 1)
  {
    litk = BITVECTOR_UGT;
  }
  else if (litk == BITVECTOR_SLT && index == 1)
  {
    litk = BITVECTOR_SGT;
  }

  /* Bit-vector layer -------------------------------------------- */

  while (!path.empty())
  {
    index = path.back();
    Assert(index < sv_t.getNumChildren());
    path.pop_back();
    k = sv_t.getKind();
    nchildren = sv_t.getNumChildren();

    if (k == BITVECTOR_NOT || k == BITVECTOR_NEG)
    {
      t = nm->mkNode(k, t);
    }
    else if (k == BITVECTOR_CONCAT)
    {
      /* x = t[upper:lower]
       * where
       * upper = getSize(t) - 1 - sum(getSize(sv_t[i])) for i < index
       * lower = getSize(sv_t[i]) for i > index  */
      unsigned upper, lower;
      upper = bv::utils::getSize(t) - 1;
      lower = 0;
      NodeBuilder<> nb(nm, BITVECTOR_CONCAT);
      for (unsigned i = 0; i < nchildren; i++)
      {
        if (i < index) { upper -= bv::utils::getSize(sv_t[i]); }
        else if (i > index) { lower += bv::utils::getSize(sv_t[i]); }
      }
      t = bv::utils::mkExtract(t, upper, lower);
    }
    else if (k == BITVECTOR_SIGN_EXTEND)
    {
      t = bv::utils::mkExtract(t, bv::utils::getSize(sv_t[index]) - 1, 0);
    }
    else if (k == BITVECTOR_EXTRACT || k == BITVECTOR_COMP)
    {
      Trace("bv-invert") << "bv-invert : Unsupported for index " << index
                         << ", from " << sv_t << std::endl;
      return Node::null();
    }
    else
    {
      Assert(nchildren >= 2);
      Node s = nchildren == 2 ? sv_t[1 - index] : dropChild(sv_t, index);
      Node t_new;
      /* Note: All n-ary kinds except for CONCAT (i.e., AND, OR, MULT, PLUS)
       *       are commutative (no case split based on index). */

      // handle cases where the inversion has a unique solution
      if (k == BITVECTOR_PLUS)
      {
        t_new = nm->mkNode(BITVECTOR_SUB, t, s);
      }
      else if (k == BITVECTOR_XOR)
      {
        t_new = nm->mkNode(BITVECTOR_XOR, t, s);
      }
      else if (k == BITVECTOR_MULT)
      {
        if (s.isConst() && bv::utils::getBit(s, 0))
        {
          unsigned ssize = bv::utils::getSize(s);
          Integer a = s.getConst<BitVector>().toInteger();
          Integer w = Integer(1).multiplyByPow2(ssize);
          Trace("bv-invert-debug")
              << "Compute inverse : " << a << " " << w << std::endl;
          Integer inv = a.modInverse(w);
          Trace("bv-invert-debug") << "Inverse : " << inv << std::endl;
          Node inv_val = nm->mkConst(BitVector(ssize, inv));
          t_new = nm->mkNode(BITVECTOR_MULT, inv_val, t);
        }
      }

      if (!t_new.isNull())
      {
        // In this case, s op x = t is equivalent to x = t_new
        t = t_new;
      }
      else
      {
        TypeNode solve_tn = sv_t[index].getType();
        Node sc;

        switch (k)
        {
          case BITVECTOR_MULT:
            sc = getScBvMult(
                pol, litk, k, index, getSolveVariable(solve_tn), s, t);
            break;

          case BITVECTOR_SHL:
            sc = getScBvShl(
                pol, litk, k, index, getSolveVariable(solve_tn), s, t);
            break;

          case BITVECTOR_UREM_TOTAL:
            sc = getScBvUrem(
                pol, litk, k, index, getSolveVariable(solve_tn), s, t);
            break;

          case BITVECTOR_UDIV_TOTAL:
            sc = getScBvUdiv(
                pol, litk, k, index, getSolveVariable(solve_tn), s, t);
            break;

          case BITVECTOR_AND:
          case BITVECTOR_OR:
            sc = getScBvAndOr(
                pol, litk, k, index, getSolveVariable(solve_tn), s, t);
            break;

          case BITVECTOR_LSHR:
            sc = getScBvLshr(
                pol, litk, k, index, getSolveVariable(solve_tn), s, t);
            break;

          case BITVECTOR_ASHR:
            sc = getScBvAshr(
                pol, litk, k, index, getSolveVariable(solve_tn), s, t);
            break;

          default:
            Trace("bv-invert") << "bv-invert : Unknown kind " << k
                               << " for bit-vector term " << sv_t << std::endl;
            return Node::null();
        }
        Assert(!sc.isNull());
        /* We generate a choice term (choice x0. SC => x0 <k> s <litk> t) for
         * x <k> s <litk> t. When traversing down, this choice term determines
         * the value for x <k> s = (choice x0. SC => x0 <k> s <litk> t), i.e.,
         * from here on, the propagated literal is a positive equality. */
        litk = EQUAL;
        pol = true;
        /* t = fresh skolem constant */
        t = getInversionNode(sc, solve_tn, m);
        if (t.isNull())
        {
          return t;
        }
      }
    }
    sv_t = sv_t[index];
  }
  Assert(sv_t == sv);
  if (litk == BITVECTOR_ULT || litk == BITVECTOR_UGT)
  {
    TypeNode solve_tn = sv_t.getType();
    Node sc = getScBvUltUgt(pol, litk, getSolveVariable(solve_tn), t);
    t = getInversionNode(sc, solve_tn, m);
  }
  else if (litk == BITVECTOR_SLT || litk == BITVECTOR_SGT)
  {
    TypeNode solve_tn = sv_t.getType();
    Node sc = getScBvSltSgt(pol, litk, getSolveVariable(solve_tn), t);
    t = getInversionNode(sc, solve_tn, m);
  }
  else if (pol == false)
  {
    Assert (litk == EQUAL);
    TypeNode solve_tn = sv_t.getType();
    Node x = getSolveVariable(solve_tn);
    Node sc = nm->mkNode(DISTINCT, x, t);
    Trace("bv-invert") << "Add SC_" << litk << "(" << x << "): " << sc
                       << std::endl;
    t = getInversionNode(sc, solve_tn, m);
  }
  return t;
}

/*---------------------------------------------------------------------------*/

}  // namespace quantifiers
}  // namespace theory
}  // namespace CVC4
