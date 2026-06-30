// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_tail.hpp"
#include "btop_tools.hpp"

namespace fs = std::filesystem;

namespace {
	[[nodiscard]] fs::path workspace() {
		auto path = fs::current_path() / "build" / "comfy_tail_test_workspace";
		fs::remove_all(path);
		fs::create_directories(path);
		return path;
	}

	void write_file(const fs::path& path, const std::string& text, std::ios::openmode mode = std::ios::trunc) {
		std::ofstream out(path, mode);
		out << text;
	}

	[[nodiscard]] std::vector<std::string> texts(const ComfyTail::Snapshot& snapshot) {
		std::vector<std::string> out;
		for (const auto& line : snapshot.lines)
			out.push_back(line.text);
		return out;
	}
}

TEST(comfy_tail, scrub_removes_terminal_control_garbage) {
	EXPECT_EQ(ComfyTail::scrub_line("ok \x1b[31mred\x1b[0m"), "ok red");
	EXPECT_EQ(ComfyTail::scrub_line("abc\bZ"), "abZ");
	EXPECT_EQ(ComfyTail::scrub_line(std::string{"a\0b\tc", 5}), "ab c");
	EXPECT_EQ(ComfyTail::scrub_line(std::string{"bad \xC3(", 6}), "bad \xEF\xBF\xBD(");
}

TEST(comfy_tail, classification_precedence_keeps_fatal_visible) {
	EXPECT_EQ(ComfyTail::classify_line("RuntimeError: FutureWarning: The pynvml package is deprecated"), ComfyTail::Severity::Fatal);
	EXPECT_EQ(ComfyTail::classify_line("][ MP:detach | id=abc"), ComfyTail::Severity::Marker);
	EXPECT_EQ(ComfyTail::classify_line("FETCH ComfyRegistry Data"), ComfyTail::Severity::Noise);

	EXPECT_TRUE(ComfyTail::visible_in_view(ComfyTail::Severity::Fatal, ComfyTail::View::Markers));
	EXPECT_FALSE(ComfyTail::visible_in_view(ComfyTail::Severity::Noise, ComfyTail::View::Clean));
	EXPECT_TRUE(ComfyTail::visible_in_view(ComfyTail::Severity::Noise, ComfyTail::View::Raw));
	EXPECT_TRUE(ComfyTail::visible_in_view(ComfyTail::Severity::Marker, ComfyTail::View::Triage));
	EXPECT_FALSE(ComfyTail::visible_in_view(ComfyTail::Severity::Triage, ComfyTail::View::Errors));
}

TEST(comfy_tail, reader_tails_appends_and_filters_views) {
	const auto path = workspace() / "append.log";
	write_file(path, "normal\nFETCH ComfyRegistry Data\n][ MP:detach | id=abc\nRuntimeError: boom\n");

	ComfyTail::TailReader reader;
	auto clean = reader.read(path, ComfyTail::View::Clean, 10);
	EXPECT_EQ(texts(clean), (std::vector<std::string>{"normal", "][ MP:detach | id=abc", "RuntimeError: boom"}));

	auto markers = reader.read(path, ComfyTail::View::Markers, 10);
	EXPECT_EQ(texts(markers), (std::vector<std::string>{"][ MP:detach | id=abc", "RuntimeError: boom"}));

	write_file(path, "partial", std::ios::app);
	auto partial = reader.read(path, ComfyTail::View::Raw, 10);
	EXPECT_EQ(texts(partial).back(), "RuntimeError: boom");

	write_file(path, " done\n", std::ios::app);
	auto completed = reader.read(path, ComfyTail::View::Raw, 10);
	EXPECT_EQ(texts(completed).back(), "partial done");
}

