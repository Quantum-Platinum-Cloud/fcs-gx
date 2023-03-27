/*
-----------------------------------------------------------------------------
                             PUBLIC DOMAIN NOTICE
                 National Center for Biotechnology Information

  This software is a "United States Government Work" under the terms of the
  United States Copyright Act.  It was written as part of the author's official
  duties as a United States Government employees and thus cannot be copyrighted.
  This software is freely available to the public for use. The National Library
  of Medicine and the U.S. Government have not placed any restriction on its use
  or reproduction.

  Although all reasonable efforts have been taken to ensure the accuracy and
  reliability of this software, the NLM and the U.S. Government do not and
  cannot warrant the performance or results that may be obtained by using this
  software. The NLM and the U.S. Government disclaim all warranties, expressed
  or implied, including warranties of performance, merchantability or fitness
  for any particular purpose.

  Please cite NCBI in any work or product based on this material.

-----------------------------------------------------------------------------
*/
#define RANGELESS_ENABLE_TSV 1
#include "types.hpp"
#include "util.hpp"
#include "segment.hpp"
#include "serial_util.hpp"

#include <map>
#include <cctype>
#include <sstream>

using namespace gx;

namespace fn = rangeless::fn;

using fn::operators::operator%; // see fn.hpp
using fn::operators::operator%=;


static const std::array<bool, 256> s_is_valid_iupacna = []
{
    auto ret = std::array<bool, 256>{};

    static const char* bases = "ACMGRSVTWYHKDBN"; // excluding U
    const size_t n = strlen(bases);

    for (const auto i : irange{ n }) {
        ret[bases[i]] = true;
        ret[std::tolower(bases[i])] = true;
    }

    return ret;
}();

/////////////////////////////////////////////////////////////////////////////
// Strip gi|\d+| prefix if followed by prefixed accver; strip trailing pipe.
// If prefixed accession and in scope (e.g. gb| or ref|), but not (lcl| or gi| or gnl|),
// Strip suffix, and the suffix, if any e.g. ref|NM_12345.6|suffix -> NM_12345.6
static seq_id_str_t extract_seq_id(std::string defline)
{
    VERIFY(!defline.empty() && defline.front() == '>');
    defline.erase(defline.begin()); // keep everything up to first space
    {
        const auto pos = defline.find_first_of(" \t\n\v\f\r");  // GP-34042
        if (pos != std::string::npos) {
            defline.resize(pos);
        }
    }
    VERIFY(!defline.empty());
    std::string& seq_id = defline;

    // Convert to bare accession-version, where applicable, because getfasta produces
    // deflines with long seq-ids, whereas blastdbcmd and other NCBI tools output
    // seqid-lists as just bare accvers.

    static const bool normalize_seq_ids = get_env("NORMALIZE_SEQ_IDS", 1);
    if (!normalize_seq_ids) {
        return seq_id_str_t{ std::move(seq_id) };
    }

    // strip gi|\d+|, if followed by prefixed seq-id;
    // if not followed by prefixed seq-id, strip trailing pipe.
    if (str::startswith(seq_id, "gi|")) {
        size_t i = 3; // skip "gi|" ad following digits
        while (i < seq_id.size() && '0' <= seq_id[i] && seq_id[i] <= '9') {
            ++i;
        }

        // can only strip gi if the gi-digits are followed by a prefixed accession.
        if (i > 3
           && i + 6 < seq_id.size()
           && seq_id[i] == '|'    // expecting a pipe after digits
           && (seq_id[i+3] == '|' || seq_id[i+4] == '|')) // expecting prefixed accession
        {
            seq_id = seq_id.substr(i+1); // strip digits + pipe
            VERIFY(seq_id.size() >= 6);

        } else if (seq_id.back() == '|') {
            seq_id.pop_back(); // drop trailing pipe
        }
    }

    // NB: some prefixes are not in scope, e.g. lcl|, gi|, gnl|
    static const auto prefixes = std::string{ "|gb|emb|pir|sp|tr|ref|dbj|prf|tpg|tpe|tpd|gpp|" };

    // Strip prefix (if in scope for stripping), and everything starting at next pipe.
    // e.g.  ref|NZ_CBTO000000000.1|NZ_CBTO010000000 -> NZ_CBTO000000000.1
    // ref|NC_123456.7| -> NC_123456.7
    if (seq_id.size() > 4)
        for (size_t prefix_len : {4, 3})
            if (str::contains(prefixes, "|" + seq_id.substr(0, prefix_len)))
    {
        seq_id = seq_id.substr(prefix_len);
        if (auto pos = seq_id.find('|'); pos != std::string::npos) {
            seq_id.resize(pos);
        }
        break;
    }

    VERIFY(!seq_id.empty());
    return seq_id_str_t{ std::move(seq_id) };
}

