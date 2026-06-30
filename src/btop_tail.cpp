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

#include "btop_tail.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>

#include "btop_config.hpp"
#include "btop_shared.hpp"

using namespace std::literals;

namespace {
	constexpr size_t initial_read_bytes = 64 * 1024;
	constexpr size_t max_history_lines = 400;
	const string replacement = "\xEF\xBF\xBD";

	[[nodiscard]] string lower_copy(std::string_view value) {
		string out;
		out.reserve(value.size());
		for (unsigned char c : value)
			out.push_back(static_cast<char>(std::tolower(c)));
		return out;
	}

	[[nodiscard]] bool contains_ic(std::string_view haystack, std::string_view needle) {
		return lower_copy(haystack).contains(lower_copy(needle));
	}

	[[nodiscard]] std::string_view ltrim_view(std::string_view value) {
		while (not value.empty() and std::isspace(static_cast<unsigned char>(value.front())))
			value.remove_prefix(1);
		return value;
	}

	[[nodiscard]] std::string_view trim_view(std::string_view value) {
		value = ltrim_view(value);
		while (not value.empty() and std::isspace(static_cast<unsigned char>(value.back())))
			value.remove_suffix(1);
		return value;
	}

	[[nodiscard]] string trim_copy(std::string_view value) {
		return string{trim_view(value)};
	}

	[[nodiscard]] bool parse_u64(std::string_view value, size_t& pos, uint64_t& out) {
		while (pos < value.size() and std::isspace(static_cast<unsigned char>(value[pos])))
			pos++;
		if (pos >= value.size() or not std::isdigit(static_cast<unsigned char>(value[pos])))
			return false;

		uint64_t parsed = 0;
		while (pos < value.size() and std::isdigit(static_cast<unsigned char>(value[pos]))) {
			parsed = parsed * 10 + static_cast<uint64_t>(value[pos] - '0');
			pos++;
		}
		out = parsed;
		return true;
	}

	[[nodiscard]] string strip_trailing_colon(std::string_view value) {
		value = trim_view(value);
		while (not value.empty() and (value.back() == ':' or std::isspace(static_cast<unsigned char>(value.back()))))
			value.remove_suffix(1);
		return trim_copy(value);
	}

	void pop_last_utf8_char(string& value) {
		if (value.empty()) return;
		value.pop_back();
		while (not value.empty() and (static_cast<unsigned char>(value.back()) & 0xC0) == 0x80)
			value.pop_back();
	}

	void append_utf8_repaired(string& out, std::string_view value) {
		for (size_t i = 0; i < value.size();) {
			const auto c = static_cast<unsigned char>(value[i]);
			if (c < 0x80) {
				out.push_back(static_cast<char>(c));
				i++;
				continue;
			}

			size_t len = 0;
			uint32_t codepoint = 0;
			uint32_t min_codepoint = 0;
			if ((c & 0xE0) == 0xC0) {
				len = 2;
				codepoint = c & 0x1F;
				min_codepoint = 0x80;
			}
			else if ((c & 0xF0) == 0xE0) {
				len = 3;
				codepoint = c & 0x0F;
				min_codepoint = 0x800;
			}
			else if ((c & 0xF8) == 0xF0) {
				len = 4;
				codepoint = c & 0x07;
				min_codepoint = 0x10000;
			}
			else {
				out += replacement;
				i++;
				continue;
			}

			if (i + len > value.size()) {
				out += replacement;
				i++;
				continue;
			}

			bool valid = true;
			for (size_t j = 1; j < len; j++) {
				const auto cc = static_cast<unsigned char>(value[i + j]);
				if ((cc & 0xC0) != 0x80) {
					valid = false;
					break;
				}
				codepoint = (codepoint << 6) | (cc & 0x3F);
			}

			if (not valid or codepoint < min_codepoint or (codepoint >= 0xD800 and codepoint <= 0xDFFF) or codepoint > 0x10FFFF) {
				out += replacement;
				i++;
				continue;
			}

			out.append(value.substr(i, len));
			i += len;
		}
	}

	[[nodiscard]] bool looks_like_traceback_continuation(std::string_view line) {
		if (line.empty()) return false;
		const auto trimmed = ltrim_view(line);
		if (trimmed.size() != line.size()) return true;
		return trimmed.starts_with("File ") or (not trimmed.starts_with("[") and not trimmed.starts_with("][") and trimmed.contains(":"));
	}

