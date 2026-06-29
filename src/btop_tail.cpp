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

	void TailReader::reset() {
		path.clear();
		offset = 0;
		initialized = false;
		traceback_active = false;
		partial.clear();
		history.clear();
	}

	Snapshot TailReader::read(const std::filesystem::path& new_path, View view, size_t max_lines) {
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
			traceback_active = false;
			return snapshot(view, max_lines, "waiting for "s + path.string());
		}

		if (initialized and size < offset) {
			initialized = false;
			offset = 0;
			partial.clear();
			traceback_active = false;
			history.clear();
		}

		std::ifstream file(path, std::ios::binary);
		if (not file.good()) {
			initialized = false;
			offset = 0;
			partial.clear();
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

		return snapshot(view, max_lines, history.empty() ? "waiting for log lines" : "");
	}

	void TailReader::ingest(std::string_view chunk) {
		for (char c : chunk) {
			if (c == '\r') {
				partial.clear();
				continue;
			}
			if (c == '\n') {
				append_line(std::move(partial));
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

		history.push_back({std::move(clean), severity});
		while (history.size() > max_history_lines)
			history.pop_front();
	}

	Snapshot TailReader::snapshot(View view, size_t max_lines, string status) const {
		Snapshot result;
		result.status = std::move(status);
		if (max_lines == 0) return result;

		for (auto it = history.rbegin(); it != history.rend() and result.lines.size() < max_lines; ++it) {
			if (visible_in_view(it->severity, view))
				result.lines.push_back(*it);
		}
		std::ranges::reverse(result.lines);
		return result;
	}
}

namespace Comfy {
	namespace {
		std::array<ComfyTail::TailReader, 2> readers;
		std::array<ComfyTail::Snapshot, 2> snapshots;

		[[nodiscard]] string config_prefix(unsigned long panel) {
			return "comfy"s + static_cast<char>('0' + panel);
		}
	}

	auto collect(unsigned long index, bool no_update) -> const ComfyTail::Snapshot& {
		if (index >= shown_panels.size() or index >= snapshots.size()) return snapshots.front();
		const auto panel = static_cast<unsigned long>(shown_panels.at(index));
		if (panel >= readers.size()) return snapshots.at(index);
		if (no_update) return snapshots.at(index);

		const auto prefix = config_prefix(panel);
		const auto view = ComfyTail::parse_view(Config::getS(prefix + "_view"));
		const auto max_lines = index < height_vec.size() ? static_cast<size_t>(std::max(1, height_vec.at(index) - 2)) : 8;
		snapshots.at(index) = readers.at(panel).read(Config::getS(prefix + "_log_path"), view, max_lines);
		return snapshots.at(index);
	}
}
