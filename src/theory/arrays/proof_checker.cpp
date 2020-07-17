/*********************                                                        */
/*! \file proof_checker.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Haniel Barbosa
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2020 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved.  See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief Implementation of arrays proof checker
 **/

#include "theory/arrays/proof_checker.h"
#include "expr/skolem_manager.h"
#include "theory/rewriter.h"

namespace CVC4 {
namespace theory {
namespace arrays {

void ArraysProofRuleChecker::registerTo(ProofChecker* pc)
{
  pc->registerChecker(PfRule::ARRAYS_READ_OVER_WRITE, this);
  pc->registerChecker(PfRule::ARRAYS_READ_OVER_WRITE_1, this);
  pc->registerChecker(PfRule::ARRAYS_EXT, this);
}

Node ArraysProofRuleChecker::checkInternal(PfRule id,
                                           const std::vector<Node>& children,
                                           const std::vector<Node>& args)
{
  if (id == PfRule::ARRAYS_READ_OVER_WRITE)
  {
    Assert(children.size() == 1);
    Assert(args.size() == 1);
    Node ideq = children[0];
    if (ideq.getKind() != kind::NOT || ideq[0].getKind() != kind::EQUAL)
    {
      return Node::null();
    }
    Node lhs = args[0];
    if (lhs.getKind() != kind::SELECT || lhs[0].getKind() != kind::STORE
        || lhs[0][1] != ideq[0][0])
    {
      return Node::null();
    }
    Node rhs =
        NodeManager::currentNM()->mkNode(kind::SELECT, lhs[0][0], ideq[0][1]);
    return lhs.eqNode(rhs);
  }
  if (id == PfRule::ARRAYS_READ_OVER_WRITE_1)
  {
    Assert(children.empty());
    Assert(args.size() == 1);
    Node lhs = args[0];
    if (lhs.getKind() != kind::SELECT || lhs[0].getKind() != kind::STORE
        || lhs[0][1] != lhs[1])
    {
      return Node::null();
    }
    Node rhs = lhs[0][2];
    return lhs.eqNode(rhs);
  }
  if (id == PfRule::ARRAYS_EXT)
  {
  }
  // no rule
  return Node::null();
}

}  // namespace arrays
}  // namespace theory
}  // namespace CVC4
