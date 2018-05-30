/**
 * Functions to generate kallisto-esque output, i.e. to map TCCs onto
 * kallisto's.
 */

#include <fstream>
#include <iostream>
#include <algorithm>

#include "kallisto_util.hpp"
#include "structs.hpp"
#include "file_io.hpp"
#include "util.hpp"

using namespace std;

#define TRANSCRIPT_NAME_END_CHAR '.'


/**
 * @brief Fill map with index->seqid strings from a single GTF file. See
 * get_index_to_seqid for more info about algorithm.
 *
 * @param file              Name of input GTF file.
 * @param map               Map to fill with index->seqid info.
 * @param transcript_count  Number at which to start indexing this file.
 * @return                  1 if file fails to open, otherwise 0.
 */
int get_index_to_seqid_help(string file, unordered_map<uint64_t, string> &map,
                            uint64_t &transcript_count) {
    
    ifstream f(file); // The GTF file.
    if (!f.is_open()) {
        return 1;
    }
    
    string inp; // A line in the file. For use with getline.
    string prev_seqname = "", prev_id = "";
    
    while (getline(f, inp)) {
        // Parse input and put information in seq.
        inp = lower(inp);
        Sequence seq = get_sequence(inp);
        
        // If the sequence was properly read and it's an exon...
        if (seq.start != 0 && seq.seqname.size() != 0
            && seq.feature.compare("exon") == 0) {
            
            // If it's a new transcript, increment transcript_count.
            if (prev_seqname.compare(seq.seqname) != 0
                || prev_id.compare(seq.id) != 0) {
                
                ++transcript_count;
                prev_seqname = seq.seqname;
                prev_id = seq.id;
            }
            
            // Add it to the map!
            map.emplace(transcript_count, seq.id);
        }
    }
    
    f.close();
    return 0;
}

/**
 * @brief Fill map with index->seqid strings from multiple GTF files. Prints an
 * error message if any file fails to open and returns 1.
 *
 * The readGTFs function goes through each entry in order from top
 * to bottom, and the main function reads through GTFs in the order in which
 * the user inputs them. So, the order in which exons are seen and consequently
 * numbered is deterministic. Helper function get_index_to_seqid_help replicates
 * the readGTF read order and maps read number, i.e. index of the transcript, to
 * the transcript_id as given in the GTF. Vector <files> MUST receive file names
 * in the same order that readGTFs did for function to work.
 *
 * @param files     Vector holding names of input GTF files. They must appear
 * in the same order as they do in the input parameter to readGTFs.
 *
 * @param map       Map to fill with index->seqid info.
 *
 * @return          1 if a file fails to open, otherwise 0.
 */
int get_index_to_seqid(const vector<string> &files,
                       unordered_map<uint64_t, string> &map) {
    
    // 0-indexed transcript counts. This will start count at 0. We do it this
    // way to match how we do it in readGTF, where it's necessary that we start
    // it at -1.
    uint64_t transcript_count = -1;
    
    for (uint i = 0; i < files.size(); ++i) {
        int ret = get_index_to_seqid_help(files[i], map, transcript_count);
        if (ret == 1) {
            cerr << "  ERROR: could not read " << files[i] << endl;
            return 1;
        }
    }
    
    return 0;
}

/**
 * @brief Fill map with id->kallisto index strings from a single FASTA. ID is
 * the transcript_id in a GTF file. See get_id_to_kallisto_index for more info
 * about algorithm.
 *
 * @param file              Input FASTA file containing transcriptome.
 * @param map               Map to fill with id->kallisto index info.
 * @param transcript_count  Number at which to start indexing this file.
 * @return                  1 if file fails to open, otherwise 0.
 */
int get_id_to_kallisto_index_help(string file,
                                  unordered_map<string, uint64_t> &map,
                                  uint64_t &transcript_count) {
    ifstream f(file);
    if (!f.is_open()) {
        return 1;
    }
    
    string inp;
    while (getline(f, inp)) {
        if (inp.size() == 0 || inp[0] != '>') {
            continue;
        }
        
        ++transcript_count;
        
        inp = lower(inp);
        uint first_space = inp.find(TRANSCRIPT_NAME_END_CHAR);
        // Exclude the first character ('>') and the END CHAR.
        map.emplace(inp.substr(1, first_space - 1), transcript_count);
    }
    
    f.close();
    return 0;
}

/**
 * @brief Fill map with id->kallisto index strings from multiple FASTAs. ID is
 * the transcript_id in a GTF file. Prints error message if any file fails to
 * open and returns 1.
 *
 * kallisto assigns indices based on the order in which transcripts show up in
 * the transcriptome file. This function simply goes through the transcripts,
 * looks at the ID (function currently assumes this will be the first word, i.e.
 * up to the first space, and two transcripts will not share the same ID), and
 * matches it up against a transcript count, the index.
 *
 * @param files             Vector of names of FASTA files containing
 * transcriptome.
 *
 * @param map               Map to fill with id->kallisto index info.
 *
 * @return                  1 if file fails to open, otherwise 0.
 */
uint64_t get_id_to_kallisto_index(const vector<string> &files,
                             unordered_map<string, uint64_t> &map) {
    
    // 0-indexed transcript counts. This will make count start at 0. We do it
    // this way to match the way readGTF does it.
    uint64_t transcript_count = -1;
    
    for (uint i = 0; i < files.size(); ++i) {
        int ret = get_id_to_kallisto_index_help(files[i], map,
                                                transcript_count);
        if (ret == 1) {
            cerr << "  ERROR: could not read " << files[i] << endl;
            return -1;
        }
    }
    
    return transcript_count;
}

