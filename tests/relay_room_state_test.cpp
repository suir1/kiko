#include "relay_room_state.hpp"

#include <iostream>

namespace {

kiko::RelayWaitingPeer peer(kiko::Role role) {
  kiko::RelayWaitingPeer value;
  value.role = role;
  return value;
}

}  // namespace

int main() {
  using namespace kiko;

  {
    RelayRoomState rooms{RelayServerConfig{}};
    auto first = rooms.pair_or_wait("room-a#0", "room-a", peer(Role::Sender), /*auxiliary=*/false);
    if (first.kind != RelayRoomPairing::Kind::Waiting) {
      std::cerr << "FAIL: first primary peer should wait\n";
      return 1;
    }

    auto matched = rooms.pair_or_wait("room-a#0", "room-a", peer(Role::Receiver), /*auxiliary=*/false);
    if (matched.kind != RelayRoomPairing::Kind::Matched || !matched.claimed_active_room || !matched.self ||
        !matched.peer) {
      std::cerr << "FAIL: primary pair should match and claim active room\n";
      return 1;
    }

    auto duplicate = rooms.pair_or_wait("room-a#0", "room-a", peer(Role::Sender), /*auxiliary=*/false);
    if (duplicate.kind != RelayRoomPairing::Kind::RoomFull || !duplicate.self) {
      std::cerr << "FAIL: active primary room should reject duplicate primary peer\n";
      return 1;
    }

    rooms.release_active_room("room-a");
    auto after_release = rooms.pair_or_wait("room-a#0", "room-a", peer(Role::Sender), /*auxiliary=*/false);
    if (after_release.kind != RelayRoomPairing::Kind::Waiting) {
      std::cerr << "FAIL: released room should accept a new primary peer\n";
      return 1;
    }
  }

  {
    RelayRoomState rooms{RelayServerConfig{}};
    (void)rooms.pair_or_wait("room-b#0", "room-b", peer(Role::Sender), /*auxiliary=*/false);
    auto primary = rooms.pair_or_wait("room-b#0", "room-b", peer(Role::Receiver), /*auxiliary=*/false);
    if (primary.kind != RelayRoomPairing::Kind::Matched) {
      std::cerr << "FAIL: primary setup did not match\n";
      return 1;
    }

    auto aux_first = rooms.pair_or_wait("room-b#1", "room-b", peer(Role::Sender), /*auxiliary=*/true);
    if (aux_first.kind != RelayRoomPairing::Kind::Waiting) {
      std::cerr << "FAIL: aux peer should be allowed while primary room is active\n";
      return 1;
    }
    auto aux_second = rooms.pair_or_wait("room-b#1", "room-b", peer(Role::Receiver), /*auxiliary=*/true);
    if (aux_second.kind != RelayRoomPairing::Kind::Matched || aux_second.claimed_active_room) {
      std::cerr << "FAIL: aux pair should match without claiming active room\n";
      return 1;
    }
  }

  {
    RelayRoomState rooms{RelayServerConfig{}};
    (void)rooms.pair_or_wait("room-c#0", "room-c", peer(Role::Sender), /*auxiliary=*/false);
    auto same_role = rooms.pair_or_wait("room-c#0", "room-c", peer(Role::Sender), /*auxiliary=*/false);
    if (same_role.kind != RelayRoomPairing::Kind::RoomFull || !same_role.self) {
      std::cerr << "FAIL: same-role peer should be rejected without replacing waiter\n";
      return 1;
    }
    auto opposite = rooms.pair_or_wait("room-c#0", "room-c", peer(Role::Receiver), /*auxiliary=*/false);
    if (opposite.kind != RelayRoomPairing::Kind::Matched || !opposite.peer ||
        opposite.peer->role != Role::Sender) {
      std::cerr << "FAIL: original waiting peer should remain after same-role rejection\n";
      return 1;
    }
  }

  if (relay_room_base("room-d#2") != "room-d" || relay_room_base("room-e") != "room-e") {
    std::cerr << "FAIL: relay room base parsing\n";
    return 1;
  }

  std::cout << "relay_room_state_test ok\n";
  return 0;
}
