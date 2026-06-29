// SPDX-License-Identifier: Apache-2.0

#include "btop.hpp"

#include <iterator>
#include <ranges>
#include <string_view>
#include <vector>

auto main(int argc, const char* argv[]) -> int {
	std::vector<std::string_view> args;
	args.reserve(argc > 0 ? argc - 1 : 0);
	for (const auto* arg : std::views::counted(std::next(argv), argc - 1))
		args.emplace_back(arg);
	return btop_main(args);
}
