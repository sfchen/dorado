#include "PolyACalculator.h"

#include "3rdparty/edlib/edlib/include/edlib.h"
#include "utils/sequence_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace {

int kMaxTailLength = 750;

// This algorithm walks through the signal in windows. For each window
// the avg and stdev of the signal is computed. If the stdev is below
// an empirically determined threshold, and consecutive windows have
// similar avg and stdev, then those windows are considered to be part
// of the polyA tail.
std::pair<int, int> determine_signal_bounds5(int signal_anchor,
                                             const c10::Half* signal,
                                             const std::vector<uint64_t>& seq_to_sig_map,
                                             bool fwd,
                                             const dorado::ReadPtr& read,
                                             int num_samples_per_base,
                                             bool is_rna) {
    int signal_len = seq_to_sig_map[seq_to_sig_map.size() - 1];

    // Maximum gap between intervals that can be combined.
    const int kMaxSampleGap = num_samples_per_base * 3;

    auto check_for_peak = [&](float ref_x, float thres, int start, int end) -> bool {
        float max = signal[start];
        for (int i = start + 1; i < end; i++) {
            max = std::max(max, float(signal[i]));
        }
        return (std::abs(ref_x - max) > thres);
    };

    auto calc_stats = [&](int s, int e) -> std::pair<float, float> {
        float avg = 0;
        for (int i = s; i < e; i++) {
            avg += signal[i];
        }
        avg = avg / (e - s);
        float var = 0;
        for (int i = s; i < e; i++) {
            var += (signal[i] - avg) * (signal[i] - avg);
        }
        var = var / (e - s);
        return {avg, std::sqrt(var)};
    };

    std::vector<std::pair<int, int>> intervals;
    std::vector<std::pair<float, float>> interval_stats;
    std::pair<int, int> last_interval{0, 0};

    // Maximum variance between consecutive values to be
    // considered part of the same interval.
    const float kVar = 0.35f;

    // Determine the outer boundary of the signal space to
    // consider based on the anchor. Go further in the direction
    // the polyA/T tail is running, and less in the other, assuming
    // that the anchor provides a reasonable starting position.
    const int kSpread = num_samples_per_base * kMaxTailLength;
    int left_end = is_rna ? std::max(0, signal_anchor - 50)
                          : (fwd ? std::max(0, signal_anchor - kSpread)
                                 : std::max(0, signal_anchor - kSpread));
    int right_end = (fwd ? std::min(signal_len, signal_anchor + kSpread)
                         : std::min(signal_len, signal_anchor + kSpread));

    const int kStride = 3;
    spdlog::debug("Bounds left {}, right {}", left_end, right_end);
    for (int s = left_end; s < right_end; s += kStride) {
        int e = std::min(s + kMaxSampleGap, right_end);
        auto [avg, stdev] = calc_stats(s, e);
        if (stdev < kVar) {
            if (intervals.size() > 1 && intervals.back().second >= s &&
                std::abs(avg - interval_stats.back().first) < 0.2) {
                auto& last = intervals.back();
                last.second = e;
            } else {
                intervals.push_back({s, e});
            }
            interval_stats.push_back({avg, stdev});
        }
    }

    std::string int_str = "";
    for (auto in : intervals) {
        int_str += std::to_string(in.first) + "-" + std::to_string(in.second) + ", ";
    }
    spdlog::debug("found intervals {}", int_str);

    std::vector<std::pair<int, int>> filtered_intervals;
    // In forward strand, the poly A/T signal should end within 50bp of the
    // signal anchor, and in reverse strand it should start within 50bp of the
    // anchor.
    int kAnchorProximity = 25 * num_samples_per_base;
    if (fwd) {
        std::copy_if(intervals.begin(), intervals.end(), std::back_inserter(filtered_intervals),
                     [&](auto& i) {
                         return std::abs(signal_anchor - i.second) < kAnchorProximity ||
                                (i.first <= signal_anchor) && (signal_anchor <= i.second);
                     });
    } else {
        std::copy_if(intervals.begin(), intervals.end(), std::back_inserter(filtered_intervals),
                     [&](auto& i) {
                         return std::abs(signal_anchor - i.first) < kAnchorProximity ||
                                (i.first <= signal_anchor) && (signal_anchor <= i.second);
                     });
    }

    int_str = "";
    for (auto in : filtered_intervals) {
        int_str += std::to_string(in.first) + "-" + std::to_string(in.second) + ", ";
    }
    spdlog::debug("filtered intervals {}", int_str);

    if (filtered_intervals.empty()) {
        spdlog::debug("Anchor {} No range within anchor proximity found", signal_anchor);
        return {0, 0};
    }

    // Sort the remaining intervals by how close they are to the anchor.
    if (fwd) {
        std::sort(filtered_intervals.begin(), filtered_intervals.end(), [&](auto& l, auto& r) {
            return std::abs(l.second - signal_anchor) < std::abs(r.second - signal_anchor);
        });
    } else {
        std::sort(filtered_intervals.begin(), filtered_intervals.end(), [&](auto& l, auto& r) {
            return std::abs(l.first - signal_anchor) < std::abs(r.first - signal_anchor);
        });
    }

    // Choose the longest interval. This is a stable max, so if there's a tie the one closest
    // to the anchor is chosen.
    auto best_interval = std::max_element(
            filtered_intervals.begin(), filtered_intervals.end(),
            [](auto& l, auto& r) { return (l.second - l.first) < (r.second - r.first); });

    spdlog::debug("Anchor {} Range {} {}", signal_anchor, best_interval->first,
                  best_interval->second);
    return *best_interval;
}

