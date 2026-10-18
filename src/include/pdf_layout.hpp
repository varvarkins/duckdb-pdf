#pragma once

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace duckdb {

// Character measurements count code points: std::string::size() is bytes, which
// overstates accented, symbol and non-Latin text and shrinks every width ratio.
static int64_t Utf8CodepointCount(const string &text) {
	int64_t count = 0;
	for (unsigned char c : text) {
		if ((c & 0xC0) != 0x80) {
			count++;
		}
	}
	return count;
}

static double Median(std::vector<double> v) {
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	size_t m = v.size() / 2;
	if (v.size() % 2 == 1) {
		return v[m];
	}
	return 0.5 * (v[m - 1] + v[m]);
}

//===--------------------------------------------------------------------===//
// Page layout over positioned words
//
// Reading order is a geometric fact, not a rendering hint. Words arrive from
// page::text_list() (or OCR) with bounding boxes, and everything a reader needs
// — which column a word sits in, which line, and what order the lines go in —
// follows from those boxes. This is the single implementation: read_pdf,
// read_pdf_words, read_pdf_lines, read_pdf_elements and pdf_to_markdown all
// group words through it, so they cannot disagree about what a line is.
//
// Poppler's own text modes stay reachable through `layout` because they answer
// different questions: 'physical' pads with spaces to imitate the printed page,
// 'raw' reports content-stream order. Neither of them knows what a column is.
//===--------------------------------------------------------------------===//
struct LayoutWord {
	double x0 = 0.0;
	double y0 = 0.0;
	double x1 = 0.0;
	double y1 = 0.0;
	double font_size = 0.0; // 0 when unknown (OCR); only used to spot display type
	string text;
};

// A word joins a line when it overlaps that line vertically by at least this
// fraction of the shorter of the two heights.
static constexpr double LAYOUT_LINE_OVERLAP_MIN_RATIO = 0.5;

// A candidate gutter must be this many median character widths wide, measured
// in the units the page itself chose rather than in absolute points.
static constexpr double LAYOUT_GUTTER_MIN_CHARS = 3.0;
// ...and every column a gutter produces must be at least this fraction of the
// page wide. This is what separates a column boundary from the regular gaps
// inside a table: on the DuckDB Friendly SQL calendar the prose/grid gutter is
// 53.8pt and leaves columns 42% and 40% of the page wide, while the gaps
// between weekday columns are 20.6pt and would leave columns 3% wide.
static constexpr double LAYOUT_BAND_MIN_WIDTH_RATIO = 0.12;
// ...and wide enough to hold a line of text, measured in the page's own
// characters. A page fraction alone is not enough, because a small page can
// carry a narrow table whose cell columns are a large fraction of it:
// test/data/table.pdf splits Item | Qty | Price into bands 34% and 47% of the
// page, but only 12 characters wide. Real reading columns are far wider — the
// calendar's are 65 characters, a two-column paper's around 45.
static constexpr double LAYOUT_BAND_MIN_CHARS = 20.0;
// Pages with less text than this are never split.
static constexpr size_t LAYOUT_MIN_WORDS_TO_SPLIT = 12;
static constexpr size_t LAYOUT_MAX_BANDS = 8;
// A fallback edge must recur on three words; the affected paper pages have
// hundreds of aligned body edges, while figure/table edges are shorter runs.
static constexpr size_t LAYOUT_MIN_GUTTER_EDGE_SUPPORT = 3;
// A fallback gutter needs 20 words on each side; the paper has hundreds per
// body column, while the markdown fixture's right-side table has only ten.
static constexpr size_t LAYOUT_MIN_GUTTER_SIDE_WORDS = 20;
// Poppler's repeated column edges vary by less than 1.5pt in the measured
// paper, so this tolerance ignores that jitter without merging adjacent cells.
static constexpr double LAYOUT_GUTTER_EDGE_TOLERANCE = 1.5;
// A fallback gutter may be crossed only by lines that carry little of the page's text:
// at most this fraction of all words may sit on a line the corridor crosses. Figure
// labels and titles crossing a real gutter are a handful of words per line, whereas
// full-width prose above or beside a figure or table is most of the page's words, and
// cutting there would slice each of those lines in two. Each side also needs this many
// lines of its own.
static constexpr double LAYOUT_GUTTER_MAX_CROSSING_WORD_RATIO = 0.2;
static constexpr size_t LAYOUT_MIN_GUTTER_SIDE_LINES = 8;
// LayoutLineIds is defined after the band code that calls it.
// `spanning_lines`, when given, receives one flag per line id (index id - 1):
// whether that line crosses a column gutter.
static std::vector<int32_t> LayoutLineIds(const std::vector<LayoutWord> &words, std::vector<int32_t> &bands,
                                          const std::vector<std::pair<double, double>> &gutters = {},
                                          std::vector<bool> *spanning_lines = nullptr);
// Words set this much larger than the body font are display type — titles and
// banners, which routinely straddle a gutter. They get no vote on where the
// gutters are, but they are still placed in a band afterwards.
static constexpr double LAYOUT_DISPLAY_FONT_RATIO = 1.6;

static double LayoutMedianCharWidth(const std::vector<LayoutWord> &words) {
	std::vector<double> widths;
	widths.reserve(words.size());
	for (const auto &w : words) {
		if (!w.text.empty() && w.x1 > w.x0) {
			widths.push_back((w.x1 - w.x0) / static_cast<double>(Utf8CodepointCount(w.text)));
		}
	}
	return Median(std::move(widths));
}

// Keep the existing page-wide candidates first; the vertical-run fallback below
// is only needed when spanning content makes those candidates unusable.
static std::vector<std::pair<double, double>> LayoutGutters(const std::vector<LayoutWord> &words, double min_width) {
	std::vector<std::pair<double, double>> spans;
	spans.reserve(words.size());
	for (const auto &w : words) {
		spans.emplace_back(w.x0, w.x1);
	}
	std::sort(spans.begin(), spans.end());
	std::vector<std::pair<double, double>> gutters;
	if (spans.empty()) {
		return gutters;
	}
	double run_max = spans.front().second;
	for (size_t i = 1; i < spans.size(); i++) {
		if (spans[i].first - run_max >= min_width) {
			gutters.emplace_back(run_max, spans[i].first);
		}
		run_max = MaxValue<double>(run_max, spans[i].second);
	}
	return gutters;
}

// Find edge pairs whose corridor stays clear across the longest vertical run.
// A spanning word blocks only its own y-range rather than the whole page.
static std::vector<std::pair<double, double>> LayoutGuttersByVerticalRun(const std::vector<LayoutWord> &words,
                                                                         double min_width, double page_left,
                                                                         double page_right, double min_band) {
	struct EdgeCluster {
		double position;
		size_t count;
	};
	const double edge_tol = LAYOUT_GUTTER_EDGE_TOLERANCE;
	auto cluster_edges = [&](bool right_edges) {
		std::vector<double> positions;
		positions.reserve(words.size());
		for (const auto &word : words) {
			positions.push_back(right_edges ? word.x1 : word.x0);
		}
		std::sort(positions.begin(), positions.end());
		std::vector<EdgeCluster> clusters;
		for (double position : positions) {
			if (clusters.empty() || position - clusters.back().position > edge_tol) {
				clusters.push_back({position, 1});
			} else {
				clusters.back().position =
				    (clusters.back().position * static_cast<double>(clusters.back().count) + position) /
				    static_cast<double>(clusters.back().count + 1);
				clusters.back().count++;
			}
		}
		return clusters;
	};
	auto left_edges = cluster_edges(true);
	auto right_edges = cluster_edges(false);
	struct Candidate {
		double first;
		double second;
		size_t side_lines; // lines that sit wholly on one side of the corridor
	};
	std::vector<Candidate> candidates;
	// Lines are grouped without regard to columns, so a line of two-column text is
	// one line with words on both sides of the corridor.
	std::vector<int32_t> no_bands(words.size(), 0);
	const auto line_of = LayoutLineIds(words, no_bands);
	int32_t n_lines = 0;
	for (int32_t l : line_of) {
		n_lines = MaxValue<int32_t>(n_lines, l);
	}
	std::vector<size_t> words_on_line(static_cast<size_t>(n_lines) + 1, 0);
	for (int32_t l : line_of) {
		words_on_line[static_cast<size_t>(l)]++;
	}
	for (const auto &left : left_edges) {
		if (left.count < LAYOUT_MIN_GUTTER_EDGE_SUPPORT) {
			continue;
		}
		for (const auto &right : right_edges) {
			if (right.count < LAYOUT_MIN_GUTTER_EDGE_SUPPORT || right.position - left.position < min_width) {
				continue;
			}
			const double midpoint = 0.5 * (left.position + right.position);
			if (midpoint - page_left < min_band || page_right - midpoint < min_band) {
				continue;
			}
			size_t left_words = 0;
			size_t right_words = 0;
			for (const auto &word : words) {
				left_words += word.x1 <= left.position + edge_tol;
				right_words += word.x0 >= right.position - edge_tol;
			}
			if (left_words < LAYOUT_MIN_GUTTER_SIDE_WORDS || right_words < LAYOUT_MIN_GUTTER_SIDE_WORDS) {
				continue;
			}
			// A gutter has text on both sides of it, at the same heights, on most lines.
			// Blank space alone is not evidence: the empty middle of a figure is clear
			// too, and cutting there would slice every full-width prose line in two.
			std::vector<char> has_left(static_cast<size_t>(n_lines) + 1, 0);
			std::vector<char> has_right(static_cast<size_t>(n_lines) + 1, 0);
			std::vector<char> crosses(static_cast<size_t>(n_lines) + 1, 0);
			for (size_t i = 0; i < words.size(); i++) {
				const auto &word = words[i];
				const size_t l = static_cast<size_t>(line_of[i]);
				if (word.x1 <= left.position + edge_tol) {
					has_left[l] = 1;
				} else if (word.x0 >= right.position - edge_tol) {
					has_right[l] = 1;
				} else {
					crosses[l] = 1;
				}
			}
			size_t crossing_words = 0;
			size_t left_lines = 0;
			size_t right_lines = 0;
			for (size_t l = 1; l <= static_cast<size_t>(n_lines); l++) {
				if (crosses[l]) {
					crossing_words += words_on_line[l];
				} else if (has_left[l] || has_right[l]) {
					left_lines += has_left[l];
					right_lines += has_right[l];
				}
			}
			if (static_cast<double>(crossing_words) >
			        LAYOUT_GUTTER_MAX_CROSSING_WORD_RATIO * static_cast<double>(words.size()) ||
			    left_lines < LAYOUT_MIN_GUTTER_SIDE_LINES || right_lines < LAYOUT_MIN_GUTTER_SIDE_LINES) {
				continue;
			}
			candidates.push_back({left.position, right.position, left_lines + right_lines});
		}
	}
	if (candidates.empty()) {
		return {};
	}
	Candidate best = candidates.front();
	for (const auto &candidate : candidates) {
		if (candidate.side_lines > best.side_lines || (candidate.side_lines == best.side_lines &&
		                                               candidate.second - candidate.first > best.second - best.first)) {
			best = candidate;
		}
	}
	return {{best.first, best.second}};
}

// Split a page into reading columns. Returns a 0-based band id per input word,
// parallel to `words`; an all-zero result means one column.
static std::vector<int32_t> LayoutColumnBands(const std::vector<LayoutWord> &words, double page_width,
                                              std::vector<std::pair<double, double>> *accepted_gutters = nullptr) {
	std::vector<int32_t> band_of(words.size(), 0);
	if (accepted_gutters) {
		accepted_gutters->clear();
	}
	if (words.size() < LAYOUT_MIN_WORDS_TO_SPLIT || page_width <= 0.0) {
		return band_of;
	}
	std::vector<double> sizes;
	for (const auto &w : words) {
		if (w.font_size > 0.0) {
			sizes.push_back(w.font_size);
		}
	}
	const double body = Median(std::move(sizes));
	std::vector<LayoutWord> body_words;
	body_words.reserve(words.size());
	for (const auto &w : words) {
		if (body <= 0.0 || w.font_size <= 0.0 || w.font_size <= LAYOUT_DISPLAY_FONT_RATIO * body) {
			body_words.push_back(w);
		}
	}
	if (body_words.size() < LAYOUT_MIN_WORDS_TO_SPLIT) {
		return band_of;
	}
	const double char_width = LayoutMedianCharWidth(body_words);
	const double min_gutter = MaxValue<double>(LAYOUT_GUTTER_MIN_CHARS * char_width, 1.0);
	double left = body_words.front().x0;
	double right = body_words.front().x1;
	for (const auto &w : body_words) {
		left = MinValue<double>(left, w.x0);
		right = MaxValue<double>(right, w.x1);
	}
	// Corridors of the same width are one phenomenon — a table's cell pitch, or
	// the gutters of an N-column layout — so they are accepted or rejected as a
	// class. Judging them one at a time does not work: three of the DuckDB
	// calendar's seven 20.6pt cell gaps sit far enough apart to pass the
	// band-width test individually, which chops its date grid into four columns.
	const double width_tol = MaxValue<double>(1.5, 0.25 * char_width);
	const double min_band =
	    MaxValue<double>(LAYOUT_BAND_MIN_WIDTH_RATIO * page_width, LAYOUT_BAND_MIN_CHARS * char_width);
	auto choose_gutters = [&](const std::vector<std::pair<double, double>> &candidates) {
		std::vector<double> class_width; // representative width per class
		std::vector<std::vector<std::pair<double, double>>> class_gutters;
		for (const auto &g : candidates) {
			const double w = g.second - g.first;
			size_t at = class_width.size();
			for (size_t i = 0; i < class_width.size(); i++) {
				if (std::fabs(class_width[i] - w) <= width_tol) {
					at = i;
					break;
				}
			}
			if (at == class_width.size()) {
				class_width.push_back(w);
				class_gutters.emplace_back();
			}
			class_gutters[at].push_back(g);
		}
		std::vector<std::pair<double, double>> gutters;
		std::vector<size_t> by_width(class_width.size());
		for (size_t i = 0; i < by_width.size(); i++) {
			by_width[i] = i;
		}
		std::sort(by_width.begin(), by_width.end(),
		          [&](size_t a, size_t b) { return class_width[a] > class_width[b]; });

		// Widest class first, and take a class only while every column it leaves
		// behind is still wide enough to be a column rather than a cell.
		for (size_t ci : by_width) {
			const auto &members = class_gutters[ci];
			if (gutters.size() + members.size() + 1 > LAYOUT_MAX_BANDS) {
				continue;
			}
			auto trial = gutters;
			trial.insert(trial.end(), members.begin(), members.end());
			std::sort(trial.begin(), trial.end(),
			          [](const std::pair<double, double> &a, const std::pair<double, double> &b) {
				          return 0.5 * (a.first + a.second) < 0.5 * (b.first + b.second);
			          });
			bool ok = true;
			double prev = left;
			for (const auto &g : trial) {
				double c = 0.5 * (g.first + g.second);
				if (c - prev < min_band) {
					ok = false;
					break;
				}
				prev = c;
			}
			if (ok && right - prev >= min_band) {
				gutters = std::move(trial);
			}
		}
		return gutters;
	};

	// A page-wide sweep can leave only figure/table corridors. Retry with the
	// longest line-spanning runs before concluding that the page is one column.
	auto gutters = choose_gutters(LayoutGutters(body_words, min_gutter));
	if (gutters.empty()) {
		gutters = choose_gutters(LayoutGuttersByVerticalRun(body_words, min_gutter, left, right, min_band));
	}
	if (gutters.empty()) {
		return band_of;
	}
	if (accepted_gutters) {
		*accepted_gutters = gutters;
	}
	for (size_t i = 0; i < words.size(); i++) {
		const double mid = 0.5 * (words[i].x0 + words[i].x1);
		int32_t band = 0;
		for (const auto &g : gutters) {
			double c = 0.5 * (g.first + g.second);
			if (mid > c) {
				band++;
			}
		}
		band_of[i] = band;
	}
	return band_of;
}

// 1-based line ids in reading order: columns left to right, lines top to bottom
// within a column. Parallel to `words`.
static std::vector<int32_t> LayoutLineIds(const std::vector<LayoutWord> &words, std::vector<int32_t> &bands,
                                          const std::vector<std::pair<double, double>> &gutters,
                                          std::vector<bool> *spanning_lines) {
	std::vector<int32_t> line_of(words.size(), 0);
	if (words.empty()) {
		return line_of;
	}
	struct LineGroup {
		std::vector<size_t> members;
		int32_t band;
		double y0;
		double y1;
		bool spanning;
	};
	std::vector<size_t> order(words.size());
	for (size_t i = 0; i < order.size(); i++) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		if (bands[a] != bands[b]) {
			return bands[a] < bands[b];
		}
		if (words[a].y0 != words[b].y0) {
			return words[a].y0 < words[b].y0;
		}
		return words[a].x0 < words[b].x0;
	});
	std::vector<LineGroup> fragments;
	for (size_t idx : order) {
		const auto &w = words[idx];
		bool joined = !fragments.empty() && bands[idx] == fragments.back().band;
		if (joined) {
			const auto &fragment = fragments.back();
			const double overlap = MinValue<double>(w.y1, fragment.y1) - MaxValue<double>(w.y0, fragment.y0);
			const double shorter = MinValue<double>(w.y1 - w.y0, fragment.y1 - fragment.y0);
			joined = shorter > 0.0 && overlap >= LAYOUT_LINE_OVERLAP_MIN_RATIO * shorter;
		}
		if (joined) {
			auto &fragment = fragments.back();
			fragment.members.push_back(idx);
			fragment.y0 = MinValue<double>(fragment.y0, w.y0);
			fragment.y1 = MaxValue<double>(fragment.y1, w.y1);
		} else {
			fragments.push_back({{idx}, bands[idx], w.y0, w.y1, false});
		}
	}
	for (auto &fragment : fragments) {
		for (size_t idx : fragment.members) {
			for (const auto &gutter : gutters) {
				if (words[idx].x0 < gutter.second - LAYOUT_GUTTER_EDGE_TOLERANCE &&
				    words[idx].x1 > gutter.first + LAYOUT_GUTTER_EDGE_TOLERANCE) {
					fragment.spanning = true;
					break;
				}
			}
			if (fragment.spanning) {
				break;
			}
		}
	}
	std::vector<LineGroup> groups;
	std::vector<bool> consumed(fragments.size(), false);
	auto overlaps = [](const LineGroup &a, const LineGroup &b) {
		const double overlap = MinValue<double>(a.y1, b.y1) - MaxValue<double>(a.y0, b.y0);
		const double shorter = MinValue<double>(a.y1 - a.y0, b.y1 - b.y0);
		return shorter > 0.0 && overlap >= LAYOUT_LINE_OVERLAP_MIN_RATIO * shorter;
	};
	const double max_span_gap = MaxValue<double>(6.0, 1.5 * LayoutMedianCharWidth(words));
	auto connects = [&](const LineGroup &a, const LineGroup &b) {
		if (a.band == b.band) {
			return false;
		}
		const auto &left_group = a.band < b.band ? a : b;
		const auto &right_group = a.band < b.band ? b : a;
		double left_end = left_group.members.empty() ? 0.0 : words[left_group.members.front()].x1;
		for (size_t idx : left_group.members) {
			left_end = MaxValue<double>(left_end, words[idx].x1);
		}
		double right_start = right_group.members.empty() ? 0.0 : words[right_group.members.front()].x0;
		for (size_t idx : right_group.members) {
			right_start = MinValue<double>(right_start, words[idx].x0);
		}
		return right_start - left_end <= max_span_gap;
	};
	for (size_t i = 0; i < fragments.size(); i++) {
		if (consumed[i]) {
			continue;
		}
		bool merge = fragments[i].spanning;
		if (!merge) {
			for (size_t j = 0; j < fragments.size(); j++) {
				if (fragments[j].spanning && overlaps(fragments[i], fragments[j])) {
					merge = true;
					break;
				}
			}
		}
		LineGroup group = fragments[i];
		consumed[i] = true;
		if (merge) {
			bool changed = true;
			while (changed) {
				changed = false;
				for (size_t j = 0; j < fragments.size(); j++) {
					if (consumed[j] || !overlaps(group, fragments[j]) || !connects(group, fragments[j])) {
						continue;
					}
					group.members.insert(group.members.end(), fragments[j].members.begin(), fragments[j].members.end());
					group.y0 = MinValue<double>(group.y0, fragments[j].y0);
					group.y1 = MaxValue<double>(group.y1, fragments[j].y1);
					consumed[j] = true;
					changed = true;
				}
			}
			group.spanning = false;
			for (size_t member : group.members) {
				if (bands[member] != group.band) {
					group.spanning = true;
					break;
				}
			}
		}
		groups.push_back(std::move(group));
	}
	std::vector<size_t> spanning_groups;
	for (size_t i = 0; i < groups.size(); i++) {
		if (groups[i].spanning) {
			spanning_groups.push_back(i);
		}
	}
	std::sort(spanning_groups.begin(), spanning_groups.end(),
	          [&](size_t a, size_t b) { return groups[a].y0 < groups[b].y0; });
	auto spanning_rank = [&](size_t group_index) {
		for (size_t rank = 0; rank < spanning_groups.size(); rank++) {
			if (spanning_groups[rank] == group_index) {
				return rank;
			}
		}
		return spanning_groups.size();
	};
	std::vector<size_t> group_order(groups.size());
	for (size_t i = 0; i < group_order.size(); i++) {
		group_order[i] = i;
	}
	std::sort(group_order.begin(), group_order.end(), [&](size_t a, size_t b) {
		auto section = [&](size_t group_index) {
			if (groups[group_index].spanning) {
				return spanning_rank(group_index);
			}
			size_t result = 0;
			for (size_t span : spanning_groups) {
				result += groups[span].y0 <= groups[group_index].y0;
			}
			return result;
		};
		const size_t section_a = section(a);
		const size_t section_b = section(b);
		if (section_a != section_b) {
			return section_a < section_b;
		}
		if (groups[a].spanning != groups[b].spanning) {
			return !groups[a].spanning;
		}
		if (!groups[a].spanning && groups[a].band != groups[b].band) {
			return groups[a].band < groups[b].band;
		}
		return groups[a].y0 < groups[b].y0;
	});
	for (const auto &group : groups) {
		if (!group.spanning || group.members.empty()) {
			continue;
		}
		int32_t band = bands[group.members.front()];
		for (size_t idx : group.members) {
			band = MinValue<int32_t>(band, bands[idx]);
		}
		for (size_t idx : group.members) {
			bands[idx] = band;
		}
	}
	int32_t line_no = 0;
	for (size_t group_index : group_order) {
		line_no++;
		for (size_t idx : groups[group_index].members) {
			line_of[idx] = line_no;
		}
		if (spanning_lines) {
			spanning_lines->push_back(groups[group_index].spanning);
		}
	}
	return line_of;
}

