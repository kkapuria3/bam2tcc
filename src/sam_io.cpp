/**
 * Functions for reading SAM/BAM files.
 */

#include <iostream>
#include <algorithm>      /* set_intersection, sort, unique */
#include <future>         /* Multithreading */
#include <seqan/bam_io.h>
#include "sam_io.hpp"
#include "util.hpp"
#include "semaphore.hpp"
using namespace std;

typedef seqan::FormattedFileContext<seqan::BamFileIn, void>::Type TBamContext;

#define GENOMEBAM_DEBUG 1
#define DEBUG 0

/**
 * @brief Get the value of the @PG:ID tag if present. Note: only works for SAM
 * files. Does not use SeqAn because there is no way I've found to extract
 * values from the header other than the @SQ fields.
 * @param filename Name of query SAM file.
 * @return         String value stored in tag, or empty string if not present.
 */
string get_sam_pg(string filename) {
    ifstream in(filename);
    if (!in.is_open()) {
        return "";
    }
    string pg;
    string inp;
    while (getline(in, inp)) {
        if (inp.size() != 0 && inp.substr(0, 3).compare("@PG") == 0) {
            int start = inp.find("ID:");
            if (start == string::npos) {
                break;
            }
            start += 3;
            int end = inp.find('\t', start);
            if (end == string::npos) {
                break;
            }
            pg = inp.substr(start, end - start);
            break;
        } else if (inp.size() != 0 && inp[0] != '@') {
            break;
        }
    }
    in.close();
    return pg;
}

/**
 * @brief Get total number of reads in a SAM/BAM file.
 *
 * Default is to not use SeqAn, since it seems to be slower.
 *
 * @param filename  Name of query SAM/BAM file
 * @return          Number of lines in file, or -1 if file fails to open.
 */
int get_sam_line_count(string filename, bool seqan=false) {
    int line_count = 0;
    if (seqan) {
        seqan::BamFileIn bam;
        if (!seqan::open(bam, filename.c_str())) {
            return -1;
        }
        seqan::BamHeader head;
        seqan::readHeader(head, bam);
        seqan::BamAlignmentRecord rec;
        while (!atEnd(bam)) {
            ++line_count;
            seqan::readRecord(rec, bam);
        }
    } else {
        ifstream in(filename);
        if (!in.is_open()) {
            return -1;
        }
        string inp;
        while (getline(in, inp)) {
            if (inp.size() != 0 && inp[0] != '@') {
                break;
            }
        }
        ++line_count;
        while (getline(in, inp)) {
            ++line_count;
        }
    }
    return line_count;
}

/**
 * @brief Gets "exons" of an alignment.
 *
 * Looks at CIGAR string to determine where a read was split up across different
 * exons, and returns a vector containing the exons of the read.
 *
 * @param r         Alignment to examine.
 * @return          Vector of alignment exons.
 */
vector<Exon> get_alignment_exon_positions(seqan::BamAlignmentRecord &r) {
    vector<Exon> exons;
    int start = r.beginPos, end = start;
    for (uint i = 0; i < seqan::length(r.cigar); ++i) {
        switch (r.cigar[i].operation) {
            case 'M':
            case 'D':
            case '=':
            case 'X': end += r.cigar[i].count;
                      break;
            case 'N': exons.push_back(Exon(start, end));
                      start = end + r.cigar[i].count;
                      end = start;
                      break;
            default: /* do nothing */ break;
        }
    }
    if (end == r.beginPos) {
        cerr << "  COMPLAINS LOUDLY" << endl;
    }
    exons.push_back(Exon(start, end));
    return exons;
}

/**
 * @brief Gets all possible transcripts associated with read exon vector (for
 * use with get_alignment_exon_positions function). That is, the function fills
 * in the "transcripts" member of the exon struct.
 *
 * @param chrom         Exons of the chromosome/scaffold the read aligns to.
 * @param read_exons    Vector of exons to fill with transcript information.
 * @postcondition       Transcripts member of exons in read_exons filled.
 */