	[[nodiscard]] bool is_fatal(std::string_view line) {
		return line.contains("Traceback (most recent call last)")
			or line.contains("Segmentation fault")
			or line.contains("CUDA error")
			or line.contains("CUDA out of memory")
			or line.contains("RuntimeError")
			or line.contains("KeyError");
	}

	[[nodiscard]] bool is_triage(std::string_view line) {
		return line.contains("Potential memory leak detected with model NoneType")
			or line.contains("WARNING, memory leak with model NoneType")
			or line.contains("[ZOMBIE]")
			or line.contains("[cleanup_models] Removing dead model")
			or line.contains("[load_models_gpu]")
			or (line.contains("Installed frontend version ") and line.contains(" is lower than the recommended version"));
	}

	[[nodiscard]] bool is_marker(std::string_view line) {
		const auto trimmed = ltrim_view(line);
		return trimmed.starts_with("][ load_models_gpu")
			or trimmed.starts_with("][ free_memory")
			or trimmed.starts_with("][ cleanup_models")
			or trimmed.starts_with("][ stability_test_monitor")
			or trimmed.starts_with("][ MP:")
			or trimmed.starts_with("][ MM:")
			or trimmed.starts_with("][ SHM:")
			or trimmed.starts_with("][ _clean_cache");
	}

	[[nodiscard]] bool is_noise(std::string_view line) {
		return line.contains("FutureWarning: The pynvml package is deprecated")
			or (line.contains("UserWarning:") and contains_ic(line, "torch"))
			or line.contains("FETCH ComfyRegistry Data")
			or line.contains("standard output captured")
			or line.contains("got prompt")
			or line.contains("Error running sage attention");
	}
}

namespace ComfyTail {
	bool valid_view(std::string_view value) {
		return value == "raw" or value == "clean" or value == "triage" or value == "markers" or value == "errors";
	}

	View parse_view(std::string_view value) {
		if (value == "raw") return View::Raw;
		if (value == "triage") return View::Triage;
		if (value == "markers") return View::Markers;
		if (value == "errors") return View::Errors;
		return View::Clean;
	}

	string scrub_line(std::string_view raw) {
		string stripped;
		stripped.reserve(raw.size());

		for (size_t i = 0; i < raw.size();) {
			const auto c = static_cast<unsigned char>(raw[i]);

			if (c == 0x1B) {
				if (i + 1 >= raw.size()) break;
				const auto next = raw[i + 1];
				if (next == '[') {
					i += 2;
					while (i < raw.size() and not (static_cast<unsigned char>(raw[i]) >= 0x40 and static_cast<unsigned char>(raw[i]) <= 0x7E))
						i++;
					if (i < raw.size()) i++;
					continue;
				}
				if (next == ']') {
					i += 2;
					while (i < raw.size()) {
						if (raw[i] == '\a') {
							i++;
							break;
						}
						if (raw[i] == 0x1B and i + 1 < raw.size() and raw[i + 1] == '\\') {
							i += 2;
							break;
						}
						i++;
					}
					continue;
				}
				i += 2;
				continue;
			}

			if (c == '\b' or c == 0x7F) {
				pop_last_utf8_char(stripped);
				i++;
				continue;
			}

			if (c == '\t') {
				stripped.push_back(' ');
				i++;
				continue;
			}

			if (c == '\0' or (c < 0x20 and c != '\r' and c != '\n')) {
				i++;
				continue;
			}

			stripped.push_back(static_cast<char>(c));
			i++;
		}

		string repaired;
		repaired.reserve(stripped.size());
		append_utf8_repaired(repaired, stripped);
		return repaired;
	}

	Severity classify_line(std::string_view clean) {
		if (is_fatal(clean)) return Severity::Fatal;
		if (is_triage(clean)) return Severity::Triage;
		if (is_marker(clean)) return Severity::Marker;
		if (is_noise(clean)) return Severity::Noise;
		return Severity::Normal;
	}

	bool visible_in_view(Severity severity, View view) {
		if (severity == Severity::Fatal) return true;
		switch (view) {
			case View::Raw:
				return true;
			case View::Clean:
				return severity != Severity::Noise;
			case View::Triage:
				return severity == Severity::Triage or severity == Severity::Marker;
			case View::Markers:
				return severity == Severity::Marker;
			case View::Errors:
				return false;
		}
		return severity != Severity::Noise;
	}