// Byte length of an end-of-line hyphen that the linked poppler's 'reading' text
// removes when the line continues. Before 26.05 that is '-' alone; from 26.05 it
// is also the soft hyphen and the characters its default
// EndOfLineHyphenMode::RemoveAll treats as hyphens (TextOutputDev isHyphenChar).
static size_t TrailingLineHyphenBytes(const string &line) {
#if POPPLER_VERSION_MAJOR > 26 || (POPPLER_VERSION_MAJOR == 26 && POPPLER_VERSION_MINOR >= 5)
	static const char *const HYPHENS[] = {"-", "\xC2\xAD", "\xE2\x80\x90", "\xEF\xB9\xA3", "\xEF\xBC\x8D"};
#else
	static const char *const HYPHENS[] = {"-"};
#endif
	for (const char *hyphen : HYPHENS) {
		if (StringUtil::EndsWith(line, hyphen)) {
			return strlen(hyphen);
		}
	}
	return 0;
}

// The page as text, one line per geometric line, columns in reading order.
// A line ending in a hyphen is joined to the next line of the same column with
// the hyphen dropped, as poppler does within a text flow; the last line of a
// column or of the page keeps its hyphen, as there. read_pdf_words is untouched.
static string LayoutPageText(const std::vector<LayoutWord> &words, double page_width) {
	if (words.empty()) {
		return string();
	}
	std::vector<std::pair<double, double>> gutters;
	auto bands = LayoutColumnBands(words, page_width, &gutters);
	std::vector<bool> spanning;
	auto lines = LayoutLineIds(words, bands, gutters, &spanning);
	int32_t max_line = 0;
	for (int32_t l : lines) {
		max_line = MaxValue<int32_t>(max_line, l);
	}
	if (max_line <= 0) {
		return string();
	}
	std::vector<std::vector<size_t>> by_line(static_cast<size_t>(max_line));
	for (size_t i = 0; i < words.size(); i++) {
		if (lines[i] > 0) {
			by_line[static_cast<size_t>(lines[i] - 1)].push_back(i);
		}
	}
	string out;
	bool continues = false;
	for (size_t l = 0; l < by_line.size(); l++) {
		auto &idxs = by_line[l];
		std::sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b) { return words[a].x0 < words[b].x0; });
		if (!out.empty() && !continues) {
			out += "\n";
		}
		string line;
		for (size_t k = 0; k < idxs.size(); k++) {
			if (k > 0) {
				line += " ";
			}
			line += words[idxs[k]].text;
		}
		// Every line id has at least one word, so its band is that of any member;
		// spanning lines share one band and form their own flow.
		const bool same_flow = l + 1 < by_line.size() && !idxs.empty() && !by_line[l + 1].empty() &&
		                       spanning[l] == spanning[l + 1] && bands[idxs.front()] == bands[by_line[l + 1].front()];
		const size_t hyphen = same_flow ? TrailingLineHyphenBytes(line) : 0;
		continues = hyphen > 0;
		line.resize(line.size() - hyphen);
		out += line;
	}
	return out;
}

} // namespace duckdb
