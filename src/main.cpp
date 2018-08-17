/**
 * Main function. Generates ec and tsv files.
 */
#include <iostream>           // warning and message i/o
#include <string>
#include <vector>
#include <time.h>             // provides total time elapsed
#include <getopt.h>           // getopt_long to parse commandline args
#include <set>
#include <seqan/gff_io.h>     /* Test-open of GFF and SAM/BAM files */
#include <seqan/bam_io.h>

#include "exon.hpp"
#include "gff_io.hpp"
#include "sam_io.hpp"
#include "util.hpp"
#include "kallisto_util.hpp"
#include "TCC_Matrix.hpp"

using namespace std;

/**
 * Parse commandline arguments. Test-open relevant files and abort if a file
 * fails to open. Allocate relevant data structures and fill with information
 * from GTFs and SAMs. Write to output files, deallocate data structures, and 
 * return.
 */
int main(int argc, char **argv) {
    string usage = "Usage:\n  " + string(argv[0]) + " [options]* -g <GTF> ";
    usage += "-S <SAM> [-o <output>]\n";
    usage += "  <GTF>               Comma-separated list of GTF gene ";
    usage +=                       "annotation files. Not required with";
    usage +=                       " rapmap option.\n";
    usage += "  <SAM>               Comma-separated list of ";
    usage +=                       "SAM/BAM files containing aligned ";
    usage +=                       "single-end reads\n";
    usage += "  <output>            Name of output file (defaults to ";
    usage +=                       "matrix.ec, matrix.tsv, matrix.cells)\n";
    usage += "\nOptions:\n";
    usage += "  -U                         Indicate that reads are unpaired.\n";
    usage += "  -p, --threads <int>        Max number of threads to use.";
    usage +=                              " Defaults to 1.\n";
    usage += "  -q                         Suppresses some warnings and ";
    usage +=                              "status updates\n";
    usage += "  -t, --transcriptome <fa>   Change TCC numbering so it ";
    usage +=                              "those that would be generated by ";
    usage +=                              "kallisto using transcriptome(s) ";
    usage +=                              "<fa>. Takes a comma-separated ";
    usage +=                              "of file names\n";
    usage += "  -r, --rapmap               Indicate that the <SAM> is a RapMap";
    usage +=                              " \"lightweight\" file. Program ";
    usage +=                              "can also infer this from the header";
    usage +=                              " tag @PG:ID \n";
    usage += "  -e, --ec <ec>              Output TCCs in the same order as in";
    usage +=                              " input file ec.\n";
    usage += "  --full-matrix              Output full (non-sparse) matrix. ";
    usage +=                              "Defaults to sparse matrix output.\n";
    usage += "  -u, --unmatched <SAM>      Output unmatched reads to ";
    usage +=                              "file <SAM>. Default ";
    usage +=                              "setting ignores these reads. ";
    usage +=                              "Currently writes a pretty buggy ";
    usage +=                              "header.\n";

    /* Parse commandline arguments */
    vector<string> gtf_files; // annotated sequence files
    vector<string> sam_files; // files with information about reads
    vector<string> transcriptome_files; // optional--transcriptomes input into
                                        // kallisto, so that this program will
                                        // modify its own output so that TCC
                                        // numbering matches
    string kallisto_ec; // a kallisto EC file, for use if user wants TCC indexes
                        // to match those of this kallisto output
    
    string out_name = "matrix", unmatched_out = ""; // default outfile names
    // Some booleans
    bool rapmap = false, paired = true;
    int err, verbose = 1, full = 0;
    int threads = 1, num_transcripts = 0;
    time_t start = time(0); // to calculate total runtime

    // for use with getopt_long
    struct option long_opts[] = {
        {"unpaired", no_argument, no_argument, 'U'},
        {"threads", required_argument, 0, 'p'},
        {"quiet", no_argument, no_argument, 'q'},
        {"unmatched", required_argument, 0, 'u'},
        {"gtf", required_argument, 0, 'g'},
        {"sam", required_argument, 0, 's'},
        {"output", required_argument, 0, 'o'},
        {"transcriptome", required_argument, 0, 't'},
        {"rapmap", no_argument, no_argument, 'r'},
        {"ec", required_argument, 0, 'e'},
        {"full-matrix", no_argument, no_argument, 'f'}
    };

    // use getopt_long to parse arguments
    int opt_index = 0;
    while(1) {
        int c = getopt_long(argc, argv,
                "Up:qu:g:S:o:t:re:f", long_opts, &opt_index);
        if (c == -1) { break; }
        switch (c) {
            case 'U':   paired = false;
                        break;
            case 'p':   threads = atoi(optarg);
                        break;
            case 'q':   verbose = 0;
                        break;
            case 'u':   unmatched_out = optarg;
                        break;
            case 'g':   gtf_files = parse_csv(optarg);
                        break;
            case 'S':   sam_files = parse_csv(optarg);
                        break;
            case 'o':   out_name = optarg;
                        break;
            case 't':   transcriptome_files = parse_csv(optarg);
                        break;
            case 'r':   rapmap = true;
                        break;
            case 'e':   kallisto_ec = optarg;
                        break;
            case 'f':   full = 1;
                        break;
        }
    }

    if (sam_files.size() == 0) {
        cerr << usage << flush;
        return 1;
    }
    if (!rapmap && gtf_files.size() == 0) {
        cerr << usage << flush;
        return 1;
    }


    /* Check that all files are valid by trying to open them */
    cout << "Checking that all files are valid ";
    cout << "and clearing output files... " << flush;
    for (uint i = 0; i < gtf_files.size(); ++i) {
        seqan::GffFileIn gff;
        if (!seqan::open(gff, gtf_files[i].c_str())) {
            cerr << endl << "  ERROR: Failed to open " << gtf_files[i] << endl;
            return 1;
        }
        seqan::GffRecord rec;
        seqan::readRecord(rec, gff);
        seqan::close(gff);
    }
    for (uint i = 0; i < sam_files.size(); ++i) {
        seqan::BamFileIn bam;
        if (!seqan::open(bam, sam_files[i].c_str())) {
            cerr << endl <<  "  ERROR: failed to open " << sam_files[i] << endl;
            return 1;
        }
        seqan::BamHeader head;
        seqan::readHeader(head, bam);
        seqan::BamAlignmentRecord rec;
        seqan::readRecord(rec, bam);
        seqan::close(bam);
    }
    if (unmatched_out.size() != 0 && !test_open(unmatched_out, 1)) {
        cerr << endl << "  ERROR: failed to open " << unmatched_out << endl;
        return 1;
    }
    if (transcriptome_files.size() != 0) {
        for (uint i = 0; i < transcriptome_files.size(); ++i) {
            if (!test_open(transcriptome_files[i])) {
                cerr << endl << "  ERROR: failed to open ";
                cerr << transcriptome_files[i] << endl;
                return 1;
            }
        }
    }
    if (kallisto_ec.size() != 0 && !test_open(kallisto_ec)) {
        cerr << endl << "  ERROR: failed to open " << kallisto_ec << endl;
        return 1;
    }
    if (!test_open(out_name + ".ec", 1)) {
        cerr << endl << "  ERROR: failed to open " << out_name << ".ec" << endl;
        return 1;
    }
    if (!test_open(out_name + ".tsv", 1)) {
        cerr << endl << "  ERROR: failed to open " << out_name << ".tsv" << endl;
        return 1;
    }
    if (!test_open(out_name + ".cells", 1)) {
        cerr << endl << "  ERROR: failed to open " << out_name << ".cells";
        cerr << endl;
        return 1;
    }
    cout << "success!" << endl;


    /* Start reading files and filling in TCC matrix */
    unordered_map<string, vector<Exon>*> *exons
            = new unordered_map<string, vector<Exon>*>;
    TCC_Matrix *matrix = new TCC_Matrix(sam_files.size());
    
    if (!rapmap) {
        num_transcripts = readGFFs(gtf_files, transcriptome_files,
                *exons, verbose);
        if (num_transcripts == -1) {
            /* No need to print an error message--readGTFs does it for us. */
            return 1;
        }
    }
    
    cout << "Reading SAMs..." << endl;
    for (int i = 0; i < sam_files.size(); ++i) {
        readSAM(sam_files[i], i, *exons, *matrix, unmatched_out,
                verbose, threads, rapmap, paired);
    }
    cout << "  done" << endl;
    
    /* Write to output files */
    cout << "Writing to file... " << flush;
    if (kallisto_ec.size() == 0) {
        if (full) {
            err = matrix->write_to_file(out_name, num_transcripts);
        } else {
            err = matrix->write_to_file_sparse(out_name, num_transcripts);
        }
    }
    else {
        vector<string> *kallisto_order = new vector<string>;
        set<string> *kallisto_ecs = new set<string>;
        err = get_kallisto_ec_order(kallisto_ec, *kallisto_order,
                                    *kallisto_ecs);
        // Only continue if previous function was successful.
        if (err != 1) {
            if (full) {
                err = matrix->write_to_file_in_order(out_name,
                                            *kallisto_order, *kallisto_ecs);
            } else {
                err = matrix->write_to_file_in_order_sparse(out_name,
                                            *kallisto_order, *kallisto_ecs);
            }
        }
        delete kallisto_order;
        delete kallisto_ecs;
    }
    // Write .cells file.
    ofstream cells(out_name + ".cells");
    err = !cells.is_open();
    if (!err) {
        for (int i = 0; i < sam_files.size(); ++i) {
            if (sam_files[i].substr(sam_files[i].size() - 4, 4).compare(".sam")
                    == 0 || sam_files[i].substr(sam_files[i].size() - 4, 4)
                    .compare(".bam") == 0) {
                sam_files[i] = sam_files[i].substr(0, sam_files[i].size() - 4);
            }
            cells << sam_files[i] << endl;
        }
        cells.close();
    }
    if (err == 1) {
        cerr << "  ERROR: Failed to open outfile(s) of name " << out_name << endl;
    }
    cout << "  done" << endl;



    /* Clean-up */
    for (unordered_map<string, vector<Exon>*>::iterator it = exons->begin();
            it != exons->end(); ++it) {
        delete it->second;
    }
    delete exons;
    delete matrix;


    /* Print total runtime of program and number of unmatched reads */
    cout << "Time: ";
    time_t diff = time(0) - start;
    string s = to_string((int)(diff / 3600));
    if (s.size() == 1) {
        cout << "0";
    }
    cout << s << ":";
    s = to_string((int)((diff % 3600) / 60));
    if (s.size() == 1) {
        cout << "0";
    }
    cout << s << ":";
    s = to_string((int)((diff % 3600) % 60));
    if (s.size() == 1) {
        cout << "0";
    }
    cout << s << endl;

    return 0;
}