static const bool test_extract_seq_id = []
{
    if (!get_env("NORMALIZE_SEQ_IDS", 1)) {
        return true;
    }

    VERIFY(extract_seq_id(">gi|12345 rest")                 == "gi|12345");
    VERIFY(extract_seq_id(">gi|12345| rest")                == "gi|12345");
    VERIFY(extract_seq_id(">gi|12345|NC_12345.6 rest")      == "gi|12345|NC_12345.6");
    VERIFY(extract_seq_id(">gi|12345|ref|NC_12345.6 rest")  == "NC_12345.6");
    VERIFY(extract_seq_id(">gi|12345|ref|NC_12345.6| rest") == "NC_12345.6");
    VERIFY(extract_seq_id(">ref|NC_12345.6 rest")           == "NC_12345.6");
    VERIFY(extract_seq_id(">ref|NC_12345.6| rest")          == "NC_12345.6");
    VERIFY(extract_seq_id(">ref|NC_12345.6|extra rest")     == "NC_12345.6");
    VERIFY(extract_seq_id(">ref|NC_12345.6|extra| rest")    == "NC_12345.6");

    // cases where prefix and suffix are not in scope for stripping
    VERIFY(extract_seq_id(">lcl|NC_12345.6 rest")           == "lcl|NC_12345.6");
    VERIFY(extract_seq_id(">gnl|foo|NC_12345.6|bar rest")   == "gnl|foo|NC_12345.6|bar");
    VERIFY(extract_seq_id(">foo|bar rest")                  == "foo|bar");

    return true;
}();

/////////////////////////////////////////////////////////////////////////////
// Apped a fasta-line to next_inp, or set first defline.
// Return false iff reached next defline.
static bool consume_fasta_line(const std::string& line, validate_iupacna_t validate_iupacna, fasta_seq_t& next_inp)
{
    if (line.empty()) {
        ;
    } else if (line.front() != '>') { // not a defline

        VERIFY(!next_inp.seq_id.empty()); // expecting seq-id to have been read

        if (validate_iupacna == validate_iupacna_t::yes)
            for (const char na : line)
                if (!s_is_valid_iupacna[na])
        {
            GX_THROW(std::string("Invalid base '") + na
                     + "' in seq-id " +next_inp.seq_id
                     + "; line: " + line);
        }

        next_inp.seq += line;

        // uppercase all fasta
        for (const auto i : irange(next_inp.seq.size() - line.size(), next_inp.seq.size())) {
            auto& na = next_inp.seq[i];
            na = char(std::toupper(char(na)));
        }

    } else if (next_inp.seq_id.empty()) { // first defline

        VERIFY(next_inp.seq.empty() && next_inp.offset == 0);
        next_inp.seq_id = extract_seq_id(line);
        next_inp.defline = line;

    } else {
        return false;
    }
    return true;
}

static fasta_seq_t extract_chunk(fasta_seq_t& next_inp, size_t chunk_size, size_t chunk_stride)
{
    VERIFY(next_inp.seq.size() >= chunk_size);
    auto ret_seq = iupacna_seq_t{ next_inp.seq.substr(0, chunk_size) };
    next_inp.seq.erase(0, chunk_stride);
    next_inp.offset += chunk_stride;

    return fasta_seq_t{
        next_inp.seq_id,
        seq_oid_t{},
        next_inp.defline,
        next_inp.offset - chunk_stride, // original offset
        std::move(ret_seq) };
}