// An alternate approach to detecting the polyA tail. The approach here is to
// walk the signal starting from the signal anchor and moving in the direction
// of the polyA tail by computing a running mean/stdev of a sliding window
// of N bases and comparing the next signal value against that mean/stdev. If
// the value is within a threshold, the next position is tested and so on. This
// is how the interval for the polyA signal is extended.
std::pair<int, int> determine_signal_bounds3(int signal_end,
                                             const c10::Half* signal,
                                             const std::vector<uint64_t>& seq_to_sig_map,
                                             bool fwd,
                                             const std::shared_ptr<dorado::Read>& read) {
    const int kNum = 50;
    std::array<float, kNum> inputs;
    auto stats = [&inputs]() -> std::pair<float, float> {
        float avg = 0;
        for (auto x : inputs) {
            avg += x;
        }
        avg = avg / inputs.size();
        float var = 0;
        for (auto x : inputs) {
            var += (x - avg) * (x - avg);
        }
        var = var / inputs.size();
        return {avg, std::sqrt(var)};
    };
    auto smoother = [&inputs](int n, float x) -> float {
        const float factor = 0.5;
        float val = 0;
        for (int i = 0; i < inputs.size(); i++) {
            val += inputs[i];
        }
        val /= inputs.size();
        return factor * val + (1 - factor) * x;
    };
    int signal_start = 0;
    int n = 0;
    for (int i = signal_end; (fwd ? i > 0 : i < read->raw_data.size(0)); (fwd ? i-- : i++)) {
        float raw_x = signal[i];
        float x = smoother(n, raw_x);
        auto [avg, stdev] = stats();
        spdlog::debug("idx {} x {}, avg {}, stdev {}", i, x, avg, stdev);
        if (n > kNum && std::abs(x - avg) > 2 * stdev) {
            spdlog::debug("Reached end at {} at mean {} stdev {}", i, avg, stdev);
            break;
        }
        inputs[n % inputs.size()] = raw_x;
        signal_start = i;
        n++;
    }
    if (!fwd) {
        std::swap(signal_start, signal_end);
    }
    return {signal_start, signal_end};
}

// Basic estimation of avg translocation speed by dividing number of samples by the
// number of bases called.
int estimate_samples_per_base(const dorado::ReadPtr& read) {
    float num_samples_per_base = static_cast<float>(read->raw_data.size(0)) / read->seq.length();
    return static_cast<int>(num_samples_per_base);
}