void get_alignment_exon_transcripts(const vector<Exon> &chrom,
                               vector<Exon> &read_exons) {
   
    for (uint i = 0; i < chrom.size(); ++i) {
        for (uint j = 0; j < read_exons.size(); ++j) {
            if (read_exons[j].start >= chrom[i].start
                && read_exons[j].end <= chrom[i].end
#if !GENOMEBAM_DEBUG
                /* Makes sure that transcripts fall exactly across the splice
                 * junction. kallisto doesn't check this. */
                && (j == 0 || read_exons[j].start == chrom[i].start)
                && (j == read_exons.size() - 1
                    || read_exons[j].end == chrom[i].end)
#endif
                                                        ) {
               read_exons[j].transcripts->insert(
                    read_exons[j].transcripts->end(),
                    chrom[i].transcripts->begin(), chrom[i].transcripts->end());
            }
        }
    }
}

/**
 * @brief Gets string representation of the equivalence class of a read
 * based on the information contained in vector exons.
 *
 * @param exons     Vector containing information about annotated sequences
 * as generated by readGTFs method.
 *
 * @param rec       Record containing information about this alignment.
 *
 * @return          Vector containing the transcripts composing the equivalence
 * class.  Transcripts are described by an index which corresponds either to
 * where it showed up in the GFF(s), or to where it shows up in the FASTA
 * transcriptome file if that option was used. See readGFFs in gff_io.cpp for
 * more info.
 */
vector<int> getEC(const unordered_map<string, vector<Exon>*> &exons,
        TBamContext &cont, seqan::BamAlignmentRecord rec) {
    /* Find the vector describing the chromosome/scaffold this read aligned
     * to. If we can't find it, return immediately.*/
    auto it = exons.find(lower(string(seqan::toCString(
            seqan::contigNames(cont)[rec.rID]))));
    if (it == exons.end()) {    
        return {};
    }
    vector<Exon> *chrom = it->second;

    /* Get "exons" of this read and fill in their associated transcript
     * vectors. */
    vector<Exon> read_exons = get_alignment_exon_positions(rec);
    get_alignment_exon_transcripts(*chrom, read_exons);
    
    /* Take the intersection of all the transcripts the read exons aligned
     * to, which gives the equivalence class of this read. */
    vector<int> inter(*read_exons[0].transcripts);
    sort(inter.begin(), inter.end()); 
    for (uint i = 1; i < read_exons.size(); ++i) {
        vector<int> temp;
        vector <int> *curr_transcripts = read_exons[i].transcripts;
        /* Sort transcripts in preparation for set_intersection. */
        sort(curr_transcripts->begin(), curr_transcripts->end());
        set_intersection(inter.begin(), inter.end(),
                         curr_transcripts->begin(), curr_transcripts->end(),
                         back_inserter(temp));
        inter = temp;
    }
    /* Remove duplicates. inter is already sorted! */
    inter.erase(unique(inter.begin(), inter.end()), inter.end());
    return inter;
}

/**
 * @brief Get equivalence class of the read stored in `curr`.
 *
 * This function, and those that it calls, refer to a number of different ECs.
 * To clarify, there is the EC, the alignment EC, and the read EC. The EC is the
 * equivalence class of a single alignment (line) in the SAM file. The alignment
 * EC is equivalent to the EC for single-end reads. But for paired-end reads,
 * the alignment EC describes the equivalence class of a pair of alignments
 * (lines) in the SAM. Reads may multimap, and therefore have multiple
 * alignments in the SAM file. The read EC is the equivalence class of the read
 * overall.
 *
 * @param exons     Vector containing information about annotated sequences
 * as generated by readGTFs method.
 * 
 * @param cont              Context of input SAM needed by Seqan.
 *
 * @param curr              Vector holding alignments describing the read.
 * curr[0] should hold all alignments of one segment of the template, and
 * curr[1] all alignments of the other segment. This program assumes that there
 * are at most two segments per template. (See SAM specifications on the
 * language used.)
 *
 * @return                  A vector describing the equivalence class of the
 * read.
 */
