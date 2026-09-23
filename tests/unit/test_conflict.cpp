#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/conflict.hpp"
#include "loss_observatory/testkit/testkit.hpp"

using namespace loss_observatory;

namespace {

SourceClaim claim(const char* source, SourceAuthority authority, LossClass klass, std::uint32_t ratio_bp,
                  bool ratio_defined = true) {
  SourceClaim value{};
  value.source = SourceId::from_canonical_text(source);
  value.epoch = EpochId::from_canonical_text("epoch/1");
  value.method = MeasurementMethod::SequenceGap;
  value.granularity = Granularity::Flow;
  value.klass = klass;
  value.freshness = Freshness::Fresh;
  value.authority = authority;
  value.lost = ratio_bp;
  value.offered = 10000;
  value.ratio_defined = ratio_defined;
  value.ratio_bp = ratio_bp;
  value.observation_count = 1;
  value.evidence_ids.push_back(MeasurementId::from_value(1));
  return value;
}

}  // namespace

LO_TEST(conflict, a_single_source_never_conflicts) {
  ConflictDetector detector{LossPolicy{}};
  const std::vector<SourceClaim> claims{claim("a", SourceAuthority::Primary, LossClass::ConfirmedLoss, 500)};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(!resolution.conflicted);
  LO_CHECK(!resolution.resolved);
  LO_CHECK_EQ(resolution.claims.size(), 1ULL);
}

LO_TEST(conflict, agreeing_sources_do_not_conflict) {
  ConflictDetector detector{LossPolicy{}};
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 500),
      claim("b", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 510)};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(!resolution.conflicted);
}

LO_TEST(conflict, unresolved_disagreement_becomes_conflicting_evidence) {
  ConflictDetector detector{LossPolicy{}};
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 5000),
      claim("b", SourceAuthority::Secondary, LossClass::NoLossObserved, 0)};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(resolution.conflicted);
  LO_CHECK(resolution.basis == ConflictBasis::ClassDivergence);
  LO_CHECK(!resolution.resolved);
  LO_CHECK(!resolution.rationale.empty());

  // The shared invariant checker must agree with the detector.
  Classification classification{};
  classification.klass = LossClass::ConflictingEvidence;
  LO_CHECK(testkit::disagreement_is_retained(classification, resolution));
  classification.klass = LossClass::ConfirmedLoss;
  LO_CHECK(!testkit::disagreement_is_retained(classification, resolution));
}

LO_TEST(conflict, higher_authority_resolves_but_the_losing_claim_is_retained) {
  ConflictDetector detector{LossPolicy{}};
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::Primary, LossClass::ConfirmedLoss, 5000),
      claim("b", SourceAuthority::Advisory, LossClass::NoLossObserved, 0),
      claim("c", SourceAuthority::Advisory, LossClass::NoLossObserved, 0)};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(resolution.conflicted);
  LO_CHECK(resolution.resolved);
  LO_CHECK(resolution.authoritative == SourceId::from_canonical_text("a"));
  LO_CHECK_EQ(resolution.retained_lower_authority.size(), 2ULL);
  LO_CHECK_EQ(resolution.claims.size(), 3ULL);
}

LO_TEST(conflict, equal_top_authority_cannot_be_broken) {
  ConflictDetector detector{LossPolicy{}};
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::Primary, LossClass::ConfirmedLoss, 5000),
      claim("b", SourceAuthority::Primary, LossClass::NoLossObserved, 0)};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(resolution.conflicted);
  LO_CHECK(!resolution.resolved);
  LO_CHECK(resolution.authoritative.is_nil());
}

LO_TEST(conflict, no_authority_at_all_cannot_be_broken) {
  ConflictDetector detector{LossPolicy{}};
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::None, LossClass::ConfirmedLoss, 5000),
      claim("b", SourceAuthority::None, LossClass::NoLossObserved, 0)};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(resolution.conflicted);
  LO_CHECK(!resolution.resolved);
}

LO_TEST(conflict, ratio_divergence_beyond_tolerance_is_a_conflict) {
  LossPolicy policy{};
  policy.conflict_tolerance_bp = 10;
  ConflictDetector detector{policy};
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 100),
      claim("b", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 900)};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(resolution.conflicted);
  LO_CHECK(resolution.basis == ConflictBasis::RatioDivergence);
  LO_CHECK_EQ(resolution.ratio_spread_bp, 800U);
}

LO_TEST(conflict, divergence_inside_tolerance_is_not_a_conflict) {
  LossPolicy policy{};
  policy.conflict_tolerance_bp = 100;
  ConflictDetector detector{policy};
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 100),
      claim("b", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 150)};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(!resolution.conflicted);
}

LO_TEST(conflict, two_epochs_of_one_source_are_not_independent_witnesses) {
  ConflictDetector detector{LossPolicy{}};
  SourceClaim first = claim("a", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 5000);
  SourceClaim second = claim("a", SourceAuthority::Secondary, LossClass::NoLossObserved, 0);
  second.epoch = EpochId::from_canonical_text("epoch/2");
  const std::vector<SourceClaim> claims{first, second};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(!resolution.conflicted);
}

LO_TEST(conflict, stale_claims_do_not_participate) {
  ConflictDetector detector{LossPolicy{}};
  SourceClaim stale = claim("b", SourceAuthority::Primary, LossClass::NoLossObserved, 0);
  stale.freshness = Freshness::Stale;
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 5000), stale};
  const ConflictResolution resolution = detector.evaluate(claims);
  LO_CHECK(!resolution.conflicted);
  LO_CHECK_EQ(resolution.claims.size(), 1ULL);
}

LO_TEST(conflict, incompatible_is_symmetric_and_tolerance_aware) {
  const SourceClaim loss = claim("a", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 500);
  const SourceClaim none = claim("b", SourceAuthority::Secondary, LossClass::NoLossObserved, 0);
  LO_CHECK(ConflictDetector::incompatible(loss, none, 50));
  LO_CHECK(ConflictDetector::incompatible(none, loss, 50));

  const SourceClaim near = claim("c", SourceAuthority::Secondary, LossClass::ConfirmedLoss, 520);
  LO_CHECK(!ConflictDetector::incompatible(loss, near, 50));
  LO_CHECK(ConflictDetector::incompatible(loss, near, 5));

  SourceClaim stale = loss;
  stale.freshness = Freshness::Stale;
  LO_CHECK(!ConflictDetector::incompatible(stale, none, 0));
}

LO_TEST(conflict, resolution_text_is_deterministic) {
  ConflictDetector detector{LossPolicy{}};
  const std::vector<SourceClaim> claims{
      claim("a", SourceAuthority::Primary, LossClass::ConfirmedLoss, 5000),
      claim("b", SourceAuthority::Advisory, LossClass::NoLossObserved, 0)};
  const ConflictResolution first = detector.evaluate(claims);
  const ConflictResolution second = detector.evaluate(claims);
  LO_CHECK_EQ(first.to_string(), second.to_string());
}