fn::any_seq_t<gx::fasta_seq_t> gx::MakeFastaReader(
        std::istream& istr,
        size_t chunk_stride,
        size_t overlap_size,
        validate_iupacna_t validate_iupacna)
{
    VERIFY(chunk_stride > overlap_size);

    return fn::make_typerased(fn::seq(
        [    chunk_stride
          ,  overlap_size
          , validate_iupacna
          , get_next_line = rangeless::tsv::get_next_line(istr)
          ,      next_inp = fasta_seq_t()
          ,   reached_eof = false
        ] () mutable -> fasta_seq_t
    {
        const size_t target_chunk_size = chunk_stride + overlap_size;

        while (!reached_eof && next_inp.seq.size() < target_chunk_size) try {
            if (const auto& line = get_next_line(); !consume_fasta_line(line, validate_iupacna, next_inp)) {
                return std::exchange(next_inp, fasta_seq_t{ extract_seq_id(line), {}, line, 0UL, {} });
            }
        } catch (const fn::end_seq::exception&) { // from get_next_line()
            reached_eof = true;
        }

        // NB: sequence can be empty (defline-only) - GP-33468
        return next_inp.seq.size() >= target_chunk_size ? extract_chunk(next_inp, target_chunk_size, chunk_stride)
                             : !next_inp.seq_id.empty() ? std::exchange(next_inp, fasta_seq_t{}) // last remaining
                             :                            fn::end_seq();
    }));
}

na_t fasta_seq_t::s_complement(na_t na)
{
    static const std::array<na_t, 256> s_arr = [&]
    {
        std::array<na_t, 256> arr;
        for (const auto i : irange{ 256 }) {
            arr[i] = na_t(i);
        }

        static const char* bases    = "-ACMGRSVTWYHKDBNU";
        static const char* bases_rc = "-TGKCYSBAWRDMHVNA";
        const size_t n = strlen(bases);

        for (const auto i : irange{ n }) {
            const auto na_     = bases[i];
            const auto na_rc   = bases_rc[i];
            arr[(size_t)na_ ]              = (na_t)na_rc;
            arr[(size_t)std::tolower(na_)] = (na_t)std::tolower(na_rc);
        }

        return arr;
    }();

    return s_arr[(size_t)na];
}

static size_t s_get_fasta_line_width(size_t seq_len)
{
    static const int s_fasta_line_width = get_env("GX_FASTA_LINE_WIDTH", 80);
    return s_fasta_line_width > 0 ? s_fasta_line_width : seq_len;
}

// GP-31265
void gx::SplitFasta(std::istream& fasta_istr, std::ostream& ostr)
{
    static const auto min_n_run     = get_env("GX_FASTA_MIN_N_RUN"               , 10UL);
    static const auto min_chunk_len = get_env("GX_FASTA_MIN_REPORTABLE_CHUNK_LEN", 100UL);

    auto ivls = ivls_t{};
    auto n_run = ivl_t{};

    const auto push_n_run = [&]
    {
        if ((size_t)n_run.len >= min_n_run) {
            ivls.push_back(n_run);
        }
    };

    for (auto fasta_seq : gx::MakeFastaReader(fasta_istr)) {

        ivls.clear();
        n_run = ivl_t{};

        for (const auto i : irange{ fasta_seq.seq.size() }) {
            const auto c = fasta_seq.seq[i];
            if (c != 'N' && c != 'n') {
                ;
            } else if (n_run.endpos() == int32_t(i+1)) {
                n_run.len++; // extend current n-run
            } else {
                push_n_run();
                n_run.pos = int32_t(i+1);
                n_run.len = 1;
            }
        }
        push_n_run();

        const auto whole_ivl = ivl_t{ 1, (len_t)fasta_seq.seq.size() };
        ivls = ivl_t::invert(std::move(ivls), whole_ivl); // change to reportable-intervals.

        // GP-33472: preserve bounds info by emitting 1bp intervals at whole_ivl bounds
        if (ivls.front().pos != 1) {
            ivls.insert(ivls.begin(), ivl_t{ 1, 1 });
        }

        if (ivls.back().endpos() != whole_ivl.endpos()) {
            ivls.push_back(ivl_t{ whole_ivl.endpos() - 1, 1 });
        }

        ivls %= fn::where L((size_t)_.len >= min_chunk_len || _.pos == 1 || _.endpos() == whole_ivl.endpos());

        for (const auto ivl : ivls) {

            if (ivl == whole_ivl) {
                ostr << ">" << fasta_seq.seq_id << std::endl;
            } else {
                ostr << ">" << fasta_seq.seq_id << "~" << ivl.pos << ".." << ivl.endpos()-1 << std::endl;
            }
            VERIFY(ivl.pos > 0 && ivl.endpos() <= (len_t)fasta_seq.seq.size()+1);

            const auto fasta_line_width = (len_t)s_get_fasta_line_width(ivl.len);
            for (len_t i = 0; i < ivl.len; i += fasta_line_width) {
                ostr.write(fasta_seq.seq.data() + ivl.pos - 1 + i, std::min(fasta_line_width, ivl.len - i));
                ostr << "\n";
            }
            ostr << std::flush;
        }

        VERIFY(ostr);
    }
}

