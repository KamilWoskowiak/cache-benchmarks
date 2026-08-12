#include <iostream>
#include <cstdlib>

#include <alcami/cache.h>
#include <alcami/predefined.h>

auto main() -> int {
  // prioritize smaller numbers
  auto summ_f = [](int k) { return static_cast<alc::policies::fqsz_summ_t>(std::abs(k)); };
  auto map_f = alc::mapping_adapter<int>([](int k) { return 2 * k; });
  auto cache = alc::make_cache<int, int>(2, map_f, alc::policies::fqsz, summ_f);

  const auto k = 1;
  auto h = cache.lookup(k);
  std::cout << *h.value() << "\n";
}