vector<int> getReadEC(unordered_map<string, vector<Exon>*> &exons,
        TBamContext &cont, vector<vector<seqan::BamAlignmentRecord>> &curr,
        bool rapmap, bool paired) {
#if DEBUG || GENOMEBAM_DEBUG
    string qname;
    if (curr[0].size() != 0) {
        qname = seqan::toCString(curr[0][0].qName);
    } else {
        qname = seqan::toCString(curr[1][0].qName);
    }
#endif

#if DEBUG
    cout << endl << qname << ": " << curr[0].size() << '\t' << curr[1].size() << endl;
#endif

#if GENOMEBAM_DEBUG
    /* kallisto doesn't allow orphaned reads. */
    if (paired && (curr[0].size() == 0 || curr[1].size() == 0)) {
        return {};
    }
#endif

#if DEBUG && GENOMEBAM_DEBUG
    bool all_unmapped = true;
#endif

    vector<int> temp;

    vector<int> ECforward;
    vector<int> ECreverse;
    for (auto r = curr[0].begin(); r != curr[0].end(); ++r) {
        if (seqan::hasFlagUnmapped(*r)) {
            continue;
        }
#if DEBUG && GENOMEBAM_DEBUG
        all_unmapped = false;
#endif
        if (rapmap) {
            temp = {r->rID};
        } else {
           temp = getEC(exons, cont, *r);
        }
#if DEBUG
        cout << qname << " at " << r->beginPos << ": " << flush;
        for (int i = 0; i < temp.size(); ++i) {
            cout << temp[i] << '\t' << flush;
        }
        cout << endl;
#endif
        if (seqan::hasFlagRC(*r)) {
            ECreverse.insert(ECreverse.end(), temp.begin(), temp.end());
        } else {
            ECforward.insert(ECforward.end(), temp.begin(), temp.end());
        }
    }

    vector<int> EC2forward;
    vector<int> EC2reverse;
    for (auto r = curr[1].begin(); r != curr[1].end(); ++r) {
        if (seqan::hasFlagUnmapped(*r)) {
            continue;
        }
#if DEBUG && GENOMEBAM_DEBUG
        all_unmapped = false;
#endif
        if (rapmap) {
            temp = {r->rID};
        } else {
            temp = getEC(exons, cont, *r);
        }
#if DEBUG
        cout << qname << " at " << r->beginPos << ": " << flush;
        for (int i = 0; i < temp.size(); ++i) {
            cout << temp[i] << '\t' << flush;
        }
        cout << endl;
#endif
        if (seqan::hasFlagRC(*r)) {
            EC2reverse.insert(EC2reverse.end(), temp.begin(), temp.end());
        } else {
            EC2forward.insert(EC2forward.end(), temp.begin(), temp.end());
        }
    }
        
    vector<int> EC;
#if GENOMEBAM_DEBUG
    if ((ECforward.size() != 0 || ECreverse.size() != 0)
            && (EC2forward.size() != 0 || EC2reverse.size() != 0)) {
        /* kallisto doesn't care about which strand the segments align to. */
        ECforward.insert(ECforward.end(), ECreverse.begin(), ECreverse.end());
        EC2forward.insert(EC2forward.end(), EC2reverse.begin(),
                EC2reverse.end());
        sort(ECforward.begin(), ECforward.end());
        sort(EC2forward.begin(), EC2forward.end());
        set_intersection(ECforward.begin(), ECforward.end(),
                EC2forward.begin(), EC2forward.end(), back_inserter(EC));
#else
    if (paired) {
        sort(ECforward.begin(), ECforward.end());
        sort(ECreverse.begin(), ECreverse.end());
        sort(EC2forward.begin(), EC2forward.end());
        sort(EC2reverse.begin(), EC2reverse.end());
        set_intersection(ECforward.begin(), ECforward.end(),
                EC2reverse.begin(), EC2reverse.end(), back_inserter(EC));
        set_intersection(ECreverse.begin(), ECreverse.end(),
                EC2forward.begin(), EC2forward.end(), back_inserter(EC));
#endif   
    } else {
#if GENOMEBAM_DEBUG
        if (ECforward.size() == 0 && ECreverse.size() == 0) {
            ECforward = EC2forward;
            ECreverse = EC2reverse;
        }
#endif
        ECforward.insert(ECforward.end(), ECreverse.begin(), ECreverse.end());
        EC = ECforward;
    }

    sort(EC.begin(), EC.end());
    EC.erase(unique(EC.begin(), EC.end()), EC.end());
#if DEBUG && GENOMEBAM_DEBUG
    /* Any read that shows up in genomebam without the `unmapped` flag should
     * map to something here, too. Otherwise, kallisto can't give genomic
     * coordinates in the first place. */
    if (!all_unmapped && EC.size() == 0) {
        cout << qname;
    }
#endif

#if DEBUG
    cout << qname << ": " << flush;
    for (int i = 0; i < EC.size(); ++i) {
        cout << EC[i] << '\t' << flush;
    }
    cout << endl;
#endif

    return EC;
}

/**
 * @brief Reads input SAM/BAM file and populates `matrix` with TCC counts.
 * Prints error message if file cannot be opened.
 *
 * @param file              Name of SAM/BAM file. 
 *
 * @param filenumber        "Number" of this SAM/BAM file corresponding to the
 * order in which query SAM/BAM files were entered into command line.
 *
 * @param start             The number of the read in this file to start at.
 *
 * @param end               The number of the read in this file to stop at. This
 * read (read # ${end}) will not be read.
 *
 * @param thread            Number describing which thread runs this function.
 * For debugging purposes.
 *
 * @param exons             Vector containing information from GFF. (See
 * readGFFs in gff_io.cpp for more info.
 *
 * @param matrix            The TCC matrix to populate with information about
 * TCC counts for this SAM/BAM file.
 *
 * @param unmatched_outfile The name of the file to which to write unmatched
 * reads.
 *
 * @param sem               Semaphore to control terminal output when multiple
 * threads are in use.
 *
 * @postcondition           `matrix` is populated with information about TCC
 * counts of reads in given SAM/BAM file.
 *
 * @return -1 if file fails to open, else 0
 */
int readSAMHelp(string file, int filenumber,
        int start, int end, int thread,
        unordered_map<string, vector<Exon>*> &exons, TCC_Matrix &matrix,
        string unmatched_outfile, int verbose, Semaphore &sem,
        bool rapmap, bool paired, bool all_same) {

    if (end - start <= 1) {
        return -1;
    }
    uint64_t line_count = 0;
    seqan::BamFileIn bam;
    if (!seqan::open(bam, file.c_str())) {
        sem.dec();
        cerr << "    ERROR: failed to open " << file << endl;
        sem.inc();
        return -1;
    }

    /* Everything after this is a bit convoluted. Mostly because I can't find a
     * way to "unread" a line, as it were. */
    seqan::BamAlignmentRecord rec;
    seqan::BamHeader header;
    seqan::readHeader(header, bam);
    TBamContext cont = context(bam);
    vector<vector<seqan::BamAlignmentRecord>> curr;
    curr.push_back(vector<seqan::BamAlignmentRecord>());
    curr.push_back(vector<seqan::BamAlignmentRecord>());
    seqan::BamFileOut unmatched_out;
    if (unmatched_outfile.size() != 0) {
        seqan::open(unmatched_out, unmatched_outfile.c_str(),
                seqan::OPEN_APPEND);
    }
    
    /* Read in the first line. */
    string qName;
    string recqName;
    if (start == 0) {
        ++line_count;
        seqan::readRecord(rec, bam);
    } else {
        while (line_count < start - 1) {
            ++line_count;
            seqan::readRecord(rec, bam);
        }
        qName = seqan::toCString(rec.qName);
        if (!all_same) {
            qName = qName.substr(0, qName.size() - 2);
        }
        ++line_count;
        seqan::readRecord(rec, bam);
        /* If `start` is in the middle of a multimapping (multientry) read,
         * keep going until it's done. */
        recqName = seqan::toCString(rec.qName);
        if (!all_same) {
            recqName = recqName.substr(0, recqName.size() - 2);
        }
        while (!atEnd(bam) && qName.compare(recqName) == 0) {
            ++line_count;
            seqan::readRecord(rec, bam);
            recqName = seqan::toCString(rec.qName);
            if (!all_same) {
                recqName = recqName.substr(0, recqName.size() - 2);
            }
        }
    }
    while (line_count < end) {
        qName = seqan::toCString(rec.qName);
        if (!all_same) {
            qName = qName.substr(0, qName.size() - 2);
        }
        recqName = qName;
        while (qName.compare(recqName) == 0) {
#if GENOMEBAM_DEBUG
            if (!(paired && rec.rID != rec.rNextId)) {
#else
            if (!(seqan::hasFlagMultiple(rec) && rec.rID != rec.rNextId)
                && !seqan::hasFlagUnmapped(rec)
                && !(seqan::hasFlagMultiple(rec)
                    && !seqan::hasFlagAllProper(rec))) {
#endif
                if (seqan::hasFlagLast(rec)) {
                    curr[1].push_back(rec);
                } else {
                    curr[0].push_back(rec);
                }
            }
            ++line_count;
            if (seqan::atEnd(bam)) {
                break;
            }
            seqan::readRecord(rec, bam);
            recqName = seqan::toCString(rec.qName);
            if (!all_same) {
                recqName = recqName.substr(0, recqName.size() - 2);
            }
        }
        vector<int> EC = getReadEC(exons, cont, curr, rapmap, paired);
        if (EC.size() == 0) {
            /* Deal with unmapped output */
        } else {
            string stringEC = to_string(EC[0]);
            for (int i = 1; i < EC.size(); ++i) {
                stringEC += ',' + to_string(EC[i]);
            }
            matrix.inc_TCC(stringEC, filenumber);
        }
        curr[0].clear();
        curr[1].clear();
    }
    return 0;
}

int readSAM(string file, int filenumber,
             unordered_map<string, vector<Exon>*> &exons, TCC_Matrix &matrix,
             string unmatched_outfile, int verbose, int nthreads,
             bool rapmap, bool paired) {

    cout << "  Reading " << file << flush;

    vector<future<int>> threads;
    Semaphore sem;
    int lines = get_sam_line_count(file);
    if (lines == -1) {
        cerr << endl << "    ERROR: failed to open " << file << endl;
        return -1;
    }

#if GENOMEBAM_DEBUG
    cout << " with GENOMEBAM_DEBUG=true" << flush;
#endif
    string pg = get_sam_pg(file);
    if (pg.compare("rapmap") == 0) {
        rapmap = true;
        cout << " using format RapMap" << flush;
    }
    cout << "..." << endl;

    seqan::BamFileIn bam;
    if (!seqan::open(bam, file.c_str())) {
        cerr << "    ERROR: failed to open " << file << endl;
        return -1;
    }
    seqan::BamHeader header;
    seqan::readHeader(header, bam);

    /* Write header of unmatched SAM if necessary. */
    if (unmatched_outfile.size() != 0) {
        seqan::BamFileOut unmatched_out(seqan::context(bam),
            unmatched_outfile.c_str());
        seqan::writeHeader(unmatched_out, header);
        seqan::close(unmatched_out);
    }

    /* Try to figure out naming convention of reads, i.e. are pairs
     * labelled with the same `QNAME` or different? I'm assuming here that no
     * one is going to give me a dataset with like two reads. Also that it will
     * be in order, i.e. I will not see read x.2 before x.1. */
    bool all_same = true;
    if (paired) {
        seqan::BamAlignmentRecord rec;
        bool one_seen = false, two_seen = false;
        while (!seqan::atEnd(bam)) {
            seqan::readRecord(rec, bam);
            string qName = seqan::toCString(rec.qName);
            if (qName.size() < 2) {
                break;
            }
            if (!isdigit(qName[qName.size() - 2])) {
                if (qName[qName.size() - 1] == '1') {
                    if (one_seen && two_seen) {
                        all_same = false;
                        break;
                    } else {
                        one_seen = true;
                    }
                } else if (qName[qName.size() - 1] == '2') {
                    two_seen = true;
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    seqan::close(bam);

    /* Launch threads that do the actual work. */
    for (int j = 0; j < nthreads - 1; ++j) {
        threads.push_back(async(launch::async, &readSAMHelp,
                file, filenumber,
                lines / nthreads * j, lines / nthreads * (j + 1), j,
                ref(exons), ref(matrix), unmatched_outfile, verbose,
                ref(sem), rapmap, paired, all_same));
    }
    threads.push_back(async(launch::async, &readSAMHelp,
            file, filenumber,
            lines / nthreads * (nthreads - 1), lines + 1, nthreads - 1,
            ref(exons), ref(matrix), unmatched_outfile, verbose,
            ref(sem), rapmap, paired, all_same));

    for (int i = 0; i < threads.size(); ++i) {
        if (threads[i].valid()) {
            threads[i].wait();
        }
        else {
            cout << "invalid state?" << endl;
        }
        int err = threads[i].get();
        if (err == -1) {
            cerr << "  WARNING: thread " << i << " failed" << endl;
        }
    }

    return 0;
}