#if 0
struct id_ivl_t
{
    seq_id_str_t id = {};
    ivl_t ivl = {};
};

// parse seq-id in the form $SEQ_ID~START..STOP, e.g. lcl|xyz~123..456
static std::string parse_seq_id(const std::string& s) = ivl_id_t
{
    const auto tilde_pos = s.find('~');
    if (tilde_pos == std::string::npos) {
        return id_ivl_t{ seq_id_str_t{s}, ivl_t{} };
    }

    const auto dots_pos = s.find("..", tilde_pos);
    if (dots_pos == std::string::npos) {
        GX_THROW("Can't parse seq-id: " + s);
    }

    auto ivl = ivl_t{};

    ivl.pos = tsv::to_num(s.substr(tilde_pos + 1, dots_pos - tilde_pos - 1));
    const auto stop_pos = (int32_t)tsv::to_num(s.substr(dots_pos + 2));
    VERIFY(ivl.pos >= 0);
    VERIFY(stop_pos >= ivl.pos);
    ivl.len = stop_pos + 1 - ivl.pos;

    return id_ivl_t{ seq_id_str_t{s.substr(0, tilde_pos)}, ivl };
};
#endif


static void write_fasta_seq(std::ostream& ostr, const gx::fasta_seq_t& fasta_seq)
{
    const auto len = fasta_seq.seq.size();
    const auto fasta_line_width = s_get_fasta_line_width(len);

    ostr << fasta_seq.defline << "\n";
    for (size_t i = 0; i < len; i += fasta_line_width) {
        ostr.write(fasta_seq.seq.data() + i, std::min(fasta_line_width, len - i));
        ostr << "\n";
    }
    ostr << std::endl;
}