	Progress parse_progress_line(std::string_view clean) {
		Progress progress;
		const auto percent_marker = clean.find("%|");
		if (percent_marker == std::string_view::npos)
			return progress;

		size_t percent_begin = percent_marker;
		while (percent_begin > 0 and std::isdigit(static_cast<unsigned char>(clean[percent_begin - 1])))
			percent_begin--;
		if (percent_begin == percent_marker)
			return progress;

		uint64_t percent = 0;
		size_t percent_pos = percent_begin;
		if (not parse_u64(clean, percent_pos, percent) or percent_pos != percent_marker)
			return progress;

		const auto second_bar = clean.find('|', percent_marker + 2);
		if (second_bar == std::string_view::npos)
			return progress;

		size_t value_pos = second_bar + 1;
		uint64_t current = 0;
		uint64_t total = 0;
		if (not parse_u64(clean, value_pos, current))
			return progress;
		if (value_pos >= clean.size() or clean[value_pos] != '/')
			return progress;
		value_pos++;
		if (not parse_u64(clean, value_pos, total) or total == 0)
			return progress;

		progress.valid = true;
		progress.label = strip_trailing_colon(clean.substr(0, percent_begin));
		progress.percent = static_cast<int>(std::clamp<uint64_t>(percent, 0, 100));
		progress.current = current;
		progress.total = total;

		const auto bracket_begin = clean.find('[', value_pos);
		const auto bracket_end = bracket_begin == std::string_view::npos ? std::string_view::npos : clean.find(']', bracket_begin + 1);
		if (bracket_begin != std::string_view::npos and bracket_end != std::string_view::npos) {
			auto inside = clean.substr(bracket_begin + 1, bracket_end - bracket_begin - 1);
			const auto comma = inside.find(',');
			auto timing = comma == std::string_view::npos ? inside : inside.substr(0, comma);
			if (comma != std::string_view::npos)
				progress.rate = trim_copy(inside.substr(comma + 1));

			const auto eta_sep = timing.find('<');
			if (eta_sep != std::string_view::npos) {
				progress.elapsed = trim_copy(timing.substr(0, eta_sep));
				progress.eta = trim_copy(timing.substr(eta_sep + 1));
			}
			else {
				progress.elapsed = trim_copy(timing);
			}
		}

		return progress;
	}

	void TailReader::reset() {
		path.clear();
		offset = 0;
		initialized = false;
		traceback_active = false;
		live_active = false;
		live_line.clear();
		partial.clear();
		history.clear();
	}

	Snapshot TailReader::read(const std::filesystem::path& new_path, View view, size_t max_lines, size_t scroll_offset) {
		if (new_path.empty()) {
			reset();
			return snapshot(view, max_lines, "log path not configured");
		}

		if (path != new_path) {
			reset();
			path = new_path;
		}

		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec) {
			initialized = false;
			offset = 0;
			partial.clear();
			live_active = false;
			live_line.clear();
			traceback_active = false;
			return snapshot(view, max_lines, "waiting for "s + path.string());
		}

		if (initialized and size < offset) {
			initialized = false;
			offset = 0;
			partial.clear();
			live_active = false;
			live_line.clear();
			traceback_active = false;
			history.clear();
		}

		std::ifstream file(path, std::ios::binary);
		if (not file.good()) {
			initialized = false;
			offset = 0;
			partial.clear();
			live_active = false;
			live_line.clear();
			traceback_active = false;
			return snapshot(view, max_lines, "waiting for "s + path.string());
		}

		if (not initialized) {
			offset = size > initial_read_bytes ? size - initial_read_bytes : 0;
			file.seekg(static_cast<std::streamoff>(offset));
			if (offset > 0) {
				string discard;
				std::getline(file, discard);
				const auto pos = file.tellg();
				offset = pos == std::streampos{-1} ? size : static_cast<uintmax_t>(pos);
			}
			initialized = true;
		}
		else {
			file.seekg(static_cast<std::streamoff>(offset));
		}

