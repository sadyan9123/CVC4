/*********************                                                        */
/*! \file proof_checker.h
 ** \verbatim
 ** Top contributors (to current version):
 **   Andrew Reynolds
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2019 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved.  See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief Equality proof checker utility
 **/

#include "cvc4_private.h"

#ifndef CVC4__THEORY__BUILTIN__PROOF_CHECKER_H
#define CVC4__THEORY__BUILTIN__PROOF_CHECKER_H

#include "expr/node.h"
#include "expr/proof_checker.h"
#include "expr/proof_node.h"

namespace CVC4 {
namespace theory {

/** Identifiers for rewriters and substitutions.
 *
 * A "rewriter" is abstractly a method from Node to Node, where the output
 * is semantically equivalent to the input. The identifiers below list
 * various methods that have this contract. This identifier is used
 * in a number of the builtin rules.
 *
 * A substitution is a method for turning a formula into
 */
enum class MethodId : uint32_t
{
  //---------------------------- Rewriters
  // Rewriter::rewrite(n)
  RW_REWRITE,
  // Rewriter::rewriteExtEquality(n)
  RW_REWRITE_EQ_EXT,
  // identity
  RW_IDENTITY,
  //---------------------------- Substitutions
  // (= x y) is interpreted as x -> y, using Node::substitute(...)
  SB_DEFAULT,
  // (= x y) is interpreted as (= x y) -> true, using Node::substitute(...)
  SB_PREDICATE,
};
/** Converts a rewriter id to a string. */
const char* toString(MethodId id);
/** Write a rewriter id to out */
std::ostream& operator<<(std::ostream& out, MethodId id);
/** Make a method id node */
Node mkMethodId(MethodId id);

namespace builtin {

/** A checker for builtin proofs */
class BuiltinProofRuleChecker : public ProofRuleChecker
{
 public:
  BuiltinProofRuleChecker() {}
  ~BuiltinProofRuleChecker() {}
  /**
   * Apply rewrite on n (in witness form). This encapsulates the exact behavior
   * of a REWRITE step in a proof. Rewriting is performed on the Skolem form of
   * n.
   *
   * @param n The node (in witness form) to rewrite,
   * @param id The identifier of the rewriter.
   * @return The rewritten form of n.
   */
  static Node applyRewrite(Node n, MethodId idr = MethodId::RW_REWRITE);
  /**
   * Apply substitution on n (in witness form). This encapsulates the exact
   * behavior of a SUBS step in a proof. Substitution is on the Skolem form of
   * n.
   *
   * @param n The node (in witness form) to substitute,
   * @param exp The (set of) equalities (in witness form) corresponding to the
   * substitution
   * @return The substituted form of n.
   */
  static Node applySubstitution(Node n,
                                Node exp,
                                MethodId ids = MethodId::SB_DEFAULT);
  static Node applySubstitution(Node n,
                                const std::vector<Node>& exp,
                                MethodId ids = MethodId::SB_DEFAULT);
  /** Apply substitution + rewriting
   *
   * Combines the above two steps.
   *
   * @param n The node (in witness form) to substitute and rewrite,
   * @param exp The (set of) equalities (in witness form) corresponding to the
   * substitution
   * @param id The identifier of the rewriter.
   * @return The substituted, rewritten form of n.
   */
  static Node applySubstitutionRewrite(Node n,
                                       const std::vector<Node>& exp,
                                       MethodId ids = MethodId::SB_DEFAULT,
                                       MethodId idr = MethodId::RW_REWRITE);
  /** get a rewriter Id from a node, return false if we fail */
  static bool getMethodId(TNode n, MethodId& i);

  /** Register all rules owned by this rule checker into pc. */
  void registerTo(ProofChecker* pc) override;

 protected:
  /** Return the conclusion of the given proof step, or null if it is invalid */
  Node checkInternal(PfRule id,
                     const std::vector<Node>& children,
                     const std::vector<Node>& args) override;
  /** get method ids */
  bool getMethodIds(const std::vector<Node>& args,
                    MethodId& ids,
                    MethodId& idr,
                    size_t index);
  /**
   * Apply rewrite (on Skolem form). id is the identifier of the rewriter.
   */
  static Node applyRewriteExternal(Node n, MethodId idr = MethodId::RW_REWRITE);
  /**
   * Apply substitution for n (on Skolem form), where exp is an equality
   * (or set of equalities) in Witness form. Returns the result of
   * n * { exp[0] -> exp[1] } in Skolem form.
   */
  static Node applySubstitutionExternal(Node n, Node exp, MethodId ids);
  static Node applySubstitutionExternal(Node n,
                                        const std::vector<Node>& exp,
                                        MethodId ids);
};

}  // namespace builtin
}  // namespace theory
}  // namespace CVC4

#endif /* CVC4__THEORY__BUILTIN__PROOF_CHECKER_H */
