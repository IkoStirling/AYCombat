#include <AYCombat/InitiativeClock.h>

#include <iostream>
#include <utility>
#include <vector>

int main() {
    using ayt::combat::InitiativeClock;
    auto created = InitiativeClock::create(100);
    if (!created) return 1;
    auto clock = std::move(*created);
    if (!clock.addActor(1, 100) || !clock.addActor(2, 50)) return 1;
    std::vector<std::uint64_t> order;
    for (int tick = 0; tick < 2; ++tick) {
        if (!clock.advance(1)) return 1;
        while (const auto next = clock.beginNext()) {
            // A game can now run Ability/Combat and decide the action's result.
            order.push_back(*next);
            if (!clock.finish(*next)) return 1;
        }
    }
    if (order != std::vector<std::uint64_t>{1, 1, 2}) return 1;
    std::cout << "fixed-tick initiative: 1, 1, 2; tick " << clock.tick() << '\n';
    return 0;
}