/**
 * @brief Fill map with index->kallisto index pairs.
 *
 * Goes through GTF and transcriptome and matches the index assigned by this
 * program's algorithm when reading the GTF with the index kallisto assigns to
 * it. Both assign the indices based on the order that transcripts appear
 * in the files. So, if there are multiple files, they must be listed in the
 * same order every time for this to work.
 *
 * @param gtf               List of GTF file names.
 *
 * @param transcriptome     List of transcriptome file names. These should be
 * FASTA files.
 *
 * @param map               Map to fill with index --> kallisto index pairs.
 *
 * @param verbose           If true, print out warning messages.
 */
int get_index_to_kallisto_index(const vector<string> &gtf,
                                const vector<string> &transcriptome,
                                unordered_map<uint64_t, uint64_t> &map,
                                int verbose) {
    
    // We want to use maps allocated on the heap since we might have an awful
    // lot of transcripts.
    
    // Map from index to transcript_id, which should match...
    unordered_map<uint64_t, string> *m1 = new unordered_map<uint64_t, string>;
    int err = get_index_to_seqid(gtf, *m1);
    if (err == 1) {
        // get_index_to_seqid prints its own error message, since it knows which
        // file failed to open.
        return 1;
    }
    
    // ... the transcript IDs in the FASTA files, which are mapped here to the
    // kallisto index.
    unordered_map<string, uint64_t> *m2 = new unordered_map<string, uint64_t>;
    // Save where we left off, so we can appropriately index those transcripts
    // which appear in the GTF but not in the transcriptome.
    uint64_t index = get_id_to_kallisto_index(transcriptome, *m2);
    if (index == -1) {
        // Again, it prints its own error message.
        return 1;
    }
    
    if (verbose) {
        if (m1->size() > m2->size()) {
            cerr << "  WARNING: GTF(s) contain more entries than the ";
            cerr << "transcriptome file(s)!" << endl;
        }
        else if (m1->size() < m2->size()) {
            cerr << "  WARNING: Transcriptome file(s) contain more entries ";
            cerr << "than the GTF files(s)!" << endl;
        }
    }
    
    // A place to store GTF transcripts with no match in the transcriptome
    // files. Since we don't know how many to expect, and it can be as high as
    // the number of total transcripts in the GTF files, we'll use the heap.
    vector<uint64_t> *unfound = new vector<uint64_t>;
    
    // Now, we want to get all of this information into one map. Iterate through
    // m1, since only want to look at the transcripts in the GTFs. If m2, the
    // transcriptome map, has more entries, we don't need to do anything with
    // them.
    for (unordered_map<uint64_t, string>::iterator it = m1->begin();
         it != m1->end(); ++it) {
        
        // Try to access corresponding kallisto index. There may not be a match!
        // But this isn't case-sensitive, since all strings are lowercase-only.
        try {
            map.emplace(it->first, m2->at(it->second));
        }
        // There's no match in transcriptome files. Add it to the vector of
        // unmatched indices.
        catch (out_of_range &err) {
            unfound->push_back(it->first);
        }
    }
    
    // Take care of the unfound indices vector. Add them into the map with
    // newly assigned indices starting where kallisto's indexing ended, which
    // we can find out by looking at the size of m2, which holds every
    // transcript in the transcriptome files.
    for (uint i = 0; i < unfound->size(); ++i) {
        map.emplace((*unfound)[i], index);
        ++index;
    }
    
    // Clean-up.
    delete m1;
    delete m2;
    delete unfound;
    
    return 0;
}

/**
 * Note: currently only operates from thing --> kallisto. Another function
 * that works in other direction? boolean input to this one?
 * Also this function has never been tested... though it does compile...
 */
int change_index(const vector<string> &gtf, const vector<string> &transcriptome,
               string in_ec, string out_ec) {
    
    /* Open files. If it fails, return -1. */
    ifstream in(in_ec);
    if (!in.is_open()) {
        return 1;
    }
    ofstream out(out_ec);
    if (!out.is_open()) {
        return 1;
    }
    
    /* Get maps */
    unordered_map<uint64_t, string> *m1 = new unordered_map<uint64_t, string>;
    int err = get_index_to_seqid(gtf, *m1);
    if (err == 1) {
        return 1;
    }
    unordered_map<string, uint64_t> *m2 = new unordered_map<string, uint64_t>;
    err = get_id_to_kallisto_index(transcriptome, *m2);
    if (err == 1) {
        return 1;
    }
    
    string inp;
    while (getline(in, inp)) {
        vector<string> line = parse_tsv(inp);
        if (line.size() != 2) {
            cerr << "  ERROR: Each line should contain only two fields" << endl;
            return 1;
        }
        vector<string> eq = parse_csv(line[1]);
        string new_eq = "";
        for (int i = 0; i < eq.size(); ++i) {
            try {
                new_eq += to_string(m2->at(m1->at(stoi(eq[i])))) + ',';
            }
            catch (out_of_range &e) {
                cerr << "  ERROR: ID not found" << endl;
                return 1;
            }
        }
        sort(eq.begin(), eq.end());
        
        out << line[0] << '\t' << new_eq.substr(0, new_eq.size() - 1) << endl;
    }
    
    delete m1;
    delete m2;
    
    in.close();
    out.close();
    return 0;
}

int get_kallisto_ec_order(string ec, vector<string> &v, set<string> &s) {
    ifstream f(ec);
    if (!f.is_open()) {
        return 1;
    }
    
    string inp;
    while(getline(f, inp)) {
        inp = lower(inp);
        vector<string> line = parse_tsv(inp);
        v.push_back(line[1]);
        s.emplace(line[1]);
    }
    
    return 0;
}