// In order to find the approximate location of the start/end (anchor) of the polyA
// cDNA tail, the adapter ends are aligned to the reads to find the breakpoint
// between the read and the adapter. Adapter alignment also helps determine
// the strand direction. This function returns the straand direction,
// the approximate anchor for the tail, and if there needs to be an adjustment
// made to the final polyA tail count based on the adapter sequence (e.g. because
// the adapter itself contains several As).
std::tuple<bool, int, int> determine_base_anchor_and_strand_cdna(
        const dorado::ReadPtr& read) {
    const std::string SSP = "TTTCTGTTGGTGCTGATATTGCTTT";
    const std::string SSP_rc = dorado::utils::reverse_complement(SSP);
    const std::string VNP = "ACTTGCCTGTCGCTCTATCTTCAGAGGAGAGTCCGCCGCCCGCAAGTTTT";
    const std::string VNP_rc = dorado::utils::reverse_complement(VNP);
    int trailing_Ts = dorado::utils::count_trailing_chars(VNP, 'T');

    int window_size = 150;
    std::string read_top = read->seq.substr(0, window_size);
    auto bottom_start = std::max(0, (int)read->seq.length() - window_size);
    std::string read_bottom = read->seq.substr(bottom_start, window_size);

    EdlibAlignConfig align_config = edlibDefaultAlignConfig();
    align_config.task = EDLIB_TASK_LOC;
    align_config.mode = EDLIB_MODE_HW;

    // Check for forward strand.
    EdlibAlignResult top_v1 =
            edlibAlign(SSP.data(), SSP.length(), read_top.data(), read_top.length(), align_config);
    EdlibAlignResult bottom_v1 = edlibAlign(VNP_rc.data(), VNP_rc.length(), read_bottom.data(),
                                            read_bottom.length(), align_config);

    int dist_v1 = top_v1.editDistance + bottom_v1.editDistance;

    // Check for reverse strand.
    EdlibAlignResult top_v2 =
            edlibAlign(VNP.data(), VNP.length(), read_top.data(), read_top.length(), align_config);
    EdlibAlignResult bottom_v2 = edlibAlign(SSP_rc.data(), SSP_rc.length(), read_bottom.data(),
                                            read_bottom.length(), align_config);

    int dist_v2 = top_v2.editDistance + bottom_v2.editDistance;
    spdlog::debug("v1 dist {}, v2 dist {}", dist_v1, dist_v2);

    bool proceed = std::min(dist_v1, dist_v2) < 30;

    if (proceed) {
        bool fwd = true;
        int start = 0, end = 0;
        int base_anchor = 0;
        if (dist_v2 < dist_v1) {
            fwd = false;
            base_anchor = top_v2.endLocations[0];
        } else {
            base_anchor = bottom_start + bottom_v1.startLocations[0];
        }
        return {fwd, base_anchor, trailing_Ts};
    } else {
        spdlog::warn("{} primer edit distance too high {}", read->read_id,
                     std::min(dist_v1, dist_v2));
        return {false, -1, trailing_Ts};
    }
}

// The approach used for determining the approximate location of the polyA
// tail in dRNA is different. Since dRNA is single stranded, we already know the
// direction of ther ead. However, in dRNA, the adapter is DNA. But the model for
// basecalling is trained on RNA data. So the basecall quality of the adapter is poor,
// and alignment doesn't work well. Instead, the raw is traversed to find a point
// where there's a jump in the mean signal value, which is indicative of the
// transition from the DNA adapter to the RNA signal. The polyA will start right
// at the juncture.
std::tuple<bool, int, int> determine_base_anchor_and_strand_rna(
        const dorado::ReadPtr& read) {
    const c10::Half* signal = static_cast<c10::Half*>(read->raw_data.data_ptr());
    int signal_len = read->raw_data.size(0);
    const int kWindow = 50;

    // The algorithm is to keep track of the mean signal value over
    // consecutive windows and find the point when there's a sharp
    // increase in mean signal values. 5 previous mean values are
    // considered with a window size of 50. This gives a rolling
    // window view of ~250 bases.
    std::array<float, 5> means;
    auto check_var = [&means](int latest) -> float {
        float max = std::numeric_limits<float>::min();
        float min = std::numeric_limits<float>::max();

        for (auto v : means) {
            min = std::min(v, min);
            max = std::max(v, max);
        }
        return (means[latest] - min);
    };

    int bp = -1;
    int n = 0;
    // Since the polyA will start after the adapter, and in RNA each
    // base is at least 30 samples long (e.g. in RNA002), we can
    // limit the search space to start from 30 bases from the beginning
    // and up till about half the signal lengths. Note this is only to find
    // the __start__ of the polyA signal.
    for (int i = 3000; i < signal_len / 2; i += kWindow) {
        float mean = 0;
        int s = i, e = i + kWindow;
        for (int j = s; j < e; j++) {
            mean += signal[j];
        }
        mean /= kWindow;
        means[n] = mean;
        auto var = check_var(n);
        if (i >= means.size() && var > 2.2f) {
            bp = i;
            break;
        }
        n = (n + 1) % means.size();
    }
    spdlog::debug("Approx break point {}", bp);

    if (bp > 0) {
        return {false, bp, 0};
    } else {
        return {false, -1, 0};
    }
}

}  // namespace

