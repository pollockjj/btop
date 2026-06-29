/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using std::string;
using std::vector;

namespace ComfyTail {
	enum class Severity {
		Normal,
		Noise,
		Marker,
		Triage,
		Fatal,
	};

	enum class View {
		Raw,
		Clean,
		Triage,
		Markers,
		Errors,
	};

	struct Line {
		string text;
		Severity severity = Severity::Normal;
	};

	struct Snapshot {
		vector<Line> lines;
		string status;
		size_t max_scroll = 0;
		size_t scroll_offset = 0;
	};

	[[nodiscard]] bool valid_view(std::string_view value);
	[[nodiscard]] View parse_view(std::string_view value);
	[[nodiscard]] string scrub_line(std::string_view raw);
	[[nodiscard]] Severity classify_line(std::string_view clean);
	[[nodiscard]] bool visible_in_view(Severity severity, View view);

	class TailReader {
	public:
		[[nodiscard]] Snapshot read(const std::filesystem::path& path, View view, size_t max_lines, size_t scroll_offset = 0);
		void reset();

	private:
		std::filesystem::path path;
		uintmax_t offset = 0;
		bool initialized = false;
		bool traceback_active = false;
		string partial;
		std::deque<Line> history;

		void ingest(std::string_view chunk);
		void append_line(string raw_line);
		[[nodiscard]] Snapshot snapshot(View view, size_t max_lines, string status = "", size_t scroll_offset = 0) const;
	};
}
