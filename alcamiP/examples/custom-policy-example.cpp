#include <algorithm>
#include <functional>
#include <iostream>

#include <alcami/cache.h>
#include <alcami/predefined.h>

auto main() -> int {
  // create policy that tries to keep only values of even keys in the cache
  const auto id = 0;
  auto comb_f = [](int p1, int p2) { return std::max(p1, p2); };
  auto prio_f = [](alc::timestamp_t, int key) { return key % 2 == 0 ? 1 : 0; };
  auto my_policy = alc::make_policy<int>(id, prio_f, comb_f);

  auto map_f = alc::mapping_adapter<int>([](int k) { return k; });
  auto cache = alc::make_cache<int, int>(2, map_f, my_policy, std::identity{});

  const auto k = 1;
  auto h = cache.lookup(k);
  std::cout << *h.value() << "\n";
}