namespace dorado {

void PolyACalculator::worker_thread() {
    torch::InferenceMode inference_mode_guard;

    Message message;
    while (get_input_message(message)) {
        // If this message isn't a read, just forward it to the sink.
        if (!std::holds_alternative<ReadPtr>(message)) {
            send_message_to_sink(std::move(message));
            continue;
        }

        // If this message isn't a read, we'll get a bad_variant_access exception.
        auto read = std::get<ReadPtr>(std::move(message));

        // Determine the strand direction, approximate base space anchor for the tail, and whether
        // the final length needs to be adjusted depending on the adapter sequence.
        auto [fwd, base_anchor, trailing_Ts] =
                m_is_rna ? determine_base_anchor_and_strand_rna(read)
                         : determine_base_anchor_and_strand_cdna(read);

        if (base_anchor >= 0) {
            const auto stride = read->model_stride;
            const auto seq_to_sig_map = utils::moves_to_map(
                    read->moves, stride, read->raw_data.size(0), read->seq.size() + 1);

            if (m_is_rna) {
                for (int i = 0; i < seq_to_sig_map.size(); i++) {
                    if (base_anchor < seq_to_sig_map[i]) {
                        base_anchor = i - 1;
                        break;
                    }
                }
            }
            int signal_anchor = seq_to_sig_map[base_anchor];
            spdlog::debug("Strand {}; poly A/T signal anchor {}", fwd ? '+' : '-', signal_anchor);

            const c10::Half* signal = static_cast<c10::Half*>(read->raw_data.data_ptr());

            auto num_samples_per_base = estimate_samples_per_base(read);

            // Walk through signal
            auto [signal_start, signal_end] =
                    determine_signal_bounds5(signal_anchor, signal, seq_to_sig_map, fwd, read,
                                             num_samples_per_base, m_is_rna);

            int num_bases = std::round(static_cast<float>(signal_end - signal_start) /
                                       num_samples_per_base) -
                            trailing_Ts;
            if (num_bases >= 0 && num_bases < kMaxTailLength) {
                spdlog::debug(
                        "{} PolyA bases {}, signal anchor {} region {} Signal range is {} {}, "
                        "primer {}, "
                        "samples/base {} shift/scale/trim {} {} {}",
                        read->read_id, num_bases, signal_anchor,
                        read->seq.substr(fwd ? std::max(0, (base_anchor - num_bases)) : base_anchor,
                                         num_bases),
                        signal_start, signal_end, fwd ? base_anchor : base_anchor,
                        num_samples_per_base, read->mshift, read->mscale,
                        read->num_trimmed_samples);
                polyA += num_bases;
                read->rna_poly_tail_length = num_bases;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    poly_a_counts[num_bases]++;
                }
                if (num_bases == 49) {
                    //spdlog::error("{}", read->read_id);
                }
            } else {
                spdlog::warn(
                        "{} PolyA bases {}, signal anchor {} Signal range is {} {} primer {}, "
                        "samples/base {}, shift/scale/trim  {} {} {}",
                        read->read_id, num_bases, signal_anchor, signal_start, signal_end,
                        fwd ? base_anchor : base_anchor, num_samples_per_base, read->mshift,
                        read->mscale, read->num_trimmed_samples);
                not_called++;
            }
        } else {
            not_called++;
        }

        num_reads += 1;
        send_message_to_sink(std::move(read));
    }
}

PolyACalculator::PolyACalculator(size_t num_worker_threads, bool is_rna, size_t max_reads)
        : MessageSink(max_reads), m_num_worker_threads(num_worker_threads), m_is_rna(is_rna) {
    start_threads();
}

void PolyACalculator::start_threads() {
    for (size_t i = 0; i < m_num_worker_threads; i++) {
        m_workers.push_back(
                std::make_unique<std::thread>(std::thread(&PolyACalculator::worker_thread, this)));
    }
}

void PolyACalculator::terminate_impl() {
    terminate_input_queue();
    for (auto& m : m_workers) {
        if (m->joinable()) {
            m->join();
        }
    }
    m_workers.clear();
    spdlog::info("Total {}, not called {}, Avg polyA length {}", num_reads.load(),
                 not_called.load(), polyA.load() / num_reads.load());
    static bool done = false;
    if (!done && spdlog::get_level() != spdlog::level::debug) {
        int max_val = -1;
        for (auto [k, v] : poly_a_counts) {
            max_val = std::max(v, max_val);
        }
        int factor = std::max(1, 1 + max_val / 100);
        for (auto [k, v] : poly_a_counts) {
            spdlog::info("{} : {}", k, std::string(v / factor, '*'));
        }
        done = true;
    }
}

void PolyACalculator::restart() {
    restart_input_queue();
    start_threads();
}

}  // namespace dorado