		string chunk((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (not chunk.empty()) {
			offset += chunk.size();
			ingest(chunk);
		}

		return snapshot(view, max_lines, history.empty() ? "waiting for log lines" : "", scroll_offset);
	}

	void TailReader::ingest(std::string_view chunk) {
		for (char c : chunk) {
			if (c == '\r') {
				live_active = true;
				if (not partial.empty())
					live_line = partial;
				partial.clear();
				continue;
			}
			if (c == '\n') {
				if (not partial.empty()) {
					append_line(std::move(partial));
				}
				else if (live_active and not live_line.empty()) {
					append_line(std::move(live_line));
				}
				live_active = false;
				live_line.clear();
				partial.clear();
				continue;
			}
			partial.push_back(c);
		}
	}

	void TailReader::append_line(string raw_line) {
		auto clean = scrub_line(raw_line);
		auto severity = classify_line(clean);

		if (traceback_active) {
			if (looks_like_traceback_continuation(clean)) {
				severity = Severity::Fatal;
			}
			else {
				traceback_active = false;
			}
		}

		if (clean.contains("Traceback (most recent call last)")) {
			traceback_active = true;
			severity = Severity::Fatal;
		}

		auto progress = parse_progress_line(clean);
		history.push_back({std::move(clean), severity, false, std::move(progress)});
		while (history.size() > max_history_lines)
			history.pop_front();
	}

	bool TailReader::has_live_line() const {
		return live_active and (not partial.empty() or not live_line.empty());
	}

	Line TailReader::build_live_line() const {
		const auto raw = not partial.empty() ? partial : live_line;
		auto clean = scrub_line(raw);
		return {clean, classify_line(clean), true, parse_progress_line(clean)};
	}

	Snapshot TailReader::snapshot(View view, size_t max_lines, string status, size_t scroll_offset) const {
		Snapshot result;
		result.status = std::move(status);
		if (max_lines == 0) return result;

		vector<Line> visible;
		visible.reserve(history.size() + (has_live_line() ? 1 : 0));
		for (const auto& line : history) {
			if (visible_in_view(line.severity, view)) {
				auto out_line = line;
				if (view == View::Raw)
					out_line.progress = {};
				visible.push_back(std::move(out_line));
			}
		}
		if (has_live_line()) {
			auto line = build_live_line();
			if (view == View::Raw)
				line.progress = {};
			if (visible_in_view(line.severity, view))
				visible.push_back(std::move(line));
		}

		result.max_scroll = visible.size() > max_lines ? visible.size() - max_lines : 0;
		result.scroll_offset = std::min(scroll_offset, result.max_scroll);
		const size_t start = visible.size() > max_lines ? visible.size() - max_lines - result.scroll_offset : 0;
		const size_t stop = std::min(visible.size(), start + max_lines);

		for (size_t i = start; i < stop; ++i) {
			result.lines.push_back(visible.at(i));
		}
		return result;
	}
}

namespace Comfy {
	namespace {
		std::array<ComfyTail::TailReader, 2> readers;
		std::array<ComfyTail::Snapshot, 2> snapshots;
		std::array<size_t, 2> scroll_offsets = {};

		[[nodiscard]] string config_prefix(unsigned long panel) {
			return "comfy"s + static_cast<char>('0' + panel);
		}
	}

	bool scroll(unsigned long index, int delta) {
		if (index >= scroll_offsets.size() or index >= snapshots.size()) return false;
		const size_t current = scroll_offsets.at(index);
		const size_t max_scroll = snapshots.at(index).max_scroll;
		size_t next = current;
		if (delta > 0) {
			next = std::min(max_scroll, current + static_cast<size_t>(delta));
		}
		else if (delta < 0) {
			const auto amount = static_cast<size_t>(-delta);
			next = amount >= current ? 0 : current - amount;
		}
		if (next == current) return false;
		scroll_offsets.at(index) = next;
		if (index < redraw.size()) redraw.at(index) = true;
		return true;
	}

	unsigned long panel_at(int col, int line) {
		for (unsigned long i = 0; i < static_cast<unsigned long>(shown); ++i) {
			if (i >= x_vec.size() or i >= y_vec.size() or i >= width_vec.size() or i >= height_vec.size()) continue;
			if (col >= x_vec.at(i) and col < x_vec.at(i) + width_vec.at(i)
				and line >= y_vec.at(i) and line < y_vec.at(i) + height_vec.at(i)) {
				return i;
			}
		}
		return static_cast<unsigned long>(shown);
	}

	auto collect(unsigned long index, bool no_update) -> const ComfyTail::Snapshot& {
		if (index >= shown_panels.size() or index >= snapshots.size()) return snapshots.front();
		const auto panel = static_cast<unsigned long>(shown_panels.at(index));
		if (panel >= readers.size()) return snapshots.at(index);
		if (no_update) return snapshots.at(index);

		const auto prefix = config_prefix(panel);
		const auto view = ComfyTail::parse_view(Config::getS(prefix + "_view"));
		const auto max_lines = index < height_vec.size() ? static_cast<size_t>(std::max(1, height_vec.at(index) - 2)) : 8;
		snapshots.at(index) = readers.at(panel).read(Config::getS(prefix + "_log_path"), view, max_lines, scroll_offsets.at(index));
		scroll_offsets.at(index) = snapshots.at(index).scroll_offset;
		return snapshots.at(index);
	}
}
