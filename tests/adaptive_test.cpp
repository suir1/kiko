#include "adaptive.hpp"
#include "socket.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace kiko;

namespace {

int failures = 0;
void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

}  // namespace

int main() {
  std::vector<std::string> locals{"192.168.1.10", "fd00::1234"};

  // No reflexive observation -> unknown.
  check(classify_nat(locals, Endpoint{"", 0}).type == NatType::Unknown, "empty reflexive is unknown");

  // Reflexive IP equals a local address -> openly reachable.
  check(classify_nat(locals, Endpoint{"192.168.1.10", 4000}).type == NatType::Open, "reflexive==local is open");

  // Reflexive IP differs from all local addresses -> behind NAT.
  check(classify_nat(locals, Endpoint{"203.0.113.7", 4000}).type == NatType::BehindNat, "reflexive!=local is behind-nat");

  check(nat_type_name(NatType::Open) == "open", "open name");
  check(nat_type_name(NatType::BehindNat) == "behind-nat", "behind-nat name");
  check(nat_type_name(NatType::Unknown) == "unknown", "unknown name");

  // Plan tuning: open path should allow a longer direct window than double-NAT.
  AdaptivePuncher puncher;
  std::vector<DirectCandidate> cands{make_direct_candidate(Endpoint{"203.0.113.7", 5000}, "public", 20),
                                     make_direct_candidate(Endpoint{"192.168.1.10", 5000}, "lan", 90)};
  auto open_plan = puncher.plan(Role::Sender, cands, NatProfile{NatType::Open}, NatProfile{NatType::BehindNat});
  auto natted_plan = puncher.plan(Role::Sender, cands, NatProfile{NatType::BehindNat}, NatProfile{NatType::BehindNat});
  check(open_plan.total_timeout > natted_plan.total_timeout, "open path gets a longer direct window");
  check(open_plan.candidates.front().kind == "lan", "higher priority direct candidate sorts first");

  // local_interface_addresses must not crash and must return well-formed entries.
  auto addrs = local_interface_addresses();
  for (const auto& a : addrs) check(!a.empty(), "interface address non-empty");
  std::cout << "discovered " << addrs.size() << " local interface address(es)\n";

  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "PASS: NAT classification + adaptive plan tuning\n";
  return 0;
}
