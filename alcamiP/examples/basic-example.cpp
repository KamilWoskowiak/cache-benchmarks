#include <iostream>
#include <vector>

#include <alcami/cache.h>
#include <alcami/predefined.h>
#include <alcami/utils.h>


auto main() -> int {
  std::vector<int> data{1, 2, 4, 8};

  auto lookup_f = alc::mapping_adapter<std::size_t>([&data](std::size_t i) { return data.at(i); });
  auto cache = alc::make_cache<std::size_t, int>(2, lookup_f, alc::policies::lru);

  const auto k = 1;
  auto h = cache.lookup(k);
  std::cout << *h.value() << "\n";
}
