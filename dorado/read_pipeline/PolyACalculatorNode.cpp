#include "PolyACalculatorNode.h"

#include "ClientInfo.h"
#include "poly_tail/poly_tail_calculator.h"
#include "utils/math_utils.h"
#include "utils/sequence_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace dorado {

void PolyACalculatorNode::input_thread_fn() {
    at::InferenceMode inference_mode_guard;

    Message message;
    while (get_input_message(message)) {
        // If this message isn't a read, just forward it to the sink.
        if (!std::holds_alternative<SimplexReadPtr>(message)) {
            send_message_to_sink(std::move(message));
            continue;
        }

        // If this message isn't a read, we'll get a bad_variant_access exception.
        auto read = std::get<SimplexReadPtr>(std::move(message));

        auto calculator = read->read_common.client_info->contexts()
                                  .get_ptr<const poly_tail::PolyTailCalculator>(nullptr);

        if (!calculator) {
            send_message_to_sink(std::move(read));
            num_not_called++;
            continue;
        }

        auto signal_info = calculator->determine_signal_anchor_and_strand(*read);

        if (signal_info.signal_anchor >= 0) {
            int num_bases = calculator->calculate_num_bases(*read, signal_info);
            if (signal_info.split_tail) {
                auto split_bases = std::max(
                        0, calculator->calculate_num_bases(*read, {signal_info.is_fwd_strand, 0, 0,
                                                                   signal_info.split_tail}));
                num_bases += split_bases;
            }

            if (num_bases > 0 && num_bases < calculator->max_tail_length()) {
                // Update debug stats.
                total_tail_lengths_called += num_bases;
                ++num_called;
                if (spdlog::get_level() <= spdlog::level::debug) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    tail_length_counts[num_bases]++;
                }
                // Set tail length property in the read.
                read->read_common.rna_poly_tail_length = num_bases;
            } else {
                num_not_called++;
            }
        } else {
            num_not_called++;
        }

        send_message_to_sink(std::move(read));
    }
}

PolyACalculatorNode::PolyACalculatorNode(size_t num_worker_threads, size_t max_reads)
        : MessageSink(max_reads, static_cast<int>(num_worker_threads)) {
    start_input_processing(&PolyACalculatorNode::input_thread_fn, this);
}

void PolyACalculatorNode::terminate_impl() { stop_input_processing(); }

stats::NamedStats PolyACalculatorNode::sample_stats() const {
    stats::NamedStats stats = stats::from_obj(m_work_queue);
    stats["reads_not_estimated"] = static_cast<double>(num_not_called.load());
    stats["reads_estimated"] = static_cast<double>(num_called.load());
    stats["average_tail_length"] = static_cast<double>(
            num_called.load() > 0 ? total_tail_lengths_called.load() / num_called.load() : 0);

    if (spdlog::get_level() <= spdlog::level::debug) {
        for (const auto& [len, count] : tail_length_counts) {
            std::string key = "pt." + std::to_string(len);
            stats[key] = static_cast<float>(count);
        }
    }

    return stats;
}

}  // namespace dorado