// GP-34579
void gx::ApplyActionReport(
        std::istream& fasta_istr, 
        std::istream& action_report_istr, 
        std::ostream& ostr, 
        std::ostream* contam_fasta_out_ofstr, // may be nullptr
        const size_t min_seq_len,
        const bool silent)
{
    namespace tsv = rangeless::tsv;

    /* Example row in action-report:
                        seq_id[  1 ]:  CAIR01000361.1
                     start_pos[  2 ]:  1
                       end_pos[  3 ]:  547
                       seq_len[  4 ]:  547
                        action[  5 ]:  EXCLUDE
                           div[  6 ]:  prok:b-proteobacteria
                  agg_cont_cov[  7 ]:  100
                  top_tax_name[  8 ]:  Delftia sp.
	*/
    ConsumeMetalineHeader(action_report_istr, GX_TSV_HEADER__FCS_GENOME_RPT);

    struct action_t
    {
        ivl_t ivl = ivl_t{};
        bool erase = true; // otherwise replace with Ns
    };

    // Load action-report
    auto actions   = std::map<seq_id_str_t, std::vector<action_t>>{};
    auto seq_lens  = std::map<seq_id_str_t, len_t>{};
    size_t row_num = 0;
    for (const tsv::row_t& row : tsv::from(action_report_istr)) {
        ++row_num;

        const auto exception_guard = make_exception_scope_guard([&]
        {
            std::cerr << "While loading row " << row_num << " of the action report:\n";
            for (const auto& f : row) {
                std::cerr << f << "\t";
            }
            std::cerr << std::endl;
        });

        if (str::startswith(row[0], "#") || (row.size() == 1 && row[0] == "")) {
			continue;
		}

        VERIFY(row.size() == 8);

        const auto seq_id = seq_id_str_t{ row[0] };
        const len_t start_pos = tsv::to_num(row[1]);
        const len_t stop_pos  = tsv::to_num(row[2]);
        const len_t seq_len   = tsv::to_num(row[3]);
        const auto& action    = row[4];
        const auto action_ivl = ivl_t{ start_pos, stop_pos + 1 - start_pos };

        {
            auto& len = seq_lens[seq_id];
            VERIFY(len == 0 || len == seq_len);
            len = seq_len;
        }

        VERIFY(1         <= start_pos);
        VERIFY(start_pos <= stop_pos);
        VERIFY(stop_pos  <= seq_len);

        VERIFY(action != "EXCLUDE" || ((start_pos == 1) && (stop_pos == seq_len)));
        VERIFY(action != "TRIM"    || ((start_pos == 1) ^  (stop_pos == seq_len)));
        VERIFY(action != "FIX"     || ((start_pos != 1) && (stop_pos != seq_len)));

        if (action == "TRIM" || action == "EXCLUDE" || action == "FIX") {
            actions[seq_id].push_back(action_t{ action_ivl, /*erase=*/action != "FIX" });
        }
    }

    for (auto& kv : actions) {
        kv.second %= fn::sort_by L(_.ivl);
        kv.second %= fn::reverse(); // will process actions back-to-front, such that 
                                    // length-changing ops do not affect next action's coordinates.
    }


    // Apply actions to the input fasta.
    auto actions_count = 0ul;
    size_t num_erased_bases = 0;
    size_t num_hardmasked_bases = 0;
    for (auto fasta_seq : gx::MakeFastaReader(fasta_istr)) {

        //if (!seq_lens.count(fasta_seq.seq_id)) {
        //    GX_THROW("Seq-id " + fasta_seq.seq_id + " is not in the action-report.\n");
        //}

        if (seq_lens.count(fasta_seq.seq_id) // It's fine if the sequence is missing from the action-report.
            && seq_lens.at(fasta_seq.seq_id) != (len_t)fasta_seq.seq.length())
        {
            GX_THROW("Unexpected sequence length. seq-id: "   + fasta_seq.seq_id
                                     + "; in fasta: "         + std::to_string(fasta_seq.seq.length())
                                     + "; in action-report: " + std::to_string(seq_lens.at(fasta_seq.seq_id)));
        }

        bool written_this_contam_seq = false;
        for (const auto& action : at_or_default(actions, fasta_seq.seq_id)) {

            // Write whole-sequence-exclude cases to contam_fasta_out_ofstr
            if (   action.erase
                && (size_t)action.ivl.len == fasta_seq.seq.size()
                && contam_fasta_out_ofstr
                && !written_this_contam_seq)
            {
                write_fasta_seq(*contam_fasta_out_ofstr, fasta_seq);
                written_this_contam_seq = true; // to output this seq only once, in case of multiple actions.
            }

            if (action.erase) {
                VERIFY(action.ivl.endpos() <= (int64_t)fasta_seq.seq.size() + 1);
                fasta_seq.seq.erase(action.ivl.pos - 1, action.ivl.len);
                num_erased_bases += action.ivl.len;
            } else {
                std::fill(fasta_seq.seq.begin() + action.ivl.pos - 1, 
                          fasta_seq.seq.begin() + action.ivl.endpos() - 1,
                          'N');
                num_hardmasked_bases += action.ivl.len;
            }
            ++actions_count;
        }

        if (fasta_seq.seq.size() >= min_seq_len) {
            write_fasta_seq(ostr, fasta_seq);

        } else if (   fasta_seq.seq.size() > 0 
                   && contam_fasta_out_ofstr 
                   && !written_this_contam_seq) // redirecting short seqs into the contam output.
        {
            write_fasta_seq(*contam_fasta_out_ofstr, fasta_seq);
            written_this_contam_seq = true;
        }
    }

    if (!silent) {
        std::cerr << "Applied " << actions_count << " actions; " 
                  << num_erased_bases << " bps dropped; " 
                  << num_hardmasked_bases << " bps hardmasked.\n";
    }
}


