#include "CorrectionNode.h"

#include "ClientInfo.h"
#include "alignment/Minimap2Aligner.h"
#include "alignment/Minimap2Index.h"
#include "alignment/Minimap2IndexSupportTypes.h"
#include "alignment/Minimap2Options.h"
#include "utils/bam_utils.h"
#include "utils/sequence_utils.h"

#include <htslib/faidx.h>
#include <htslib/sam.h>
#include <minimap.h>
#include <spdlog/spdlog.h>

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

namespace {}  // namespace

namespace dorado {

const int TOP_K = 30;

struct OverlapWindow {
    size_t overlap_idx = -1;
    int tstart = -1;
    int qstart = -1;
    int qend = -1;
    int cigar_start_idx = -1;
    int cigar_start_offset = -1;
    int cigar_end_idx = -1;
    int cigar_end_offset = -1;
    float accuracy = 0;
};

bool filter_overlap(const OverlapWindow& overlap, const CorrectionAlignments& alignments) {
    bool long_indel = false;
    const auto& cigar = alignments.cigars[overlap.overlap_idx];
    for (size_t i = overlap.cigar_start_idx;
         i < std::min(size_t(overlap.cigar_end_idx + 1), cigar.size()); i++) {
        if (cigar[i].op == CigarOpType::INS || cigar[i].op == CigarOpType::DEL) {
            long_indel |= cigar[i].len >= 30;
        }
    }
    return long_indel;
}

void calculate_accuracy(OverlapWindow& overlap,
                        const CorrectionAlignments& alignments,
                        size_t win_idx,
                        int win_len,
                        int m_window_size) {
    int tstart = overlap.tstart;
    int tend = win_idx * m_window_size + win_len;

    // get query region
    const auto overlap_idx = overlap.overlap_idx;
    int oqstart = alignments.overlaps[overlap_idx].qstart;
    int oqend = alignments.overlaps[overlap_idx].qend;
    int qstart, qend;
    if (alignments.overlaps[overlap_idx].fwd) {
        qstart = oqstart + overlap.qstart;
        qend = oqstart + overlap.qend;
    } else {
        qstart = oqend - overlap.qend;
        qend = oqend - overlap.qstart;
    }

    int qlen = qend - qstart;

    // Fetch subsequences
    std::string tseq = alignments.read_seq.substr(tstart, tend - tstart);
    std::string qseq;
    if (alignments.overlaps[overlap_idx].fwd) {
        qseq = alignments.seqs[overlap_idx].substr(qstart, qlen);
    } else {
        qseq = utils::reverse_complement(alignments.seqs[overlap_idx].substr(qstart, qlen));
    }

    spdlog::info("tstart {} tend {} qstart {} qend {} cig st {} cig end {}", tstart, tend, qstart,
                 qend, overlap.cigar_start_idx, overlap.cigar_end_idx);

    const auto& cigar = alignments.cigars[overlap.overlap_idx];

    // Calculate accuracy
    int tpos = 0, qpos = 0;
    int m = 0, s = 0, i = 0, d = 0;

    for (int idx = overlap.cigar_start_idx; idx <= overlap.cigar_end_idx; idx++) {
        int len = -1;
        if (overlap.cigar_start_idx == overlap.cigar_end_idx) {
            len = overlap.cigar_end_offset - overlap.cigar_start_offset;
        } else if (idx == overlap.cigar_start_idx) {
            len = (cigar[idx].len - overlap.cigar_start_offset);
        } else if (idx == overlap.cigar_end_idx) {
            len = overlap.cigar_end_offset;
        } else {
            len = cigar[idx].len;
        }

        if (len == 0) {
            break;
        }

        //spdlog::info("len {} tpos {} qpos {}", len, tpos, qpos);

        switch (cigar[idx].op) {
        case CigarOpType::MATCH:
            for (int j = 0; j < len; j++) {
                auto tbase = tseq[tpos + j];
                auto qbase = qseq[qpos + j];
                //spdlog::info("{} tbase {}, {} qbase {}", tpos + j, tbase, qpos + j, qbase);

                if (tbase == qbase) {
                    m += 1;
                } else {
                    s += 1;
                }
            }

            tpos += len;
            qpos += len;
            break;
        case CigarOpType::INS:
            i += len;
            qpos += len;
            break;
        case CigarOpType::DEL:
            d += len;
            tpos += len;
            break;
        default:
            break;
        }
    }

    //spdlog::info("m {} s {} i {} d {}", m, s, i, d);

    overlap.accuracy = (static_cast<float>(m) / (m + s + i + d));
    spdlog::info("accuracy {}", overlap.accuracy);
}

std::vector<int> get_max_ins_for_window(const std::vector<OverlapWindow>& windows,
                                        const CorrectionAlignments& alignments,
                                        int tstart,
                                        int win_len) {
    std::vector<int> max_ins(win_len, 0);
    ;
    for (const auto& overlap : windows) {
        auto tpos = overlap.tstart - tstart;

        const auto& cigar = alignments.cigars[overlap.overlap_idx];
        int cigar_len = overlap.cigar_end_idx - overlap.cigar_start_idx + 1;

        for (int i = overlap.cigar_start_idx;
             i <= std::min(overlap.cigar_end_idx, int(cigar.size()) - 1); i++) {
            CigarOpType op = cigar[i].op;
            int len = cigar[i].len;

            int l = -1;
            if (op == CigarOpType::MATCH || op == CigarOpType::DEL) {
                l = len;
            } else if (op == CigarOpType::INS) {
                max_ins[tpos - 1] = std::max(len, max_ins[tpos - 1]);
                continue;
            }

            if (cigar_len == 1) {
                tpos += overlap.cigar_end_offset - overlap.cigar_start_offset;
            } else if (i == overlap.cigar_start_idx) {
                tpos += l - overlap.cigar_start_offset;
            } else if (i == overlap.cigar_end_idx) {
                tpos += overlap.cigar_end_offset;
            } else {
                tpos += l;
            }
        }
    }

    int sum = 0;
    for (auto i : max_ins) {
        sum += i;
    }
    //spdlog::info("max ins sum {}", sum);
    return max_ins;
}

void extract_features(std::vector<std::vector<OverlapWindow>>& windows,
                      const CorrectionAlignments& alignments,
                      int m_window_size) {
    const std::string& tseq = alignments.read_seq;
    int tlen = tseq.length();

    for (size_t w = 0; w < windows.size(); w++) {
        int win_len = (w == windows.size() - 1) ? tlen - m_window_size * w : m_window_size;
        //spdlog::info("win idx {}: win len {}", w, win_len);
        auto& overlap_windows = windows[w];

        // Filter overlaps with very large indels
        std::vector<OverlapWindow> filtered_overlaps;
        for (auto& ovlp : overlap_windows) {
            if (!filter_overlap(ovlp, alignments)) {
                filtered_overlaps.push_back(std::move(ovlp));
            }
        }
        spdlog::info("window {} pre filter windows {} post filter windows {}", w,
                     overlap_windows.size(), filtered_overlaps.size());
        windows[w] = std::move(filtered_overlaps);

        // Sort overlaps by score
        for (auto& ovlp : windows[w]) {
            calculate_accuracy(ovlp, alignments, w, win_len, m_window_size);
        }
        // Sort the filtered overlaps by accuracy score
        std::sort(windows[w].begin(), windows[w].end(),
                  [](const OverlapWindow& a, const OverlapWindow& b) {
                      return a.accuracy > b.accuracy;
                  });
        windows[w].resize(std::min(TOP_K, (int)windows[w].size()));
        spdlog::info("window {} 1st {} 2nd {}", w, windows[w][0].qend, windows[w][1].qend);

        // Find the maximum insert size
        get_max_ins_for_window(windows[w], alignments, w * m_window_size, win_len);
    }
}

void extract_windows(std::vector<std::vector<OverlapWindow>>& windows,
                     const CorrectionAlignments& alignments,
                     int m_window_size) {
    size_t num_alignments = alignments.overlaps.size();
    for (size_t a = 0; a < num_alignments; a++) {
        const auto& overlap = alignments.overlaps[a];
        const auto& cigar = alignments.cigars[a];
        //if (alignments.qnames[a] != "e3066d3e-2bdf-4803-89b9-0f077ac7ff7f") {
        //    continue;
        //}
        spdlog::info("window for {}", alignments.qnames[a]);
        //const std::string& qseq = alignments.seqs[a];

        // Following the is_target == False logic form the rust code.
        if (overlap.tend - overlap.tstart < m_window_size) {
            continue;
        }

        spdlog::info("qlen {} qstart {} qend {} strand {} tlen {} tstart {} tend {}", overlap.qlen,
                     overlap.qstart, overlap.qend, overlap.fwd, overlap.tlen, overlap.tstart,
                     overlap.tend);

        int first_window = -1;
        int last_window = -1;
        int tstart = -1;
        int tpos = -1;
        int qpos = 0;

        int zeroth_window_thresh = (0.1f * m_window_size);
        int nth_window_thresh = overlap.tlen - zeroth_window_thresh;

        spdlog::info("zeroth {} nth {}", zeroth_window_thresh, nth_window_thresh);

        first_window = (overlap.tstart < zeroth_window_thresh
                                ? 0
                                : (overlap.tstart + m_window_size - 1) / m_window_size);
        last_window = (overlap.tend > nth_window_thresh ? (overlap.tend - 1) / m_window_size + 1
                                                        : overlap.tend / m_window_size);
        tstart = overlap.tstart;
        tpos = overlap.tstart;

        spdlog::info("first window {} last window {} tstart {} tpos {}", first_window, last_window,
                     tstart, tpos);

        if (last_window - first_window < 1) {
            continue;
        }

        int t_window_start = -1;
        int q_window_start = -1;
        int cigar_start_idx = -1;
        int cigar_start_offset = -1;

        spdlog::info("tpos {} qpos {}", tpos, qpos);

        if ((tpos % m_window_size == 0) || (tstart < zeroth_window_thresh)) {
            t_window_start = tpos;
            q_window_start = qpos;
            cigar_start_idx = 0;
            cigar_start_offset = 0;
        }

        spdlog::info("t_window_start {} q_window_start {} cigar_start_idx {} cigar_start_offset {}",
                     t_window_start, q_window_start, cigar_start_idx, cigar_start_offset);

        for (size_t cigar_idx = 0; cigar_idx < cigar.size(); cigar_idx++) {
            auto op = cigar[cigar_idx];
            int tnew = tpos;
            int qnew = qpos;
            switch (op.op) {
            case CigarOpType::MATCH:
            case CigarOpType::MISMATCH:
                tnew = tpos + op.len;
                qnew = qpos + op.len;
                //spdlog::info("{} {}", op.len, "M");
                break;
            case CigarOpType::DEL:
                tnew = tpos + op.len;
                //spdlog::info("{} {}", op.len, "D");
                break;
            case CigarOpType::INS:
                qpos += op.len;
                //spdlog::info("{} {}", op.len, "I");
                continue;
            default:
                continue;
            }

            //spdlog::info("tpos {} qpos {} tnew {} qnew {}", tpos, qpos, tnew, qnew);

            int current_w = tpos / m_window_size;
            int new_w = tnew / m_window_size;
            int diff_w = new_w - current_w;

            if (diff_w == 0) {
                tpos = tnew;
                qpos = qnew;
                continue;
            }

            for (int i = 1; i < diff_w; i++) {
                int offset = (current_w + i) * m_window_size - tpos;

                int q_start_new = (op.op == CigarOpType::MATCH || op.op == CigarOpType::MISMATCH)
                                          ? qpos + offset
                                          : qpos;

                if (cigar_start_idx >= 0) {
                    windows[(current_w + i) - 1].push_back(
                            {a, t_window_start, q_window_start, q_start_new, cigar_start_idx,
                             cigar_start_offset, (int)cigar_idx, offset});

                    //spdlog::info("pushed t_window_start {} q_window_start {} q_start_new {} cigar_start_idx {} cigar_start_offseet {} cigar_idx {} offset {}", t_window_start, q_window_start, q_start_new, cigar_start_idx, cigar_start_offset, cigar_idx, offset);

                    t_window_start = tpos + offset;

                    if (op.op == CigarOpType::MATCH || op.op == CigarOpType::MISMATCH) {
                        q_window_start = qpos + offset;
                    } else {
                        q_window_start = qpos;
                    }
                } else {
                    t_window_start = tpos + offset;

                    if (op.op == CigarOpType::MATCH || op.op == CigarOpType::MISMATCH) {
                        q_window_start = qpos + offset;
                    } else {
                        q_window_start = qpos;
                    }
                }
            }

            //spdlog::info("new_w {} window size {} tpos {}", new_w, m_window_size, tpos);
            int offset = new_w * m_window_size - tpos;

            int qend = (op.op == CigarOpType::MATCH || op.op == CigarOpType::MISMATCH)
                               ? qpos + offset
                               : qpos;

            //spdlog::info("offset {} qend {}", offset, qend);

            int cigar_end_idx = -1;
            int cigar_end_offset = -1;

            if (tnew == new_w * m_window_size) {
                if (cigar_idx + 1 < cigar.size() && cigar[cigar_idx + 1].op == CigarOpType::INS) {
                    qend += cigar[cigar_idx + 1].len;
                    cigar_end_idx = cigar_idx + 2;
                } else {
                    cigar_end_idx = cigar_idx + 1;
                }

                cigar_end_offset = 0;
            } else {
                cigar_end_idx = cigar_idx;
                cigar_end_offset = offset;
            }

            //spdlog::info("offset {} qend {}", offset, qend);

            if (cigar_start_idx >= 0) {
                windows[new_w - 1].push_back({a, t_window_start, q_window_start, qend,
                                              cigar_start_idx, cigar_start_offset, cigar_end_idx,
                                              cigar_end_offset});
                //spdlog::info("pushed t_window_start {} q_window_start {} qend {} cigar_start_idx {} cigar_start_offseet {} cigar_end_idx {} cigar_end_offset {}", t_window_start, q_window_start, qend, cigar_start_idx, cigar_start_offset, cigar_end_idx, cigar_end_offset);

                t_window_start = tpos + offset;
                q_window_start = qend;
                cigar_start_idx = cigar_end_idx;
                cigar_start_offset = cigar_end_offset;
            } else {
                t_window_start = tpos + offset;
                q_window_start = qend;
                cigar_start_idx = cigar_end_idx;
                cigar_start_offset = cigar_end_offset;
            }

            tpos = tnew;
            qpos = qnew;
        }

        if (tpos > nth_window_thresh && (tpos % m_window_size != 0)) {
            windows[last_window - 1].push_back({a, t_window_start, q_window_start, qpos,
                                                cigar_start_idx, cigar_start_offset,
                                                (int)cigar.size(), 0});
            //spdlog::info("pushed t_window_start {} q_window_start {} qpos {} cigar_start_idx {} cigar_start_offseet {} cigar len {} 0", t_window_start, q_window_start, qpos, cigar_start_idx, cigar_start_offset, cigar.size());
        }
    }
}

CorrectionNode::CorrectionNode(int threads) : MessageSink(10000, threads) {
    start_input_processing(&CorrectionNode::input_thread_fn, this);
}

void CorrectionNode::input_thread_fn() {
    Message message;
    mm_tbuf_t* tbuf = mm_tbuf_init();
    while (get_input_message(message)) {
        if (std::holds_alternative<CorrectionAlignments>(message)) {
            auto alignments = std::get<CorrectionAlignments>(std::move(message));
            if (alignments.read_name == "d6a6b9c7-a8ed-4271-a003-bd299cf84c85") {
                spdlog::info("Process windows for {} of length", alignments.read_name,
                             alignments.read_seq.length());
                size_t n_windows =
                        (alignments.read_seq.length() + m_window_size - 1) / m_window_size;
                spdlog::info("num windows {}", n_windows);
                std::vector<std::vector<OverlapWindow>> windows;
                windows.resize(n_windows);
                extract_windows(windows, alignments, m_window_size);
                int i = 0;
                for (auto& ovlp_windows : windows) {
                    spdlog::info("{} ovlps in window {}", ovlp_windows.size(), i++);
                }
                extract_features(windows, alignments, m_window_size);
            }
        } else {
            send_message_to_sink(std::move(message));
            continue;
        }
    }
    mm_tbuf_destroy(tbuf);
}

stats::NamedStats CorrectionNode::sample_stats() const { return stats::from_obj(m_work_queue); }

}  // namespace dorado
