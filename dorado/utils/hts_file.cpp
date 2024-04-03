#include "hts_file.h"

#include "utils/PostCondition.h"

#include <htslib/bgzf.h>
#include <htslib/hts.h>
#include <htslib/sam.h>
#include <spdlog/spdlog.h>

#include <cassert>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace {

constexpr size_t MINIMUM_BUFFER_SIZE = 100000ul;  // The smallest allowed buffer size is 100 KB.

}  // namespace

bool compare_headers(const dorado::SamHdrPtr& header1, const dorado::SamHdrPtr& header2) {
    std::string header_str1(sam_hdr_str(header1.get()));
    std::string header_str2(sam_hdr_str(header2.get()));
    return (header_str1 == header_str2);
}

namespace dorado::utils {

HtsFile::HtsFile(const std::string& filename, OutputMode mode, size_t threads, bool sort_bam)
        : m_filename(filename), m_threads(int(threads)), m_sort_bam(sort_bam), m_mode(mode) {
    switch (mode) {
    case OutputMode::FASTQ:
        m_file.reset(hts_open(m_filename.c_str(), "wf"));
        hts_set_opt(m_file.get(), FASTQ_OPT_AUX, "RG");
        hts_set_opt(m_file.get(), FASTQ_OPT_AUX, "st");
        hts_set_opt(m_file.get(), FASTQ_OPT_AUX, "DS");
        break;
    case OutputMode::BAM: {
        auto file = m_filename;
        if (file != "-" && m_sort_bam) {
            // We're doing sorted BAM output. We need to indicate this for the
            // finalise method.
            m_finalise_is_noop = false;
            return;
        }
        m_file.reset(hts_open(file.c_str(), "wb"));
    } break;
    case OutputMode::SAM:
        m_file.reset(hts_open(filename.c_str(), "w"));
        break;
    case OutputMode::UBAM:
        m_file.reset(hts_open(filename.c_str(), "wb0"));
        break;
    default:
        throw std::runtime_error("Unknown output mode selected: " +
                                 std::to_string(static_cast<int>(mode)));
    }
    if (!m_file) {
        throw std::runtime_error("Could not open file: " + filename);
    }

    if (m_file->format.compression == bgzf) {
        auto res = bgzf_mt(m_file->fp.bgzf, m_threads, 128);
        if (res < 0) {
            throw std::runtime_error("Could not enable multi threading for BAM generation.");
        }
    }

    m_finalise_is_noop = true;
}

HtsFile::~HtsFile() {
    if (!m_finalised) {
        spdlog::error("finalise() not called on a HtsFile.");
        // Can't throw in a dtor, and this is a logic_error rather than being data dependent.
        std::abort();
    }
}

uint64_t HtsFile::calculate_sorting_key(const bam1_t* record) {
    return (static_cast<uint64_t>(record->core.tid) << 32) | record->core.pos;
}

void HtsFile::set_buffer_size(size_t buff_size) {
    if (buff_size < MINIMUM_BUFFER_SIZE) {
        throw std::runtime_error("The buffer size for sorted BAM output must be at least " +
                                 std::to_string(MINIMUM_BUFFER_SIZE) + " (" +
                                 std::to_string(MINIMUM_BUFFER_SIZE / 1000) + " KB).");
    }
    m_buffer_size = buff_size;
    m_bam_buffer.resize(m_buffer_size);
}

void HtsFile::flush_temp_file(const bam1_t* last_record) {
    if (m_current_buffer_offset == 0 && !last_record) {
        // This handles the case that the last read passed in before calling finalise() has already triggered
        // a flush, or that finalise() was called without ever passing any reads.
        return;
    }
    if (last_record) {
        // We add last_record to our buffer map with offset -1, so that we know where it should be sorted into
        // the output.
        auto sorting_key = calculate_sorting_key(last_record);
        m_buffer_map.insert({sorting_key, -1});
    }

    // Open the file for writing, and write the header. Note that all temp files will have the same header.
    auto file_index = m_temp_files.size();
    auto tempfilename = m_filename + "." + std::to_string(file_index) + ".tmp";
    m_temp_files.push_back(tempfilename);
    m_file.reset(hts_open(tempfilename.c_str(), "wb"));
    if (m_file->format.compression == bgzf) {
        auto res = bgzf_mt(m_file->fp.bgzf, m_threads, 128);
        if (res < 0) {
            throw std::runtime_error("Could not enable multi threading for BAM generation.");
        }
    }
    if (sam_hdr_write(m_file.get(), m_header.get()) != 0) {
        throw std::runtime_error("Could not write header to temp file.");
    }

    for (const auto& item : m_buffer_map) {
        // This will give us the offsets into the buffer in sorted order.
        int64_t offset = item.second;
        const bam1_t* record{nullptr};
        if (offset == -1) {
            record = last_record;
        } else {
            if (size_t(offset) + sizeof(bam1_t) > m_bam_buffer.size()) {
                throw std::out_of_range("Index out of bounds in BAM record buffer.");
            }
            record = reinterpret_cast<bam1_t*>(m_bam_buffer.data() + offset);
            if (size_t(offset) + sizeof(bam1_t) + size_t(record->l_data) > m_bam_buffer.size()) {
                throw std::out_of_range("Index out of bounds in BAM record buffer.");
            }
        }
        if (write_to_file(record) < 0) {
            throw std::runtime_error("Error writing to BAM temporary file.");
        }
    }
    m_file.reset();
    m_current_buffer_offset = 0;
    m_buffer_map.clear();
}

// If we are doing sorted BAM output, then when we are done we will have sorted temporary files
// that need to be merged into a single sorted BAM file. If there's only one temporary file, we
// can just rename it. Otherwise we create a new file, merge the temporary files into it, and
// delete the temporary files. In case an error occurs, the temporary files are left on disk, so
// users can recover their data.
void HtsFile::finalise(const ProgressCallback& progress_callback) {
    assert(progress_callback);

    // Rough divisions of how far through we are at the start of each section.
    constexpr size_t percent_start_merging = 5;
    constexpr size_t percent_start_indexing = 50;
    progress_callback(0);
    auto on_return = utils::PostCondition([&] { progress_callback(100); });

    if (std::exchange(m_finalised, true)) {
        spdlog::error("finalise() called twice on a HtsFile. Ignoring second call.");
        return;
    }

    if (m_finalise_is_noop) {
        // No cleanup is required. Just close the open objects and we're done.
        m_header.reset();
        m_file.reset();
        return;
    }

    // If any reads are cached for writing, write out the final temporary file.
    flush_temp_file(nullptr);

    bool file_is_mapped = (sam_hdr_nref(m_header.get()) > 0);
    m_header.reset();

    if (m_temp_files.empty()) {
        // No temporary files have been written. Nothing to do.
        return;
    }

    if (m_temp_files.size() == 1) {
        // We only have 1 temporary file, so just rename it.
        std::filesystem::rename(m_temp_files.back(), m_filename);
        m_temp_files.clear();
    } else {
        // Otherwise merge the temp files.
        progress_callback(percent_start_merging);
        ProgressUpdater update_progress(progress_callback, percent_start_merging, m_num_records,
                                        percent_start_indexing);
        if (!merge_temp_files(update_progress)) {
            spdlog::error("Merging of temporary files failed. Skipping indexing.");
            return;
        }
    }

    // Index the final file.
    if (file_is_mapped) {
        progress_callback(percent_start_indexing);
        if (sam_index_build(m_filename.c_str(), 0) < 0) {
            spdlog::error("Failed to build index for file {}", m_filename);
        }
    }
}

int HtsFile::set_header(const sam_hdr_t* const header) {
    if (header) {
        m_header.reset(sam_hdr_dup(header));
        if (m_file) {
            return sam_hdr_write(m_file.get(), m_header.get());
        }
    }
    return 0;
}

int HtsFile::write(const bam1_t* record) {
    ++m_num_records;
    if (m_file) {
        return write_to_file(record);
    }
    return cache_record(record);
}

int HtsFile::write_to_file(const bam1_t* record) {
    // FIXME -- HtsFile is constructed in a state where attempting to write
    // will segfault, since set_header has to have been called
    // in order to set m_header.
    if (m_mode != OutputMode::FASTQ) {
        assert(m_header);
    }
    return sam_write1(m_file.get(), m_header.get(), record);
}

int HtsFile::cache_record(const bam1_t* record) {
    size_t bytes_required = sizeof(bam1_t) + size_t(record->l_data);
    if (m_current_buffer_offset + bytes_required > m_bam_buffer.size()) {
        // This record won't fit in the buffer, so flush the current buffer, plus this record, to the file.
        flush_temp_file(record);
        return 0;
    }
    auto sorting_key = calculate_sorting_key(record);
    m_buffer_map.insert({sorting_key, int64_t(m_current_buffer_offset)});

    // Copy the contents of the bam1_t struct into the memory buffer.
    bam1_t* buffer_entry = reinterpret_cast<bam1_t*>(m_bam_buffer.data() + m_current_buffer_offset);
    memcpy(buffer_entry, record, sizeof(bam1_t));

    // The data pointed to by the bam1_t::data field is then copied immediately after the struct contents.
    m_current_buffer_offset += sizeof(bam1_t);
    memcpy(m_bam_buffer.data() + m_current_buffer_offset, record->data, record->l_data);

    // We have to tell our buffered object where its copy of the data is.
    buffer_entry->data = m_bam_buffer.data() + m_current_buffer_offset;

    // Round up our buffer offset so that the next entry will be 8-byte aligned.
    m_current_buffer_offset += size_t(record->l_data);
    m_current_buffer_offset = ((m_current_buffer_offset + 7) / 8) * 8;
    return 0;
}

bool HtsFile::merge_temp_files(ProgressUpdater& update_progress) {
    // This code assumes the headers for the files are all the same. This will be
    // true if the temp-files were created by this class, but it means that this
    // function is not suitable for generic merging of BAM files.
    std::vector<HtsFilePtr> in_files(m_temp_files.size());
    std::vector<BamPtr> top_records(m_temp_files.size());
    SamHdrPtr header{};
    for (size_t i = 0; i < m_temp_files.size(); ++i) {
        in_files[i].reset(hts_open(m_temp_files[i].c_str(), "rb"));
        if (bgzf_mt(in_files[i]->fp.bgzf, m_threads, 128) < 0) {
            spdlog::error("Could not enable multi threading for BAM reading.");
            return false;
        }
        SamHdrPtr current_header(sam_hdr_read(in_files[i].get()));
        if (i == 0) {
            header = std::move(current_header);
        } else {
            // Sanity check. Make sure headers match.
            if (!compare_headers(header, current_header)) {
                spdlog::error("Header for temporary file {} does not match other headers.",
                              m_temp_files[i]);
                return false;
            }
            current_header.reset();
        }
        top_records[i].reset(bam_init1());
        auto res = sam_read1(in_files[i].get(), header.get(), top_records[i].get());
        if (res < 0) {
            spdlog::error("Could not read first record from file " + m_temp_files[i]);
            return false;
        }
    }

    // Open the output file, and write the header.
    HtsFilePtr out_file(hts_open(m_filename.c_str(), "wb"));
    if (bgzf_mt(out_file->fp.bgzf, m_threads, 128) < 0) {
        spdlog::error("Could not enable multi threading for BAM generation.");
        return false;
    }

    SamHdrPtr out_header(sam_hdr_dup(header.get()));
    sam_hdr_change_HD(out_header.get(), "SO", "coordinate");
    if (sam_hdr_write(out_file.get(), out_header.get()) < 0) {
        spdlog::error("Failed to write header for sorted bam file {}", out_file->fn);
        return false;
    }

    size_t processed_records = 0;
    size_t files_done = 0;
    while (files_done < m_temp_files.size()) {
        // Find the next file to write a record from.
        uint64_t best_score = std::numeric_limits<uint64_t>::max();
        int best_index = -1;
        for (size_t i = 0; i < m_temp_files.size(); ++i) {
            if (top_records[i]) {
                auto score = calculate_sorting_key(top_records[i].get());
                if (best_index == -1 || score < best_score) {
                    best_score = score;
                    best_index = int(i);
                }
            }
        }
        if (best_index == -1) {
            spdlog::error("Logic error in merging algorithm.");
            return false;
        }

        // Write the record.
        if (sam_write1(out_file.get(), out_header.get(), top_records[best_index].get()) < 0) {
            spdlog::error("Failed to write to sorted file {}", out_file->fn);
            return false;
        }
        ++processed_records;
        update_progress(processed_records);

        // Load the next record for the file.
        top_records[best_index].reset(bam_init1());
        auto res =
                sam_read1(in_files[best_index].get(), header.get(), top_records[best_index].get());
        if (res == -1) {
            // EOF reached. Close the file and mark that this file is done.
            top_records[best_index].reset();
            in_files[best_index].reset();
            ++files_done;
        } else if (res < -1) {
            spdlog::error("Error reading record from file {}", in_files[best_index]->fn);
            return false;
        }
    }

    // Remove the temporary files.
    for (const auto& temp_file : m_temp_files) {
        std::filesystem::remove(temp_file);
    }
    return true;
}

}  // namespace dorado::utils