TEST(comfy_tail, reader_exposes_carriage_return_progress_as_live_line) {
	const auto path = workspace() / "progress.log";
	write_file(path, "Generating tokens:   1%|          | 10/100 [00:01<00:09, 10.0it/s]\r");

	ComfyTail::TailReader reader;
	auto first = reader.read(path, ComfyTail::View::Clean, 3);
	ASSERT_EQ(first.lines.size(), 1);
	EXPECT_TRUE(first.lines[0].live);
	ASSERT_TRUE(first.lines[0].progress.valid);
	EXPECT_EQ(first.lines[0].progress.label, "Generating tokens");
	EXPECT_EQ(first.lines[0].progress.percent, 1);
	EXPECT_EQ(first.lines[0].progress.current, 10);
	EXPECT_EQ(first.lines[0].progress.total, 100);
	EXPECT_EQ(first.lines[0].progress.elapsed, "00:01");
	EXPECT_EQ(first.lines[0].progress.eta, "00:09");
	EXPECT_EQ(first.lines[0].progress.rate, "10.0it/s");

	write_file(path, "Generating tokens:  25%|##        | 25/100 [00:02<00:06, 12.5it/s]\r", std::ios::app);
	auto updated = reader.read(path, ComfyTail::View::Clean, 3);
	ASSERT_EQ(updated.lines.size(), 1);
	EXPECT_TRUE(updated.lines[0].live);
	ASSERT_TRUE(updated.lines[0].progress.valid);
	EXPECT_EQ(updated.lines[0].progress.percent, 25);
	EXPECT_EQ(updated.lines[0].progress.current, 25);
	EXPECT_EQ(updated.lines[0].progress.rate, "12.5it/s");

	auto raw = reader.read(path, ComfyTail::View::Raw, 3);
	ASSERT_EQ(raw.lines.size(), 1);
	EXPECT_TRUE(raw.lines[0].live);
	EXPECT_FALSE(raw.lines[0].progress.valid);
	EXPECT_EQ(raw.lines[0].text, "Generating tokens:  25%|##        | 25/100 [00:02<00:06, 12.5it/s]");

	write_file(path, "\n", std::ios::app);
	auto completed = reader.read(path, ComfyTail::View::Clean, 3);
	ASSERT_EQ(completed.lines.size(), 1);
	EXPECT_FALSE(completed.lines[0].live);
	ASSERT_TRUE(completed.lines[0].progress.valid);
	EXPECT_EQ(completed.lines[0].progress.current, 25);
}

TEST(comfy_tail, reader_resets_on_missing_path_and_truncation) {
	const auto path = workspace() / "rotate.log";
	ComfyTail::TailReader reader;

	auto missing = reader.read(path, ComfyTail::View::Clean, 5);
	EXPECT_TRUE(missing.lines.empty());
	EXPECT_NE(missing.status.find("waiting for"), std::string::npos);

	write_file(path, "old line\n");
	auto first = reader.read(path, ComfyTail::View::Raw, 5);
	EXPECT_EQ(texts(first), (std::vector<std::string>{"old line"}));

	write_file(path, "new\n");
	auto reset = reader.read(path, ComfyTail::View::Raw, 5);
	EXPECT_EQ(texts(reset), (std::vector<std::string>{"new"}));

	auto unconfigured = reader.read({}, ComfyTail::View::Raw, 5);
	EXPECT_TRUE(unconfigured.lines.empty());
	EXPECT_EQ(unconfigured.status, "log path not configured");
}

TEST(comfy_tail, reader_scroll_offset_moves_window_above_tail) {
	const auto path = workspace() / "scroll.log";
	write_file(path, "one\ntwo\nthree\nfour\nfive\n");

	ComfyTail::TailReader reader;
	auto bottom = reader.read(path, ComfyTail::View::Raw, 3);
	EXPECT_EQ(texts(bottom), (std::vector<std::string>{"three", "four", "five"}));
	EXPECT_EQ(bottom.max_scroll, 2);
	EXPECT_EQ(bottom.scroll_offset, 0);

	auto older = reader.read(path, ComfyTail::View::Raw, 3, 2);
	EXPECT_EQ(texts(older), (std::vector<std::string>{"one", "two", "three"}));
	EXPECT_EQ(older.scroll_offset, 2);

	auto clamped = reader.read(path, ComfyTail::View::Raw, 3, 99);
	EXPECT_EQ(texts(clamped), (std::vector<std::string>{"one", "two", "three"}));
	EXPECT_EQ(clamped.scroll_offset, 2);
}

TEST(comfy_tail_config, boxes_views_height_and_presets_validate) {
	Term::width = 200;
	Term::height = 80;

	EXPECT_TRUE(Config::stringValid("shown_boxes", "cpu comfy0 comfy1 proc"));
	EXPECT_FALSE(Config::stringValid("comfy0_view", "garbage"));
	EXPECT_TRUE(Config::stringValid("comfy1_view", "errors"));
	EXPECT_FALSE(Config::intValid("comfy_tail_height", "5"));
	EXPECT_TRUE(Config::intValid("comfy_tail_height", "10"));

	const std::string preset = "comfy0:0:default,comfy1:0:default,cpu:0:default,proc:0:default";
	EXPECT_TRUE(Config::presetsValid(preset));
	EXPECT_TRUE(Config::apply_preset(preset));
}