static const bool test_apply_action_report = []
{
    auto fasta_istr = std::stringstream{R"(
>seq1
ACGTA

>seq2
AAAAAAAAAAAAAAAA

>seq3
AA

>seq4
ACGT

)"};

    auto actions_istr = std::stringstream{"##[[\"FCS genome report\", 2, 1]]" + str::replace_all(R"(
seq1,1,2,5,TRIM,.,.,trim AC fram ACGTA -> GTA
seq1,4,4,5,FIX,.,.,replace T with N -> GNA
seq2,1,16,16,EXCLUDE,.,.,drop seq2
seq3,1,2,2,INFO,.,.,drop - too short (will test with min_seq_len=3)
seq4,1,4,4,INFO,.,.,preserve
)", ",", "\t")};

    auto expected_out = std::string{R"(
>seq1
GNA

>seq4
ACGT

)"};

    auto ostr = std::stringstream{};
    ApplyActionReport(fasta_istr, actions_istr, ostr, nullptr, 3, true);
    const auto actual_out = "\n" + ostr.str();

    if (expected_out != actual_out) {
        std::cerr << "----'" << expected_out << "'----'" << actual_out << "'\n";
        VERIFY(false);
    }

    return true;
}();


/////////////////////////////////////////////////////////////////////////////
void gx::GetFasta(const std::string& db_path, std::istream& istr, std::ostream& ostr)
{
    const auto sbj_infos  = seq_infos_t(ser::from_stream(open_ifstream(db_path)));
    const auto seq_id2oid = make_id2oid_map(sbj_infos);

    const std::string_view mmapped_seq_db = ser::mmap(str::replace(db_path, ".gxi", ".gxs"));

    // input is 3-column locs (seq-id, from1, to1)
    ConsumeMetalineHeader(istr, GX_TSV_HEADER__LOCS);
    namespace tsv = rangeless::tsv;

    for (const tsv::row_t& row : tsv::from(istr)) {
        const auto scope_guard = make_exception_scope_guard([&]
        {
            std::cerr << "Exception on row:";
            for (const auto& field : row) {
                std::cerr << " " << field;
            }
            std::cerr << "\n";
        });

        VERIFY(row.size() >= 3);
        const auto seq_id = seq_id_str_t{ row[0] };
        auto from1 = row[1] == "." ? pos1_t{} : (pos1_t)tsv::to_num(row[1]);
        auto to1   = row[2] == "." ? pos1_t{} : (pos1_t)tsv::to_num(row[2]);

        VERIFY((from1 <= to1 && from1 >= 0 && to1 >= 0) || (from1 == 0 && to1 == 0)); // when both are 0, it means whole-loc

        if (!seq_id2oid.count(seq_id)) {
            std::cerr << "Seq-id " << seq_id << " is not in db - skipping.\n";
            continue;
        }

        const seq_info_t& si = sbj_infos.at(seq_id2oid.at(seq_id));
        if(si.length == 0) {
            // E.g. alt-locus, or "UNVERIFIED:"
            std::cerr << "Seq-id " << seq_id << " was not in scope for indexing - skipping.\n";
            continue;
        }

        const auto sbj_seq = sbj_seq_t{ si, mmapped_seq_db.data() };

        // write the defline.
        ostr << ">" << seq_id;
        if (from1 != 0) {
            ostr << "~" << from1 << ".." << to1;
        } else {
            from1 = 1;
            to1 = (pos1_t)sbj_seq.size();
        }
        ostr << std::endl;

        VERIFY(to1 <= (int64_t)sbj_seq.size());

        const auto fasta_line_width = (len_t)s_get_fasta_line_width(to1 + 1 - from1);
        for (auto i = from1; i < to1 + 1; i += fasta_line_width) {
            for (const auto k : irange{ i, std::min(i + fasta_line_width, to1 + 1)}) {
                ostr << (char)sbj_seq.at1(k);
            }
            ostr << "\n";
        }
        ostr << std::flush;
    }
}
